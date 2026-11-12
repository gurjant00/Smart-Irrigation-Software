"""Local Smart Irrigation dashboard server and ESP32 JSON API."""

from __future__ import annotations

import json
import os
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlencode, urlparse
from urllib.request import Request, urlopen
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent
HOST = os.environ.get("IRRIGATION_HOST", "0.0.0.0")
PORT = int(os.environ.get("IRRIGATION_PORT", "8000"))
STALE_AFTER_SECONDS = 90
FORECAST_REFRESH_SECONDS = 15 * 60
WEATHER_LATITUDE = float(os.environ.get("IRRIGATION_LATITUDE", "30.75571"))
WEATHER_LONGITUDE = float(os.environ.get("IRRIGATION_LONGITUDE", "76.82408"))
lock = threading.Lock()
forecast_lock = threading.Lock()
started_at = time.time()
last_forecast_attempt = 0.0
forecast = {"rain_probability": None, "expected_rainfall": None, "weather_source": None}
pump_control = {"mode": "auto", "on": False}
forecast_error = None
forecast_updated_at = None

sample = {
    "soil_moisture": 34,
    "soil_raw": None,
    "temperature": 26.4,
    "humidity": 68,
    "rain_probability": None,
    "expected_rainfall": None,
    "pump_on": False,
}
state = {**sample, "updated_at": datetime.now(timezone.utc).isoformat(), "source": "sample"}
history = [
    {"time": (i * 3), "value": v}
    for i, v in enumerate([48, 46, 44, 43, 41, 40, 38, 37, 36, 34])
]


def refresh_forecast() -> None:
    """Start a non-blocking forecast refresh when the cache is stale."""
    global last_forecast_attempt
    now = time.time()
    refresh_interval = FORECAST_REFRESH_SECONDS if forecast["weather_source"] else 60
    if now - last_forecast_attempt < refresh_interval:
        return
    if not forecast_lock.acquire(blocking=False):
        return
    now = time.time()
    refresh_interval = FORECAST_REFRESH_SECONDS if forecast["weather_source"] else 60
    if now - last_forecast_attempt < refresh_interval:
        forecast_lock.release()
        return
    last_forecast_attempt = now
    threading.Thread(target=_fetch_forecast, daemon=True, name="weather-forecast").start()


def _fetch_forecast() -> None:
    """Load six hourly rain values without delaying sensor API requests."""
    global forecast, forecast_error, forecast_updated_at
    try:
        query = urlencode({
            "latitude": WEATHER_LATITUDE,
            "longitude": WEATHER_LONGITUDE,
            "hourly": "precipitation_probability,precipitation",
            "forecast_days": 2,
            "timezone": "Asia/Kolkata",
        })
        request = Request(
            f"https://api.open-meteo.com/v1/forecast?{query}",
            headers={"User-Agent": "Fieldnote-Smart-Irrigation/1.0"},
        )
        with urlopen(request, timeout=8) as response:
            weather = json.loads(response.read())
        hourly = weather["hourly"]
        times = hourly["time"]
        probabilities = hourly["precipitation_probability"]
        rainfall = hourly["precipitation"]
        local_now = datetime.now(ZoneInfo("Asia/Kolkata"))
        current_hour = local_now.replace(minute=0, second=0, microsecond=0).strftime("%Y-%m-%dT%H:%M")
        start = next((i for i, value in enumerate(times) if value >= current_hour), None)
        if start is None:
            raise ValueError("Forecast has no upcoming hourly values")
        end = min(start + 6, len(times))
        next_probabilities = [float(v) for v in probabilities[start:end] if v is not None]
        next_rainfall = [float(v) for v in rainfall[start:end] if v is not None]
        if not next_probabilities or not next_rainfall:
            raise ValueError("Forecast returned no rain values")
        with lock:
            forecast = {
                "rain_probability": max(next_probabilities),
                "expected_rainfall": round(sum(next_rainfall), 1),
                "weather_source": "Open-Meteo · next 6 hours",
            }
            forecast_error = None
            forecast_updated_at = datetime.now(timezone.utc).isoformat()
    except Exception as exc:
        with lock:
            forecast_error = str(exc)
        print(f"Weather forecast unavailable: {exc}", flush=True)
    finally:
        forecast_lock.release()


def snapshot() -> dict:
    refresh_forecast()
    with lock:
        data = dict(state)
        current_forecast = dict(forecast)
        points = list(history)
    for key in ("rain_probability", "expected_rainfall"):
        if data.get(key) is None and current_forecast.get(key) is not None:
            data[key] = current_forecast[key]
    data["weather_source"] = current_forecast["weather_source"]
    age = time.time() - datetime.fromisoformat(data["updated_at"].replace("Z", "+00:00")).timestamp()
    data["online"] = data["source"] == "esp32" and age <= STALE_AFTER_SECONDS
    with lock:
        data["pump_mode"] = pump_control["mode"]
        data["pump_command"] = pump_control["on"]
    data["mode"] = "live" if data["online"] else "sample"
    data["history"] = points
    data["server_uptime_seconds"] = int(time.time() - started_at)
    return data


class Handler(BaseHTTPRequestHandler):
    server_version = "IrrigationDashboard/1.0"

    def _json(self, status: int, value: dict) -> None:
        body = json.dumps(value).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self) -> None:  # noqa: N802
        self._json(204, {})

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path in ("/api", "/api/status", "/api/readings"):
            self._json(200, snapshot())
            return
        if path == "/api/health":
            with lock:
                weather_status = {
                    "weather_source": forecast["weather_source"],
                    "forecast_updated_at": forecast_updated_at,
                    "forecast_error": forecast_error,
                }
            self._json(200, {"ok": True, "service": "smart-irrigation-dashboard", **weather_status})
            return
        if path == "/" or path == "/index.html":
            body = (ROOT / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self._json(404, {"error": "Not found"})

    def do_POST(self) -> None:  # noqa: N802
        global state
        path = urlparse(self.path).path
        if path == "/api/pump":
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > 1024:
                    raise ValueError("Request body must be between 1 and 1024 bytes")
                payload = json.loads(self.rfile.read(length))
                if not isinstance(payload, dict):
                    raise ValueError("JSON body must be an object")
                mode = payload.get("mode")
                if mode == "auto":
                    with lock:
                        pump_control.update(mode="auto", on=False)
                elif mode == "manual" and isinstance(payload.get("on"), bool):
                    with lock:
                        age = time.time() - datetime.fromisoformat(state["updated_at"].replace("Z", "+00:00")).timestamp()
                        if state.get("source") != "esp32" or age > STALE_AFTER_SECONDS:
                            self._json(409, {"error": "ESP32 is offline; pump control is disabled"})
                            return
                        pump_control.update(mode="manual", on=payload["on"])
                else:
                    raise ValueError('Use {"mode":"auto"} or {"mode":"manual","on":true|false}')
                self._json(200, {"ok": True, "readings": snapshot()})
            except (ValueError, TypeError, json.JSONDecodeError) as exc:
                self._json(400, {"error": str(exc)})
            return
        if path not in ("/api/readings", "/api/esp32"):
            self._json(404, {"error": "POST readings to /api/readings"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 16_384:
                raise ValueError("Request body must be between 1 and 16384 bytes")
            payload = json.loads(self.rfile.read(length))
            if not isinstance(payload, dict):
                raise ValueError("JSON body must be an object")
            numeric = ("soil_moisture", "soil_raw", "temperature", "humidity", "rain_probability", "expected_rainfall")
            clean = {}
            for key in numeric:
                if key in payload:
                    value = float(payload[key])
                    lower, upper = (0, 4095) if key == "soil_raw" else (-100, 1000)
                    if not (lower <= value <= upper):
                        raise ValueError(f"{key} is outside the accepted range")
                    clean[key] = value
            if not any(k in clean for k in ("soil_moisture", "temperature", "humidity")):
                raise ValueError("Include at least one sensor reading")
            if "pump_on" in payload:
                clean["pump_on"] = bool(payload["pump_on"])
            if payload.get("manual_stop_pending") is True or payload.get("manual_timeout") is True:
                with lock:
                    if pump_control["mode"] == "manual" and pump_control["on"]:
                        pump_control["on"] = False
            now = datetime.now(timezone.utc).isoformat()
            with lock:
                # Once a device is reporting, don't keep presenting sample
                # weather or stale sample sensor values as if they were live.
                if state.get("source") != "esp32":
                    history.clear()
                state = {
                    "soil_moisture": clean.get("soil_moisture"),
                    "soil_raw": clean.get("soil_raw"),
                    "temperature": clean.get("temperature"),
                    "humidity": clean.get("humidity"),
                    "rain_probability": clean.get("rain_probability"),
                    "expected_rainfall": clean.get("expected_rainfall"),
                    "pump_on": clean.get("pump_on", False),
                    "updated_at": now,
                    "source": "esp32",
                }
                if "soil_moisture" in clean:
                    history.append({"time": int(time.time()), "value": clean["soil_moisture"]})
                    del history[:-60]
            self._json(200, {"ok": True, "received_at": now, "readings": snapshot()})
        except (ValueError, TypeError, json.JSONDecodeError) as exc:
            self._json(400, {"error": str(exc)})

    def log_message(self, fmt: str, *args) -> None:
        print(f"[{self.log_date_time_string()}] {fmt % args}")


if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"Smart Irrigation dashboard: http://localhost:{PORT}")
    print("On your phone, open http://<laptop-IP>:%d on the same Wi-Fi." % PORT)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping dashboard server...")
    finally:
        server.server_close()

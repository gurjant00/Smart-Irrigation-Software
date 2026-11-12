# Fieldnote Smart Irrigation

A small local dashboard for the first working version of the irrigation project. It uses only Python's standard library and a regular browser; no package installation or internet connection is needed.

## Run it on your laptop

1. Install Python 3 if it is not already installed.
2. Open PowerShell or Terminal in this folder.
3. Start the server:

   ```powershell
   python server.py
   ```

   On some Windows installations, use `py server.py` instead.
4. On the laptop, visit [http://localhost:8000](http://localhost:8000).

The dashboard starts with sample sensor readings and refreshes every five seconds. Once the ESP32 posts sensor readings, the sensor cards and chart switch to device data. Rain chance and expected rainfall come from an Open-Meteo forecast fetched by the laptop server and are cached for 15 minutes; they do not need to be sent by the ESP32. The laptop needs internet access for this forecast. The default forecast location is the same location used by the earlier ESP32 weather request (latitude `30.75571`, longitude `76.82408`).

## Open it on your phone

Connect your phone and laptop to the same Wi-Fi network. Find the laptop's local IPv4 address (on Windows, run `ipconfig` and look for **IPv4 Address** under the connected Wi-Fi adapter), then open `http://LAPTOP-IP:8000` on the phone. For example: `http://192.168.1.25:8000`.

If Windows Firewall asks, allow Python on **Private networks** so the phone can reach the laptop. Keep the server running while using the dashboard.

## Connect the ESP32 sensors

An Arduino sketch is included at [esp32/smart_irrigation_esp32/smart_irrigation_esp32.ino](esp32/smart_irrigation_esp32/smart_irrigation_esp32.ino). It follows the supplied pin map, reads a DHT11 and analog soil sensor, refreshes the OLED, posts readings every five seconds, and controls the active-low relay on GPIO 14.

1. Start the laptop server as above and keep it running.
2. Find the laptop's IPv4 address with `ipconfig`.
3. Open the sketch in Arduino IDE, select your ESP32 board and port, and edit `WIFI_SSID`, `WIFI_PASSWORD`, and `DASHBOARD_URL`. For example, if the laptop IPv4 is `192.168.1.25`, set the URL to `http://192.168.1.25:8000/api/readings`.
4. The sketch uses your pins: soil sensor GPIO 34, DHT11 data GPIO 26, OLED SDA GPIO 32 and SCL GPIO 33, relay GPIO 14. Connect sensor grounds to ESP32 ground; power sensors at 3.3 V unless the sensor module documentation says otherwise. Do not connect a 5 V signal to an ESP32 GPIO.
5. Install **DHT sensor library** by Adafruit, **Adafruit SSD1306**, **Adafruit GFX Library**, and **ArduinoJson** from Arduino IDE Library Manager (plus any prompted dependencies), then upload the sketch.
6. Open Serial Monitor at **115200 baud**. It prints the ESP32 IP, soil ADC value, and HTTP response code. HTTP 200 means the laptop accepted the reading. The dashboard should show **ESP32 online**.

The sketch uses your provided mapping exactly: `map(soilRaw, 3300, 2100, 0, 100)`, constrained to 0–100. Your code labels this calibration temporary; actual percentage accuracy depends on those endpoints matching your sensor and soil. The dashboard also shows the raw ADC value.

The relay is active-low on GPIO 14: LOW turns it on and HIGH turns it off. It starts OFF. In automatic mode, watering starts only when soil is below 20%, a forecast was received, rain chance is below 50%, and expected rainfall over the next six hours is below 1 mm. Each run is capped at 10 seconds, followed by a 10-minute soak; no more than three cycles can start in an hour.

When the ESP32 is online, press **Turn pump ON** to start the pump in manual mode. It stays on until you press **Turn pump OFF**. Manual mode bypasses soil, rain, run-time, soak, and hourly-cycle limits. The ESP32 still turns it off if the dashboard connection is lost or the board reboots, and it clears any old ON command after reconnecting. Use **Return to automatic** to leave manual mode. The command reaches the ESP32 on its next five-second update. The pump controls are disabled while the device is offline. Supervise manual watering and verify the relay wiring and motor supply before enabling the pump.

Connect the pump's power through the relay contacts and use a suitable external motor supply; never power a pump from an ESP32 GPIO. Test relay logic with the pump power disconnected first. Ensure the relay module's logic voltage and contact rating suit the ESP32 and pump.

Rain probability and expected rainfall are forecast data, not measurements from the DHT11 or soil sensor. The laptop fetches the maximum hourly precipitation probability and total expected rainfall for the next six hours from Open-Meteo. The ESP32 receives those values in the POST response and uses them for the pump decision. If the forecast service or internet connection is unavailable, the cards show `—` and automatic watering remains off. To change the forecast location, set `IRRIGATION_LATITUDE` and `IRRIGATION_LONGITUDE` before starting `server.py`.

## ESP32 API

The ESP32 can POST JSON readings to `http://LAPTOP-IP:8000/api/readings` (or `/api/esp32`). The laptop's IP address is the same one used by the phone. Send `Content-Type: application/json` and any supported fields:

```json
{
  "soil_moisture": 34,
  "soil_raw": 2500,
  "temperature": 26.4,
  "humidity": 68,
  "rain_probability": 20,
  "expected_rainfall": 0.4,
  "pump_on": false
}
```

Include at least one of `soil_moisture`, `temperature`, or `humidity`. The other supported fields are optional. `soil_raw` accepts the ESP32 ADC range 0–4095. Soil moisture, temperature, humidity, rain probability, and expected rainfall are numbers; `pump_on` is a boolean. The dashboard shows live status after a device POST and marks the ESP32 offline if there has been no update for 90 seconds. Until then, it displays sample readings. Read current state at `GET /api/readings`; `GET /api/health` checks the server.

Example from another computer on the Wi-Fi:

```powershell
Invoke-RestMethod -Method Post -Uri http://192.168.1.25:8000/api/readings `
  -ContentType 'application/json' `
  -Body '{"soil_moisture":31,"temperature":27.1,"humidity":63,"rain_probability":15,"expected_rainfall":0.0,"pump_on":false}'
```

The server keeps the latest readings, pump mode/command, and up to 60 soil readings in memory. Restarting it resets sensor readings to sample values and pump mode to automatic until the ESP32 posts again. The dashboard sends manual commands to `POST /api/pump`; the ESP32 executes them on GPIO 14 and reports the relay state in its next sensor POST. At boot and after a lost dashboard connection, the ESP32 sends a stop-pending flag so an old manual ON command cannot restart the pump unexpectedly.

## Options

Default address: `0.0.0.0:8000` (all laptop network interfaces, needed for phone access). To choose a different port:

```powershell
$env:IRRIGATION_PORT = "8080"
python server.py
```

Then visit port 8080 on both devices.

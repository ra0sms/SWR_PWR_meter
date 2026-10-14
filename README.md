# SWR_PWR_meter

SWR and power meter with Wi-Fi interface based on ESP8266 (NodeMCU 0.9).

More information (in Russian): https://ra0sms.com/swr_power_meter/

![Device](docs/1.jpg)
![Example](docs/swr_example.png)

## Features

- Measures **forward power (PEP)** and **SWR** in real time
- Web interface with smooth animated power bar and SWR display
- Captive portal for easy first-time Wi-Fi setup
- Saves Wi-Fi credentials to EEPROM (survives reboot)
- Automatic fallback to SoftAP mode when the network is lost
- Automatic reconnect to the saved network
- Configurable power range: 200 / 300 / 1000 / 2000 W
- Client-side smoothing with SSB / CW / FT8 mode selector
- Optional HTTP Basic Auth for all web pages

## Specifications

| Parameter | Value |
|---|---|
| Power supply | microUSB 5V 1A |
| Full-scale power | 1 – 1000 W (configurable up to 2000 W) |
| Frequency range | 1 – 30 MHz |
| Measurement | PEP (Peak Envelope Power) |
| Wi-Fi range | up to 50 m (entry-level router) |
| Housing | Gainta G0479 |
| Board | NodeMCU 0.9 (ESP8266) |

## Hardware

**Make sure you use the ESP8266 board v 0.9. Check the pinout!**

![Pinout](docs/nodemcu_v0.9.jpg)

![Schematic](docs/sch.jpg)

### Pin usage

| Pin | NodeMCU | Function |
|---|---|---|
| `A0` | A0 | ADC input (forward/reflected detector) |
| `switch_pin` (5) | D1 | Analog switch control (forward/reflected) |
| `led_pin` (16) | D0 | Status LED (on = not connected, off = connected) |

Gerber files for the PCB: [SWR_gerber.zip](gerber/SWR_gerber.zip)

## Building and flashing

1. Install [Arduino IDE](https://www.arduino.cc/en/software) (or use `arduino-cli`).
2. Add the ESP8266 board manager URL: `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
3. Install the **esp8266** core (tested with 3.1.2).
4. Select board **NodeMCU 0.9 (ESP8266-12E)**.
5. Open [`src/swr_source/swr_source.ino`](src/swr_source/swr_source.ino) and upload.

Required libraries (installed with the core by default):

- `ESP8266WiFi`
- `ESP8266WebServer`
- `DNSServer`
- `EEPROM`

## Ready firmware

Pre-built binaries for **NodeMCU 0.9 (ESP8266-12E)** (core 3.1.2, `MAX_POWER = 1000`):

| File | Authentication | Size |
|---|---|---|
| [`firmware/swr_source_auth.bin`](firmware/swr_source_auth.bin) | Enabled (`admin` / `swr12345`) | 336 KB |
| [`firmware/swr_source_noauth.bin`](firmware/swr_source_noauth.bin) | Disabled | 332 KB |

Flash at address `0x00000` using esptool, Arduino IDE or NodeMCU PyFlasher.

## Configuration

### Power range

Set `MAX_POWER` in [`swr_source.ino`](src/swr_source/swr_source.ino:18) to one of: `200`, `300`, `1000`, `2000`.

### Calibration

The power formula is `P = U² / K`, where `K` depends on the power range:

| MAX_POWER | K |
|---|---|
| 200 W | 5230 |
| 300 W | 3487 |
| 1000 W | 1046 |
| 2000 W | 523 |

Adjust `K` in [`GetPower()`](src/swr_source/swr_source.ino:104) to match your hardware (detector sensitivity, ADC divider).

### SoftAP credentials

The access point SSID and password are defined at the top of the sketch:

```cpp
const char *softAP_ssid = "SWRmeter_sn082";
const char *softAP_password = "1234567890";
```

Change them before flashing if needed.

### Authentication

All web pages can be protected with HTTP Basic Auth. Credentials and the enable flag are defined at the top of the sketch:

```cpp
const bool AUTH_ENABLED = false;  // Set to true to enable authentication
const char *auth_user = "admin";
const char *auth_pass = "swr12345";
```

- Set `AUTH_ENABLED = true` to protect all web pages with a login/password prompt.
- Change `auth_user` / `auth_pass` before flashing if needed.
- The browser asks for credentials once; subsequent `/xml` and `/mode` requests are authenticated automatically.

### Display smoothing mode

The `/swr` page has a **SSB / CW / FT8** mode selector. Each mode uses its own smoothing coefficients (applied in the browser, no reflash needed):

| Mode | Power rise | Decay | Use case |
|---|---|---|---|
| SSB | 0.15 | 0.5 | Default, universal |
| CW | 0.4 | 0.5 | Fast response to show PEP during keying |
| FT8 | 0.25 | 0.4 | Steady reading for constant-power digital modes |

The selected mode is kept in RAM only and resets to **SSB** after reboot (no flash wear).

## First-time setup (Wi-Fi)

1. Power the device via microUSB (5V 1A). Your router must have **DHCP enabled**.
2. On your phone/computer, connect to the Wi-Fi network **`SWRmeter_sn___`** (password `1234567890`).
3. Open `http://192.168.4.1/wifi` in a browser.
4. Enter your network name and password, click **Connect**.
5. The device connects to your network, the SoftAP turns off automatically, and the device gets an IP from your router. Use that IP to access the web interface.

> If the connection is lost later, the device automatically re-enables the SoftAP and keeps retrying the saved network every 30 seconds. During a connection attempt the web server is briefly unresponsive (up to 10 s) — this is the original behavior.

## Web interface

| URL | Description |
|---|---|
| `/` | Start page with connection info |
| `/wifi` | Wi-Fi configuration (network scan, connect form) |
| `/swr` | SWR / power meter page (with SSB/CW/FT8 mode selector) |
| `/xml` | Raw data endpoint (`<pwr>`, `<swr>`, `<adcpower>`) — used by the meter page |
| `/mode?m=ssb\|cw\|ft8` | Set display smoothing mode (used by the mode selector) |

All pages require authentication (if `AUTH_ENABLED = true`). The `/xml` endpoint can be used to integrate readings into other systems (e.g., home automation).

## Changelog

- **2019** — First version
- **2021-03-12** — Added selectable max power (2000/1000/200 W) via `#define`
- **2025-05-22** — New CSS-based web interface
- **2026-06-24** — Code cleanup: removed unused vars/includes, `constexpr`, `millis()` timer
- **2026-10-13** — Reliability & security improvements:
  - Reconnect on connection loss (`WL_CONNECTION_LOST` / `WL_DISCONNECTED`)
  - `no-cache` headers for `/xml` and `/swr`
  - SWR clamped at low power, fixed unsigned underflow in SWR calculation
  - Removed dead code, merged `flagOn`/`isWiFiConnected`
  - Larger SSID/password buffers (32/63 chars), input validation with error display
  - Compact dark redesign of the `/swr` page
  - SSB/CW/FT8 display smoothing mode selector (RAM only, no flash wear)
  - HTTP Basic Auth for all web pages (`AUTH_ENABLED` flag, configurable login/password)
- **2026-10-14** — Wi-Fi fixes:
  - Restored blocking `waitForConnectResult()` in `connectWifi()` — fixes first-connect failure (`WL_WRONG_PASSWORD`) after entering credentials
  - Retry interval 30 s, no retries on wrong password

> **Note:** the buffer size change shifts the EEPROM layout — saved Wi-Fi credentials are cleared once after this update. Re-enter them via the web interface.

## License

See [LICENSE](LICENSE).

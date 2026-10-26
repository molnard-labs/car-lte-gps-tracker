# Car LTE GPS Tracker

A working Wi-Fi proof of concept for an ESP32/SIM7600 car tracker. The board sends real GNSS positions to a self-hosted Traccar Server over protected HTTPS; Home Assistant displays the car through its Traccar Server integration.

**Verified on 2026-10-26:** a real SIM-free GNSS fix, Wi-Fi HTTPS upload with HTTP 200, stored position for `pablo` in Traccar, and `device_tracker.pablo` in Home Assistant. LTE and parked power management are implemented but still need hardware qualification. See [PoC status and remaining work](docs/poc-status.md).

## Architecture

```text
SIM7600 GNSS → ESP32 → Wi-Fi / SIM7600 LTE (PPP + CMUX)
                         ↓ HTTPS OsmAnd POST + device credentials
             https://tracker.davidmolnar.dev/ingest
                         ↓ Cloudflare Access + Tunnel
              Traccar HA app → dedicated MariaDB database
                         ↓ Traccar Server integration
                   Home Assistant map
```

Traccar provides route history, trips/stops and geofences. Home Assistant's existing Recorder remains independent. Server installation is managed in the separate homelab project; the [server and HA guide](docs/setup/homeassistant-setup.md) documents the interface and configuration required by this firmware.

## Hardware

The owned board is a **LILYGO T-SIM7600X T54-V1.3 / ESP32-WROVER-E**, with **4 MB flash**, **8 MB physical PSRAM tested at 40 MHz**, and a **SIM7600E-H** modem. The printed G-H label is inconsistent with its corroborated identity. The firmware targets the original `esp32`, DIO flash at 40 MHz and PSRAM at 40 MHz.

Use the USB-C socket marked **TTL** for ESP32 programming. Connect the GNSS antenna to **GPS**; **MAIN/AUX** are cellular antenna connections. Wi-Fi commissioning requires no SIM. See the [board specification](hardware/board-specification.md), [hardware diagnostics](hardware/diagnostics-2026-10-26.md) and [wiring guide](docs/wiring/wiring-guide.md).

## Firmware

- Primary and secondary Wi-Fi profiles, with a separate Wi-Fi save button and current SSID/IP status. Either working network stays connected; failed rounds use bounded attempts and pauses.
- English/Hungarian setup page on the temporary WPA2 `CarTracker-<suffix>` AP at `http://192.168.4.1`, and on the board's assigned home Wi-Fi address while awake. Web login is optional and disabled by default; the AP has its own unique password.
- Passive live status: viewing or editing unsaved settings does not pause tracking. Validated saves pause only while settings are applied. Errors appear beside the relevant save button.
- GPS diagnostics show receiver state, response age, query/error counters and reported satellite counts/HDOP. Missing satellite information remains unknown; reported counts describe satellites used for positioning.
- Default reporting: **30 seconds** while active, another **10 minutes** after ignition turns off, then a heartbeat every **30 minutes**. Bench mode defaults on and keeps the board awake.
- Protected active-low ignition input on **GPIO13**, using its internal RTC pull-up and ext0 wake. Vehicle wiring and complete-system sleep current still require measurement.
- Persistent **4096-position / 512 KiB** offline queue, latest-position priority and chronological backfill. Real fixes retain their original timestamps; no-fix heartbeats preserve the last measured location.
- **GPIO23 LED/relay output**, controlled through Traccar custom commands and `switch.pablo_output` in HA, with actual pin-state feedback. See [remote output setup](docs/setup/remote-output.md).
- NVS configuration with atomic save/rollback, persisted SIM PIN retry lock and no compiled secrets. One modem driver owns UART for GNSS and PPP/CMUX.

MQTT, OTA and battery percentage/source estimates are outside this PoC. The GPIO output starts OFF on reset and retains its level through normal deep sleep. The 150-second parked work plus 30-second shutdown budget is a target: failed shutdown can keep the board awake. Permanent vehicle installation requires protected power and measured consumption.

## Build and setup

Use **ESP-IDF v5.5.6**, target `esp32`; `esp_modem` is pinned to **1.4.1**.

```sh
cd firmware
idf.py set-target esp32
idf.py build
```

Follow the [firmware setup guide](docs/setup/firmware-setup.md) for first installation, unique setup-password provisioning, configuration and application-only updates. The existing deployed board is already provisioned; ordinary updates preserve its configuration and queue.

The [technical specification](docs/technical-specification.md) defines the current behavior. The [acceptance checklist](docs/acceptance-tests.md) records tested boundaries and remaining LTE, recovery and vehicle tests.

## Development

```sh
cd firmware/test
make
```

The **nine active C suites** cover configuration/GNSS, AT response collection, storage, scheduling, network policy, setup and output control. On Windows, `tools/ci-local.ps1` also runs JavaScript/UI and Python provisioning tests plus portable static analysis. GitHub Actions performs these checks and an ESP-IDF build. See [contributing](CONTRIBUTING.md).

Keep credentials, raw flash images and private position history outside Git. `tools/provisioning-private/` is ignored. The standalone [RAM-only PSRAM diagnostic](tools/psram-ram-test/README.md) is retained as reproducible hardware evidence and uses its own documented toolchain.

## License

[MIT](LICENSE).

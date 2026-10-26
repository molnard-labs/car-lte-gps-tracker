# Wi-Fi PoC status

Verified on **2026-10-26** on the owned LILYGO T-SIM7600X / SIM7600E-H board. This is the current baseline; the [technical specification](technical-specification.md) defines behavior and the [acceptance checklist](acceptance-tests.md) covers further qualification.

## Verified end to end

```text
Real SIM7600 GNSS fix → ESP32 Wi-Fi → HTTPS + Cloudflare Access/Tunnel
                    → Traccar + MariaDB → Home Assistant
```

- Device identifier: **`pablo`**; Home Assistant entity: **`device_tracker.pablo`**.
- Endpoint: **`https://tracker.davidmolnar.dev/ingest`**.
- First confirmed measured fix: **2026-10-26 12:09:34 UTC** (13:09:34 Europe/Budapest).
- HTTP **200**, valid position stored in Traccar and device online; HA entity populated with coordinates and `mode`, `network`, `gpsFix`, `fix_time`, `reported_at` attributes.
- At that observation: 2D fix, 3 GPS + 2 GLONASS + 1 BeiDou satellites used, HDOP 3, zero GNSS command/data errors. Counts and fix quality vary with reception.
- Existing configuration and device credentials survived application updates. No synthetic position was sent as the real vehicle.

## Board and local UI

The board has 4 MB flash; all 8 MiB physical PSRAM passed six patterns at 40 MHz. The firmware uses DIO flash and PSRAM at 40 MHz, ESP-IDF **5.5.6** and `esp_modem` **1.4.1**. The modem firmware is `LE11B14SIM7600M22_211104`.

Phone/AP access, router connection, DHCP, network scanning and the LAN page were exercised. Tests covered separate Wi-Fi saving, redacted configuration, invalid saves, full-save readiness, AP closure while retaining LAN access, configuration preservation and passive status reads. A 315-second passive observation completed 53 reads without pausing tracking. Real fix/upload observation also showed `tracking_paused=false`.

The GPS card reports receiver power, antenna-supply setting, reply age, search duration, counters, satellite counts and HDOP where supplied. Power acknowledgment is not a physical antenna continuity test; unknown counts are not zero. The AT collector replaces cumulative `esp_modem` snapshots. Its inflatable-buffer setting ensures the same contract on UART and CMUX; fragmented-response regression tests cover both GNSS parsers.

## Server baseline

The separate homelab deployment uses Traccar HA app **0.27.0 / Traccar 6.16.0**, MariaDB app **3.0.1**, a dedicated Traccar database/user and the native Traccar Server HA integration. The existing HA Recorder is independent. The Traccar web UI remains local; only `/ingest` is exposed through Cloudflare Tunnel and device-specific Service Auth. The deployed device service token has no expiry; its values stay private.

Server commissioning verified unauthorized/unknown-device rejection, exact route restriction, route/trip/stop reports with isolated test data, no-fix handling, backfill ordering, database failure without fallback, and backup/isolated restore. Timestamp columns retain milliseconds. Retention is 365 days, with an hourly bounded cleanup and daily 03:15 Europe/Budapest backup. This repository contains firmware and interface documentation; server operational scripts and credentials remain in the homelab project.

## Remaining qualification

- LTE with the actual SIM/APN/PIN, registration, PPP/CMUX, simultaneous GNSS and automatic Wi-Fi/LTE transitions.
- A real drive, route replay/trip/stop reports for that drive, prolonged reconnection and queued-position backfill.
- Physical interrupted writes, certificate/DNS/server failures and extended configuration/authentication stress on the board.
- Optional browser password and secondary-network behavior under deliberate hardware failure scenarios.
- Protected ignition wiring, 10-minute grace, 30-minute wake cadence, 100 sleep/wake cycles and a 24-hour mixed-operation run.
- Complete-system active/parked consumption, converter losses, brownout recovery and independent starter-battery protection. Shutdown faults can exceed the 150+30-second target.

## Evidence and updates

The working PoC application before repository cleanup was **1,303,184 bytes**, SHA256 `82fe6ebdeaafd2357a651cc70ae9ce300d58b44d38e6fc242f74a01c8066a07e`. This identifies the measured image; rebuilding metadata can change a binary hash without changing behavior. Cleanup removes unused modules and documentation; it does not require reflashing the already working board.

Factory/application recovery images, credentials and raw position evidence are private and excluded from Git. Retain them locally for recovery. Public hardware evidence is in [the board report](../hardware/diagnostics-2026-10-26.md).

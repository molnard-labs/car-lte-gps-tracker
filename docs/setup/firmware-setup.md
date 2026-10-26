# Firmware build, provisioning and configuration

The Wi-Fi proof of concept is working on the owned ESP32/SIM7600 board. See [current PoC status](../poc-status.md). These instructions cover a fresh installation and routine updates; the deployed board already has the correct layout and its unique setup password.

## Build

Use an exported **ESP-IDF v5.5.6** environment, target `esp32`:

```sh
cd firmware
idf.py set-target esp32
idf.py build
```

Flash is **4 MB**, DIO at **40 MHz**; PSRAM runs at **40 MHz**. The managed `esp_modem` dependency is pinned to **1.4.1**. `CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED=y` is required for consistent accumulated AT response callbacks on UART and CMUX.

Connect through USB-C **TTL**, nearest the ESP32, using a reliable regulated 5 V supply/data cable. **COM9** was the Windows port used in the PoC; substitute the actual port. The other USB-C socket is the modem's direct USB connection. ESP32 flashing does not update modem firmware.

## Existing board: application-only update

Preserve its private recovery/provisioning bundle and current configuration. From `firmware/`, after building:

```sh
idf.py -p COM9 app-flash
```

This updates the application while preserving the bootloader, partition table, factory/mutable NVS and queued positions. Do not run first-install erasures or generate a replacement setup password for the existing board.

The commands below use esptool's underscore syntax available in the pinned IDF environment. A separate esptool 5 installation may also document hyphenated spellings; use the interpreter belonging to the selected IDF toolchain.

## Fresh board: provisioning and first flash

Preserve and verify a full factory flash recovery copy before replacing its original application. Never commit raw flash images. The hardware identity/backup method is recorded in [diagnostics](../../hardware/diagnostics-2026-10-26.md).

Generate a unique setup passphrase from the repository root:

```sh
python tools/provision_tracker.py --device-id car --idf-path "$IDF_PATH"
```

PowerShell:

```powershell
python tools/provision_tracker.py --device-id car --idf-path $env:IDF_PATH
```

Here `car` is only the local provisioning-bundle label; the Traccar ID is configured separately as `pablo`. The tool creates `tools/provisioning-private/car/factory.csv`, `factory.bin` and `setup-password.txt`. It refuses to overwrite an existing bundle. An optional `--idf-python` selects the generator's Python interpreter. Missing provisioning never falls back to a shared/default AP password.

The current partition layout is:

| Partition | Offset | Size |
|---|---:|---:|
| Mutable NVS | `0x9000` | `0x10000` |
| PHY | `0x19000` | `0x1000` |
| Factory provisioning NVS, read-only to firmware | `0x1a000` | `0x10000` |
| Application | `0x30000` | `0x350000` |
| Position queue | `0x380000` | `0x80000` |

**Only when moving a fresh/factory board to this layout**, after verifying the recovery copy, deliberately initialize the mutable NVS and queue regions:

```sh
python -m esptool --chip esp32 --port COM9 --after no_reset erase_region 0x9000 0x10000
python -m esptool --chip esp32 --port COM9 --after no_reset erase_region 0x380000 0x80000
```

These commands erase settings/history in the named regions. Skip them for an already provisioned tracker. The firmware never erases NVS automatically on a storage error.

From `firmware/`, flash the initial bootloader, partition table and application:

```sh
idf.py -p COM9 flash
```

From the repository root, flash the device's own 64 KiB factory image:

```sh
python -m esptool --chip esp32 --port COM9 write_flash 0x1a000 tools/provisioning-private/car/factory.bin
```

Never flash another device's provisioning image. Monitor with `idf.py -p COM9 monitor` from `firmware/`; exit with Ctrl+]. Keep identifiers, credentials and precise location logs private.

## Wi-Fi setup and LAN access

A power-on/RST opens the temporary WPA2 network **`CarTracker-<suffix>`**. Read its unique passphrase locally from `setup-password.txt`, connect and open **`http://192.168.4.1`**, explicitly HTTP. Keep the phone connected when it reports that this network has no Internet. Ordinary software/watchdog restarts and timer/ignition deep-sleep wakes do not open the setup AP.

Configure primary and optional secondary Wi-Fi profiles. The separate **Wi-Fi settings save** button saves only their SSID/password pairs; other edited fields remain unsaved. Each attempt lasts ten seconds, with three seconds between profiles and fifteen seconds between exhausted offline rounds. Either working network remains connected. Five-minute retries apply only while LTE is working.

The top banner shows the actual connected profile/SSID, DHCP address and RSSI. Connect the phone/computer to the same router and open the displayed address, for example **`http://192.168.88.68`**. This listener remains available after the AP closes, while the board is awake and connected. There is no LTE configuration listener. DHCP can change the address.

The AP initially lasts five minutes, then uses a five-minute inactivity and twenty-minute maximum window after a client joins. Passive page/config/status reads do not extend its lifetime. Saving does not close it automatically; **Close without saving** closes an open AP after the response.

## Tracking configuration

For the working Wi-Fi PoC use:

| Setting | Value |
|---|---|
| Network mode | `wifi_only` |
| Bench mode | On; keep the board awake |
| HTTPS endpoint | `https://tracker.davidmolnar.dev/ingest` |
| Traccar ID | `pablo`, already registered on the server |
| Access credentials | Dedicated device Client ID and Client Secret |
| Active / grace / parked intervals | `30` / `600` / `1800` seconds |

Set up the server using [the Traccar/HA guide](homeassistant-setup.md). All secrets stay in NVS and are never returned by GET. **Saved** beside an empty secret field indicates a stored value; leave it empty to preserve it, or use its explicit clear control. Full save accepts valid incomplete configuration but lists missing upload requirements and leaves uploads disabled until ready. Validation errors appear beside the clicked save button and retain draft values.

**Wi-Fi AP password** controls joining the board's wireless network. **Web interface password** is independent: empty by default, or 8–63 characters with username **admin** to enable HTTP Basic login on both local listeners. Clearing the AP override restores its unique factory password; clearing the web password removes browser login.

Configuration is versioned and saved atomically with one previous revision. v1/v2 records migrate to v3 while preserving credentials/history; neither a migration error nor an NVS error causes an automatic erase. Only validated save/restore/PIN-retry operations briefly pause tracking while applying changes. Viewing status, typing unsaved values and scanning do not pause it or keep a parked board awake.

## GPS and status

Connect the antenna to **GPS**, with a useful sky view; **AUX** is cellular diversity. The modem runs GNSS without a SIM. The GPS card shows startup/search/fix/error, last reply age, query/error counters, search duration and power readback. A power acknowledgment does not test antenna continuity. Satellite counts from `CGNSSINFO` describe satellites used; missing fields remain unknown. HDOP is not meter accuracy.

Status refreshes every five seconds, including after closing/saving the editor. Failed/stale reads become **Unknown** instead of retaining an old Yes/No. Read requests have a ten-second timeout. Authentication renewal preserves drafts; an uncertain save is not replayed automatically. The AP and awake windows remain independent of browser authentication.

Verify a real position, a successful HTTP result, storage in Traccar and the HA entity. The current board has passed this gate. Before its first accepted real fix the firmware sends no coordinate-free heartbeat; afterward a no-fix heartbeat preserves the last measured coordinates and fix time.

## Optional local credential helper

`tools/configure_tracker_access.py` can load the two device credentials from a local private JSON file with `clientId` / `clientSecret` keys. Run from the repository root, substituting the actual file/address:

```sh
python tools/configure_tracker_access.py --board-url http://192.168.88.68 --secrets-file /private/firmware-provision.json --client-id-key clientId --client-secret-key clientSecret --expected-device-id pablo --expected-endpoint https://tracker.davidmolnar.dev/ingest
```

This is read-only by default. Add `--apply` after server authentication has been verified. It matches the existing endpoint/device identity, updates only those credentials/endpoint, reads back presence/readiness and never prints secrets. Readback cannot compare plaintext values. An uncertain response is not automatically retried. The helper supports password-free local HTTP; use the browser if optional web login is enabled.

## Next commissioning stages

Fit the correct LTE antenna and compatible active SIM, configure the carrier APN and verified SIM PIN, then test cellular-only and automatic modes. A failed/uncertain PIN attempt is persistently blocked across reset; correct it and explicitly request one retry rather than repeatedly guessing.

Keep bench mode enabled until protected GPIO13 ignition wiring and sleep/wake behavior are tested. Qualify ten-minute grace, thirty-minute heartbeat cycles and complete-board current. The 150-second work plus 30-second shutdown target is not a strict fault bound: uncertain shutdown refuses sleep and can keep the board awake. See [wiring](../wiring/wiring-guide.md) and [acceptance tests](../acceptance-tests.md).

## Local checks

`make -C firmware/test` runs eight active C suites. `tools/ci-local.ps1` additionally checks portable C, embedded JavaScript/UI behavior and both Python provisioning tools. These complement the ESP-IDF build and hardware checks; they do not replace LTE/power qualification.

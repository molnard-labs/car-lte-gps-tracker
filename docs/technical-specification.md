# Car position tracker — technical specification

Version 3, 2026-10-26. This is the current design for the working Wi-Fi PoC, including passive LAN configuration/status and schema v3. Real GNSS → protected HTTPS → Traccar → Home Assistant is verified; LTE and vehicle power behavior still need hardware qualification. See [PoC status](poc-status.md) for evidence and [acceptance tests](acceptance-tests.md) for remaining gates.

## Outcome and boundaries

The owner's car reports its real GNSS position to the owner's homelab, where Traccar keeps position history and Home Assistant shows the car on a map. Commission the owned board over home Wi-Fi before introducing a SIM or permanent vehicle wiring. Both stages use the same HTTPS endpoint and payload.

This version includes parked operation, a temporary setup AP and local Wi-Fi LAN configuration page, durable offline positions and explicit fix/receipt timestamps. It includes one generic GPIO23 LED/relay-board output with remote command/state feedback. It excludes vehicle immobilization, OTA firmware updates, phone location substitution, battery percentage estimates and unverified supply-source detection. The car marker represents the last measured position; a heartbeat never pretends an old fix is newly measured.

## Hardware and firmware architecture

Use the diagnosed LILYGO T-SIM7600X T54-V1.3, ESP32-WROVER-E with 4 MB flash and SIM7600E-H. Flash and PSRAM run at 40 MHz. The 8 MB physical PSRAM test does not make all 8 MB directly allocatable; this application does not require upper-memory bank switching. The photographed G-H label does not override the corroborated E-H identity. See [hardware evidence](../hardware/board-specification.md) and [diagnostics](../hardware/diagnostics-2026-10-26.md).

Pin ESP-IDF to **v5.5.6**, with managed dependencies locked by the firmware build. Components have these responsibilities:

| Component | Responsibility |
|---|---|
| `tracker_core` | Typed configuration, validation, strict GNSS parsing and OsmAnd serialization |
| `tracker_store` | Atomic current/previous configuration, SIM PIN latch, last fix and durable position queue |
| `tracker_net` | Sole modem/UART ownership, GNSS polling, Wi-Fi, PPP/CMUX, DNS, time and HTTPS |
| `tracker_setup` | Separate SoftAP/STA HTTP listeners, local configuration, credential handling and status display |
| `tracker_output` | Fixed GPIO23, bounded ON/OFF command reception, pad readback and deep-sleep retention |
| Main scheduler | Ignition debounce, driving/grace/parked states, retry/backfill policy and deep-sleep admission |

Reserve modem UART TX/RX GPIO27/26, PWRKEY4, flight control25 and DTR32. GPIO26 is never initialized as a relay output. GPIO13 is the ignition input: a protected external open-collector interface pulls it low when ignition is on. Use the ESP32 RTC pull-up and level-low ext0 wake; leave SD/JTAG disabled. Vehicle voltage must never reach the GPIO directly. The board needs continuous regulated 5 V to operate while parked; an automotive power converter and low-voltage protection require installation qualification.

The flash layout is: mutable NVS at `0x9000`, 64 KiB; PHY at `0x19000`, 4 KiB; read-only factory provisioning NVS at `0x1a000`, 64 KiB; application at `0x30000`, size `0x350000`; position queue at `0x380000`, 512 KiB. There are no OTA application slots.

## Runtime and position contract

Default mode is `wifi_only`, with bench mode enabled. Bench mode keeps the firmware awake for testing. `auto` prefers working Wi-Fi and falls back to LTE; `cellular_only` forces the cellular path. Modem/GNSS startup must not depend on SIM readiness when using Wi-Fi. Cellular uses `esp_modem` SIM7600 support with PPP plus CMUX, allowing GNSS AT commands and IP traffic to coexist on the same UART. The exact modem firmware must pass CMUX qualification.

| State | Reporting and transition |
|---|---|
| Active | Ignition on, or bench mode; report immediately and then every 30 seconds |
| Parking grace | Ignition off; continue 30-second reporting for 600 seconds; ignition immediately returns to active |
| Parked | Wake every 1800 seconds, start-to-start; acquire a fix and send one current report before sleeping |

The parked GNSS allowance is 90 seconds inside a 150-second work window, with a target of another 30 seconds for orderly network/modem shutdown. Ignition takes priority during every phase. The bounded setup AP window and an in-progress settings application inhibit deep sleep. Reading the LAN page, polling status or editing unsaved fields does not. Sleep admission must confirm modem/PPP shutdown, disable GNSS and antenna power, request modem sleep, and recheck ignition immediately before sleeping. Measured board current decides whether the installation meets its power budget; ESP32 deep sleep alone is not proof that the modem and board peripherals are asleep.

**Current limitation:** the 150+30-second budget is not a strict bound under faults. Unconfirmed modem state, a failed PPP/CMUX transition or blocked HTTP cleanup causes the firmware to refuse sleep and retry; recovery can exceed the budget. No unqualified emergency modem power-cutoff fallback is implemented. Shutdown failure behavior and complete-board current must be measured before unattended vehicle use.

The modem callback collector replaces accumulated response snapshots; `CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED=y` makes UART and CMUX follow that same contract. GNSS parsing validates the complete AT response, coordinate hemispheres/ranges, UTC date/time and finite numeric values. Convert modem speed from knots to km/h. Optional altitude, speed and course are included only when available. Do not invent satellite counts, HDOP or meter accuracy from `CGPSINFO`. Reject malformed fixes rather than substituting zero coordinates.

The local GPS card reports response age, search duration, polling/error counters, receiver/antenna-power readback, and `CGNSSINFO` fix mode, used satellite counts and HDOP where supplied. Unknown values remain unknown and stale values are invalidated; status requests never issue AT commands. These diagnostic fields are not added to the upload payload.

POST to the configured endpoint, `https://tracker.davidmolnar.dev/ingest`, using `Content-Type: application/x-www-form-urlencoded` and Cloudflare Access `CF-Access-Client-Id` / `CF-Access-Client-Secret` headers. Require certificate-chain and hostname verification, a valid clock, bounded request time and no automatic redirects. Use the same HTTP implementation for Wi-Fi and LTE.

| Field | Meaning |
|---|---|
| `id` | Configured Traccar identifier, URL-encoded; no IMEI is required |
| `timestamp` | Original UTC fix timestamp in milliseconds for a real fix; report time for an explicit no-fix heartbeat |
| `valid`, `gpsFix` | True for a newly measured valid fix; false for a no-fix heartbeat |
| `lat`, `lon` | Decimal degrees, present only for a real fix |
| `speed`, `altitude`, `bearing` | Optional km/h, meters and degrees; Traccar OsmAnd speed units must be configured to km/h |
| `fix_time`, `reported_at` | Original fix time and sample/report creation time, both epoch milliseconds |
| `sample_id` | Stable sample identity for retries; sequence for durable positions |
| `ignition`, `mode`, `network` | Actual ignition, `active`/`parking_grace`/`parked`, and `wifi`/`lte`/`none` |

Before any real position has been commissioned, no-fix must not create a car at `(0,0)`. Once a real position is accepted, a no-fix heartbeat can identify the last fix without supplying replacement coordinates. Its `fix_time` must remain the original measurement time. Server and HA acceptance tests must prove this behavior.

Retain up to **4096** valid positions in the 512 KiB flash queue. Queue data and metadata are crash-recoverable; acknowledgements occur only after a successful HTTPS result. Full capacity evicts exactly the oldest pending position and increments a persistent dropped counter. On reconnection, send the newest live position first, then backfill chronologically at no more than two requests per second. Preserve fix/sample times throughout. Transport delivery can be repeated after ambiguous failures; do not claim exactly-once storage.

Retry transient failures with bounded delays of 5, 10, 20, 40, 80, 160 and then 300 seconds. Authentication failures stop automatic uploads until configuration is corrected or a new boot retries. SIM PIN attempts are separately persisted before sending the unlock command; a failed/uncertain attempt is not retried automatically across resets. Only the explicit local retry action re-arms it. Data/storage errors do not trigger a factory reset or automatic flash erase.

## Setup, server and Home Assistant

On `ESP_RST_POWERON`, open a WPA2 SoftAP named `CarTracker-<device suffix>` using a unique factory-provisioned passphrase or a saved override. No SoftAP opens on ordinary timer/ignition deep-sleep wakes. Serve the English/Hungarian setup page at `http://192.168.4.1` with all assets embedded; no CDN or NAT. Before AP setup starts, the worker confirms STA is disconnected and PPP is down. Bind the AP HTTP listener to its interface and exact address before listening, with HTTPD's control socket on loopback. Enable STA only after successful isolated AP-server startup and route registration. PPP remains paused during the AP setup window. APSTA allows scanning, immediate Wi-Fi connection testing and a live status banner; a shared-channel change can briefly reconnect the phone.

When STA has a DHCP address, serve the same page from a separate listener bound only to the station interface and that address, such as `http://192.168.88.68`. Rebind after an address change and remove the listener when the connection is lost. Do not create a listener on PPP/LTE. LAN configuration remains available after the AP closes, while the device is awake and connected to Wi-Fi. It is unavailable during deep sleep. Opening or leaving the LAN page open, reading configuration/status, scanning networks and typing unsaved values do not pause GNSS sampling or uploads and do not inhibit parked sleep. Authentication state is independent of the tracking/apply state.

The AP's unclaimed window lasts five minutes. After association, retain its five-minute inactivity deadline and twenty-minute maximum from the first client. Page/config/status reads, typing and read-only authentication renewal do not extend that window. Only an explicit close or the AP timer closes the AP; saving/restoring does not close it automatically. Close on a LAN-only view does not reconfigure networking, rotate authentication or pause tracking. The separate LAN listener stays available while awake and connected.

Setup supports primary and secondary Wi-Fi profiles with separate scanning/manual hidden SSID and credentials, plus APN credentials, SIM PIN, endpoint, Traccar ID, Access credentials, network mode, reporting intervals, bench mode and clearly distinguished Wi-Fi AP and web-interface passwords. Wi-Fi-only save changes exactly the two SSID/password pairs, works without server credentials, keeps the page open and asynchronously applies changed credentials. Full save validates all supplied values but accepts missing network/server/Access prerequisites. Its response distinguishes saved configuration from upload readiness and lists the missing fields; `configured` becomes true only when full validation succeeds, and uploads remain gated on that readiness. Errors appear next to the clicked save button, with focus/scroll and retained inputs. After a successful full save, the editor closes but the summary and live status stay visible; Wi-Fi-only save leaves the editor open. Neither flow stops status polling.

Only a validated save, restore or explicit PIN-retry apply enters the transient tracking pause. The apply barrier quiesces in-flight network/PIN work before persistence, and the main scheduler releases it after reconfiguration. Invalid requests do not pause tracking; a failed save retains the prior configuration and releases its uncommitted barrier. Status exposes `applying` and `tracking_paused` for this interval, distinct from an open AP. Save acknowledgement reports `setup_closing=false` and `applying=true`; it is not a promise that reconfiguration has already finished.

Both save paths are atomic and preserve the previous revision. Configuration v3 migrates existing v1/v2 current and previous records, retaining the single v1 network as primary, preserving v2's two profiles, and initializing the new optional web password empty. Other settings, identity, queue and PIN state are preserved. GET responses expose only secret-presence flags. Empty secret inputs preserve values; explicit clear flags remove them. An explicit checkbox allows one PIN retry after saving a corrected PIN. Endpoint/device-ID changes must not relabel queued history; they are rejected while positions remain pending.

Wi-Fi connection rounds try primary for ten seconds, wait three seconds after a failed attempt, then try secondary for ten seconds. Offline rounds retry after fifteen seconds; five-minute retry waits apply only while LTE is working. A real disconnect starts another primary-first round after a three-second pause. Either healthy profile remains connected indefinitely; there are no periodic primary scans or forced returns from a working secondary. Closing the AP, saving unchanged Wi-Fi credentials or changing unrelated settings preserves the working STA association and DHCP lease unless the selected mode explicitly disables Wi-Fi. The top status distinguishes association, DHCP wait, connected profile/SSID/IP/RSSI, disconnection and unavailable/stale status. Router connection does not certify endpoint reachability.

The page polls status every five seconds while reachable, including after editor close and during save. It prevents overlapping status requests, times out read requests after ten seconds, and marks data stale after a failed request or fifteen seconds without a fresh response. It shows the last response time and replaces stale current values, including GPS Yes/No, with Unknown. Recovery resumes live values without changing drafts. Browsing is not a wake lock: when the board sleeps or Wi-Fi disappears, the panel becomes stale and retries status.

The cookie/CSRF session has a five-minute authentication idle limit and a twenty-minute absolute lifetime; successful reads refresh only authentication idle time. On the exact expired-cookie/CSRF rejection, the page makes one bounded read-only renewal (`GET /`, then `GET /api/config` for cookie/CSRF and metadata) and retries status once. It does not navigate or overwrite existing inputs, unchecked checkboxes or entered secrets. Failed renewal does not create a renewal loop; explicit Reconnect remains available. Basic-login `401` stops automatic login attempts until explicit reconnect. Mutation may retry once only after a definite pre-mutation session rejection; network errors, timeouts, other `403` responses and server errors are not automatically replayed. Mutation requests allow sixty seconds for the apply path and report an unconfirmed outcome if the response is lost.

The local HTTP page requires no web password by default, as selected by the owner. An optional `web_password` of 8–63 characters enables HTTP Basic authentication with username `admin` on both Wi-Fi listeners. Clearing it disables web authentication. This password is independent of the WPA2 CarTracker AP password, which remains required. HTTP checks each accepted socket's expected destination/interface, Host and Origin, uses an HttpOnly/SameSite session cookie plus a CSRF header for mutation, and bounds/parses request bodies before applying changes. Separate interface-bound listeners also preserve their routing boundaries if the home subnet overlaps `192.168.4.0/24`; neither exposes its control socket beyond loopback. A task-scoped linker wrapper applies listener/control restrictions only during HTTPD startup and fails closed if a restriction is missing. NVS credentials are not encrypted in this prototype.

The verified server baseline is **Traccar Home Assistant app 0.27.0 / Traccar 6.16.0**, a dedicated MariaDB database and **365-day** retention. A Cloudflare Tunnel from the existing homelab exposes only the HTTPS ingestion route; Access uses a device-specific Service Auth policy. Do not expose the Traccar administrative interface or arbitrary HA routes through that ingress. Tunnel/Access and the server are deployed in the homelab project. Database backups, device-token lifecycle and restore procedures belong to that deployment; the [server guide](setup/homeassistant-setup.md) records the interface. The deployed device token is non-expiring by the owner's decision.

Home Assistant uses its native **Traccar Server** integration and map card. Display last position, original fix age and last receipt age separately. Old route backfill must not move the current marker backward; no-fix heartbeats must not move it. History storage and stale/online presentation are verified against the pinned server/integration rather than inferred from an HTTP 2xx response.

## Verification and rollout

Run portable parser/configuration/serialization, store fault-injection, scheduling and setup-guard tests, plus an ESP-IDF 5.5.6 build. Then perform sequential gates: server synthetic upload; real GNSS over Wi-Fi without SIM; credential/certificate/outage tests; SIM/APN and concurrent GNSS+PPP/CMUX; power-cycle/queue recovery; ignition/sleep behavior; measured parked energy use; and a monitored vehicle road test. See [acceptance matrix](acceptance-tests.md). A missing hardware or server gate remains explicitly unverified.

Protocol references: [Traccar OsmAnd](https://www.traccar.org/osmand/), [Home Assistant Traccar Server](https://www.home-assistant.io/integrations/traccar_server/), [Cloudflare service credentials](https://developers.cloudflare.com/cloudflare-one/access-controls/service-credentials/service-tokens/), [Espressif modem architecture](https://docs.espressif.com/projects/esp-protocols/esp_modem/docs/latest/README.html).

## Remote output

Traccar queued custom commands `OUTPUT ON` / `OUTPUT OFF` are carried only in completed authenticated HTTP 200 responses. GPIO23 is active high, OFF after reset, and retained across normal deep sleep. Successful application requests a prompt report of actual output/readback/availability and an observation timestamp independent of GPS backfill. HA uses a non-optimistic template switch and local Traccar REST commands. Network loss or sleep delays delivery until the next successful report. See [the output contract and wiring](setup/remote-output.md).

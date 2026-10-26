# Tracker acceptance tests

The **Wi-Fi PoC is verified** on 2026-10-26: real SIM7600 GNSS fix without a SIM, authenticated HTTPS upload, Traccar storage and `device_tracker.pablo` in Home Assistant. The [PoC status](poc-status.md) records the measured baseline and remaining qualification. Rows below are acceptance criteria; host coverage does not imply a hardware scenario has passed.

Phone/AP and LAN access, scanning, Wi-Fi save, redacted configuration, invalid saves, configuration preservation, AP closure with retained LAN access and passive status were exercised. A 315-second observation completed 53 reads without pausing tracking; real fixes/uploads were also observed while the status view remained passive. The optional web password, deliberate secondary-network failures, long-session renewal and full interface-isolation stress remain hardware qualification items.

## Automated checks

| Gate | Method | Required outcome |
|---|---|---|
| Core | `make` in `firmware/test`, core suite | Strict valid/no-fix/invalid GNSS parsing; date and coordinate boundaries; knots conversion; optional fields; escaped form fields; configuration and size rejection |
| AT response collection | Accumulated snapshot regression suite | Fragmented long position/satellite replies parse without duplicate partial lines; complete terminal detection and bounded storage |
| Persistence | Store host suite with injected backend failures | Atomic config/rollback, v1/v2-to-v3 current/previous migration, optional web password defaults and persistence, Wi-Fi-only field isolation, partial full-save with readiness false, durable PIN latch, original timestamps, reboot recovery, partial record/ack/GC writes, 4096 capacity and exactly one oldest eviction |
| Scheduling | Policy host suite | Active/grace/parked transitions, timing boundaries, AP/apply/ignition sleep inhibition without a passive-LAN veto, and capped retry delays |
| Network policy | Network policy host suite | Ten-second profile attempts, three-second gaps, fifteen-second offline and five-minute working-LTE retries, either working profile retained without periodic primary probes, DHCP-boundary and LTE-loss recovery, bounded HTTP deadline and stale sleep-ack rejection |
| Setup guards | Setup policy host suite | Exact AP/STA IPv4 and IPv4-mapped IPv6 matching, address-specific Host/Origin checks, optional admin credentials, session-cookie matching, duplicate-cookie rejection, flat request shape and embedded-NUL/nesting rejection |
| Setup apply/auth state | Setup policy/apply host tests | Authentication expiry independent of tracking; reads never enter apply; validated mutations pause until scheduler completion; failed/uncommitted apply releases; AP deadlines remain separate |
| Setup listener | Actual listener-wrapper source with mocked network calls | Separate AP/STA binding before listen, exact interface/address and loopback control ports, IPv4/mapped IPv6, PPP interfaces rejected, unrelated task/port binds unchanged, unavailable interface and binding/hook failures fail closed |
| Firmware build | ESP-IDF v5.5.6, target `esp32` | All four tracker components and scheduler link against real IDF APIs; binary fits the application partition |
| Embedded page | JavaScript syntax/behavior tests and browser exercise | Wi-Fi-only field isolation, failed saves preserve entries, errors focus/scroll beside the clicked Save, localized field guidance, partial-save success lists missing upload fields, optional web password distinct from AP password, safe reopen links and per-profile scans; polling continues after editor close/during save; stale GPS becomes Unknown; hung reads time out without overlap; exact expired-session renewal/retry is bounded and preserves drafts/secrets/unchecked fields; Basic-login failures and uncertain mutation results are not replayed |
| Provisioning | `python tools/test_provision_tracker.py` | Unique private credentials, overwrite refusal, label validation, safe generator failures and correct image-size checks without hardware |

The host store uses an emulated flash/NVS backend. Its fault tests do not replace power interruption on the real chip. Setup policy tests do not prove that an AP/STA socket is rejected correctly by the real network stack.

## Server and first Wi-Fi commissioning

1. Create the registered Traccar device and exact HTTPS ingestion route. A valid device service token reaches ingestion; missing, incorrect, revoked and expired tokens fail. Other paths cannot expose Traccar administration or HA APIs.
2. Send a synthetic location through the intended form contract and verify Traccar storage, km/h speed handling and HA map display. Record the original fix time separately from server receipt time.
3. Build/provision/flash the owned ESP32 only after verifying the factory recovery copy. Confirm 4 MB flash, 40 MHz settings, expected partition boundaries and a unique setup password. A missing provisioning record must not open an unsecured/default-password AP.
4. Boot without a SIM, join setup Wi-Fi, scan/select the home SSID, configure `wifi_only` with bench mode enabled, and save. Obtain a **real GNSS fix** outdoors and confirm the same location in Traccar and Home Assistant.
5. Test invalid server certificate, wrong hostname, absent clock, DNS failure and server outage. No insecure TLS fallback or redirect-following is allowed. Existing configuration is retained.
6. Before the first real fix, GNSS failure leaves the tracker unlocated rather than creating `(0,0)`. After commissioning, remove GNSS reception: no-fix heartbeat preserves the last real coordinates and original `fix_time`; receipt time may advance.
7. Replay duplicate and out-of-order reports. Current marker and last-fix age must not regress when route history is backfilled. Invalid coordinates and unknown device IDs must not silently become accepted live locations.

## Setup and stored configuration

| Scenario | Required observation |
|---|---|
| Timer/ignition wake or software reset | Setup AP does not open |
| No setup client | AP closes after the five-minute initial window |
| Connected idle client | AP closes after five minutes without user activity; passive status polls do not extend it |
| Repeated status/typing/renewal | Does not extend the AP inactivity deadline; AP still closes by twenty minutes after the first client |
| Valid save/restore/PIN-retry apply | Brief tracking pause starts only for a validated mutation, drains existing work and lasts until scheduler reconfiguration completes; `applying`/`tracking_paused` return false afterwards. Saving/restoring does not close AP automatically; STA/DHCP remain unless changed credentials or cellular-only mode require otherwise |
| Explicit close | Ends the editor and closes an open AP after the response; LAN-only close does not pause/reconfigure tracking or rotate the session; live status remains visible while connected |
| LAN configuration | At the assigned station address, e.g. `http://192.168.88.68`, open the same page before and after AP timeout/close; reconnect to the new address after a DHCP change; no listener through PPP/LTE |
| Passive LAN tracking | Leave the page open, poll status, scan and type unsaved fields across several report intervals: real GNSS sampling and eligible uploads continue; no typing `/api/activity` requests and no apply pause |
| Awake versus asleep | LAN view/polling/session renewal does not veto normal parked sleep; sleeping or disconnected device is shown as unavailable. The bounded AP window and actual settings application retain their sleep veto |
| Continuous status and stale GPS | Poll every five seconds, including after full save/editor close. Simulate disconnect and a hung request: after failure or fifteen seconds without fresh data show Unknown rather than old GPS No, with last-response time; read timeout is ten seconds, requests do not overlap, and reconnect restores live values |
| More than twenty minutes open | At absolute cookie/CSRF expiry, one exact-session-403 renewal (`GET /`, `GET /api/config`, one status retry) restores updates without a manual reload, tracking pause or AP/awake extension; entered secrets, text and unchecked selections remain unchanged |
| Renewal and login failure | Failed renewal does not recurse/repeat page/config requests on every poll; explicit reconnect preserves drafts. Basic `401` stops automatic login attempts; invalid Host/Origin `403` never triggers automatic renewal |
| Initial page-load outage | Status polling is still scheduled; reconnect fills only untouched initial fields and preserves all edits made while disconnected |
| Slow or uncertain save | Allow up to sixty seconds for the mutation response. Lost/timeout response is reported unconfirmed and retains entries; neither it nor a server error is replayed. Only a definite pre-mutation invalid-session rejection may renew and retry once |
| Default and optional web login | Default page requires no HTTP password; setting 8–63 characters requires Basic username `admin` and that password on both AP/STA pages; wrong/missing credentials fail, explicit clear restores no-login access, AP WPA2 password remains independent |
| Setup uplink isolation | Before AP HTTP starts, STA is disconnected and PPP is down; STA resumes only after AP-bound listener startup. A separate STA-bound listener intentionally serves LAN configuration; PPP stays paused during the AP window |
| Overlapping home/AP network | With a home network on `192.168.4.0/24`, each HTTP listener accepts only its own interface/address; neither HTTPD control socket is reachable from AP, STA or PPP |
| Interface binding failure | The affected HTTP listener fails closed; no unbound fallback serves the page |
| Separate Wi-Fi save | Both profiles save without server credentials; unrelated edits are not persisted, page stays open, connection begins and top status reflects actual profile/SSID/DHCP/IP |
| Unchanged Wi-Fi save or AP close | Working STA association and DHCP lease remain; no disconnect/reconnect is introduced by an unchanged save or unrelated field change |
| Primary unavailable or incorrect password | After a ten-second primary attempt and three-second pause, secondary gets ten seconds; a working secondary stays connected beyond five minutes even if primary returns; only a real disconnect starts a new primary-first round |
| Failed rounds and LTE | Both profiles failing yields a fifteen-second offline retry; five-minute retries apply only while LTE works; losing LTE shortens the pending delay |
| v1/v2 firmware update | v1 network becomes primary, v2 preserves both profiles; optional web password defaults empty; all existing secrets, queue, identity and PIN latch survive; interrupted v3 migration preserves a complete revision |
| Full save before server setup | Valid partial values save successfully; response lists missing endpoint/device/network/Access requirements and `upload_ready=false`; no upload occurs until full readiness becomes true |
| Invalid full or Wi-Fi save on mobile | Error appears beside the button used, receives focus/scroll, names the field and retains entries; no tracking pause, offscreen-only error or success claim |
| Forged Host/Origin or missing/incorrect CSRF/session cookie | Mutation denied; stored state unchanged |
| Oversized, nested, malformed or duplicate-key JSON | Request rejected; no reset, partial update or secret exposure |
| Secret fields left blank | Existing secrets remain; GET reports only presence flags |
| Explicit clear or new password | Validated web-password change applies to a new browser session; AP password change affects the next AP window; clearing each secret affects only that credential |
| Power loss during save | Either prior or complete new validated configuration loads; no mixed fields |
| Previous configuration restore | Prior complete revision is restored; no factory erase |
| Endpoint/device-ID change while queue is nonempty | Rejected; pending history cannot be relabeled |
| Incorrect/uncertain SIM PIN attempt | Persistent latch prevents automatic retries across reset; only explicit reviewed retry re-arms it |

## Cellular and recovery

1. Fit the correct LTE antenna and active compatible SIM. Demonstrate SIM readiness, actual LTE registration, carrier APN, PPP IP/DNS and verified HTTPS on **cellular only**.
2. Run GNSS polling while PPP/CMUX uploads for at least an hour. No UART data collisions, stalled AT channel, corrupted fixes or unexplained resets.
3. In automatic mode, remove working Wi-Fi and recover through LTE. Return Wi-Fi and verify preference with no duplicate network clients. Test an associated Wi-Fi network that cannot reach the endpoint.
4. Interrupt uplink while recording known positions, then restore it. The newest live position arrives first, followed by chronological backlog at at most two requests/second, retaining original timestamps.
5. Reset during record append, acknowledgement and queue recycling. Recover valid committed records; discard incomplete writes safely; no silent queue reset. Fill capacity and verify exactly the oldest pending record is dropped and the drop counter persists.
6. Fail a request after the server may have received it. Retries retain sample identity; document any server-side duplicate representation. The system does not promise exactly-once delivery.
7. Recover after server, Home Assistant, tunnel and board restarts. Distinguish network receipt, stored position, last measured fix and server/integration presentation.

## Ignition, sleep and vehicle gate

Use a protected bench ignition signal first. Measure the complete board and power supply; do not extrapolate from ESP32 chip datasheet sleep current.

- GPIO13 low means ignition on; debounce noise and test disconnected-input behavior. Verify that no SD/JTAG use conflicts with this pin.
- Ignition on produces 30-second updates. Ignition off retains that rate for 600 seconds, then enters parked operation. A renewed ignition signal cancels grace or sleep preparation immediately.
- Parked wake intervals target 1800 seconds start-to-start. GNSS work uses its 90-second allowance within the 150-second work window, with a further 30-second orderly-shutdown target. Verify both the normal path and fault behavior separately.
- Inject failed PPP/CMUX shutdown, uncertain modem state and stalled HTTP cleanup. The current firmware refuses sleep and retries, which can exceed 150+30 seconds; it does not provide a strict failure-time power bound. Record elapsed time and current, and treat unresolved unbounded wakefulness as a failed vehicle-deployment gate.
- Assert ignition during DNS, HTTPS, GNSS timeout, modem shutdown and immediately before deep sleep. The device must remain active or wake immediately through level-low ext0.
- Check modem/GNSS/antenna power and Wi-Fi state during sleep; verify current physically. The bounded setup AP window and an actual settings application intentionally prevent sleep; a passive LAN status/editor page must not.
- Test brownout, vehicle start/cranking and external supply transitions. USB/battery changeover may reset the board; validate recovery rather than assuming uninterrupted backup.
- Record active/transmit/acquisition/sleep/out-of-coverage current, converter quiescent draw and the external low-voltage cutoff. Calculate and approve a parked energy budget before unattended installation.
- Only after these gates, perform a monitored road test comparing actual route, timestamps, parking position and reconnection behavior. Vehicle power and ignition must use the qualified installation interface.

Store sanitized test evidence alongside the build identification. Keep credentials, raw factory flash, MAC/IMEI and private location history out of the repository.

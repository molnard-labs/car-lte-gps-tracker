# Tracker networking

`tracker_net` is the sole owner of Wi-Fi, the SIM7600 UART and the device's IP
interfaces. The control task owns application scheduling and the ignition input.
Initialize `tracker_store` first, then call `tracker_net_init()`. Read the cached
fix and selected network without waiting for a modem command. Supply validated
configuration (or commissioning defaults) to initialization. Supplied fields
must pass `tracker_config_validate_stored()`, which permits an incomplete setup.
Full `tracker_config_validate()` readiness is separate and gates location uploads;
missing endpoint/device/network/Access fields do not prevent saving valid settings
or connecting Wi-Fi for local configuration. Configuration schema v3 migrates
v1/v2 records and adds an optional web-interface password.

## Transport

The worker starts standalone SIM7600 GNSS without requiring a SIM. Wi-Fi uses
the station interface. Cellular uses pinned `espressif/esp_modem` 1.4.1, a
SIM7600 DCE, PPPoS and CMUX: the GNSS command channel never reads the PPP byte
stream. Its published manifest accepts ESP-IDF >=4.1; this application pins the
5.5.6 series separately. UART1 is TX27/RX26 at 115200 without flow control.
GPIO4 is PWRKEY, GPIO25 is flight control, GPIO32 is DTR, and GPIO34 is STATUS.
LILYGO's AUXVDD index127 is a driver sentinel, not a literal GPIO number. Antenna
power follows its SIM7600 driver: `AT+CVAUXV=2800`, then `AT+CVAUXS=1`; disable
with `AT+CVAUXS=0`. `AT+CGPS=1` starts standalone GNSS after querying its state.
Sanitized diagnostics identify voltage, antenna power, query and start failures
separately; raw modem responses and credentials are never printed.

The AT collector replaces each accumulated `esp_modem` response snapshot. It
must not append it: fragmented long GNSS responses would duplicate an earlier
partial line and fail parsing. `CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED=y`
is required so CMUX uses the same accumulated-snapshot contract as UART. The
collector enforces its 768-byte capacity and waits for a complete terminal line.
The host regression suite replays fragmented position/satellite responses through
the collector and the real GNSS parsers.

Wi-Fi tries the primary profile for ten seconds, disconnects a failed attempt,
waits three seconds, then tries the secondary for ten seconds. AUTO allows this
23-second first round before trying LTE. Either working profile stays connected
indefinitely; there are no periodic primary probes or automatic primary returns.
After a real disconnect, a new primary-first round starts after a three-second
pause. Exhausted rounds wait 15 seconds offline, or five minutes while LTE works;
losing LTE shortens that pending wait to 15 seconds. DHCP success is rechecked
against the driver/interface before a timed-out attempt is disconnected.
A failed Wi-Fi HTTP transport temporarily makes LTE preferable;
authentication/server errors do not change the transport. The application must
enable PPP, PAP/CHAP, the certificate bundle and
`CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF` in sdkconfig. The latter keeps DNS
consistent when the default route changes.

HTTP uses `tracker_osmand_form`, authenticated HTTPS, the public CA bundle and
hostname verification. The same endpoint and Cloudflare Access service-token
headers apply over either link. Redirects are disabled. There is no fallback to
unencrypted HTTP or disabled certificate verification. UTC must be available
from GNSS or SNTP before sending. A 2xx response is required for acceptance; the
caller owns queue acknowledgment and retry policy. A dedicated worker performs
asynchronous HTTPS against one absolute 15-second deadline. The publishing
caller returns by that deadline even if DNS is still blocked internally. The
worker retains its request buffers until it finishes cancellation and cleanup;
sleep and reconfiguration cannot proceed until cleanup completes. Reusing a
client is limited to the same selected interface and configuration.

## SIM PIN

Only `SIM PIN` is eligible for automatic unlocking. The persistent attempt lock
is committed **before** transmitting a PIN. A subsequent `READY` response clears
it. Errors, reset, or lost responses cannot cause an automatic retry. Missing
SIM, missing PIN, PUK and a locked retry are diagnostic states, not reboot causes.
The setup/control layer explicitly clears the lock when the user authorizes a
retry. Neither PINs nor modem command payloads are logged by this component.
PIN transmission is serialized with AP activation and configuration replacement;
an old configuration snapshot cannot transmit a newly replaced PIN. Opening
setup pauses unlocks until the control task reloads configuration after closing
setup, even if no setting changed. A read-only CPIN status poll also runs in
Wi-Fi-only mode; it never unlocks the SIM there.

## Sleep and cancellation

`tracker_net_prepare_sleep(timeout_ms)` closes HTTPS, requests worker shutdown,
and waits no longer than the caller's timeout. The worker leaves PPP/CMUX through
esp_modem's command-mode transition (which waits for PPP exit), disables GNSS and
antenna power, acknowledges `AT+CSCLK=1`, stops Wi-Fi, then raises and retains
DTR. PWRKEY is retained inactive and flight control high. No manual UART escape
sequence or modem firmware update is used.

The esp_modem 1.4.1 transition itself can wait up to 30 seconds for PPP exit;
this work stays in the worker and does not prevent the caller's deadline from
expiring. **Do not enter deep sleep after timeout/failure.** The control task may
call `resume()` concurrently to cancel a shutdown, then recheck ignition before
sleep. Cancellation invalidates a waiting prepare request. On resume, held GPIO
states are restored without a PWRKEY pulse; DTR is lowered, AT availability is
checked, and networking is rebuilt. Routine sleep retains modem registration;
this is not an assertion of measured whole-board sleep current.

`tracker_net_set_gnss_enabled(false)` invalidates the fix cache immediately and
asks the modem worker to stop GNSS and antenna power. The control task uses this
at the parked acquisition cutoff while allowing remaining uploads. Repeated
GNSS shutdown is safe: query the current state before requesting CGPS=0.

The 150-second parked work plus 30-second shutdown budget is a qualification
target for the functioning hardware path. If modem state cannot be confirmed,
CMUX shutdown fails or HTTP cleanup is still blocked, sleep is refused. Recovery
can therefore exceed the budget. Unattended vehicle installation remains blocked
until shutdown, failure behavior and whole-board current are measured. AT+CPOF
does not provide a guaranteed maximum shutdown time and is not an emergency
power cutoff; no unverified PWRKEY fallback is implemented.

## Setup AP and Wi-Fi LAN configuration

The setup component owns HTTP and invokes start/stop/scan here. The AP is
WPA2-Personal, one client, at 192.168.4.1. No NAT is configured. Before start
succeeds, HTTPS finishes cleanup, the modem owner stops PPP/CMUX and the Wi-Fi
driver stops the previous station. STA stays paused until the setup component
has bound its listening sockets exclusively to the AP interface/address and
installed its routes, then calls `tracker_net_setup_allow_wifi()`. APSTA can then
connect either saved network while the setup page remains available. Cellular
reconnection and PIN attempts stay paused during the AP window. Listener
isolation is mandatory even when the upstream subnet overlaps 192.168.4.0/24;
accepted-socket checks alone are insufficient. Stop the AP HTTP listener before
stopping the AP; the separate LAN listener is not part of that shutdown.
Setup-start failure restores normal network eligibility without starting HTTP.
`tracker_net_apply_wifi()` queues only the two Wi-Fi SSID/password pairs; it does
not apply other pending fields or authorize a PIN attempt. Setup can test Wi-Fi
even with a saved CELLULAR_ONLY mode; closing restores that mode. An unchanged
Wi-Fi save and closing setup preserve the working STA association and DHCP lease
unless CELLULAR_ONLY explicitly disables STA. Reloading unrelated settings also
leaves Wi-Fi alone. Sanitized diagnostics log profile number, association channel,
DHCP acquisition/loss and numeric disconnect reasons, never SSIDs or passwords.
Status uses
the driver's actual associated SSID/RSSI and the station's DHCP address, separate
from connection attempts. AP and STA share one radio/channel, so a phone may
briefly reconnect when the upstream channel changes. The setup component owns the physical recovery gesture,
password provisioning, inactivity timer, validation and secret redaction.

`tracker_setup` also maintains a separate HTTP listener on the station's current
IPv4 address, for example `http://192.168.88.68`, bound to `WIFI_STA_DEF`. It
rebinds after DHCP changes and removes the listener on Wi-Fi loss. AP and STA
HTTPD control sockets bind to separate loopback ports; PPP/LTE never gets a web
listener. Closing or expiring the AP keeps the LAN page available while the
device is awake and connected. An active editing session temporarily inhibits
sleep; an idle LAN listener does not. Deep sleep makes the page unavailable.

Web authentication is optional and off by default. The setup page's
`web_password` field accepts 8–63 characters and enables local HTTP Basic login
with username `admin` on both Wi-Fi listeners. The CarTracker WPA2 AP password
is a separate credential and remains required. Full saves accept valid partial
configuration, return explicit upload readiness and missing fields, and retain
inputs with feedback beside the clicked Save button when validation fails.
Mutation still requires the expected interface/address, Host, Origin, session
cookie and CSRF header, with or without optional web authentication.

## Validation boundary

Host parsing/encoding tests are in tracker_core. `test/test_net_policy.c` checks
primary/secondary timeout, three-second gaps, offline/LTE retry behavior,
both profiles remaining connected, DHCP success at the retry boundary, the fixed
HTTP deadline and cancellation/acknowledgement
predicates, including late completion of an obsolete sleep request. These are logic tests, not a
simulation of UART, FreeRTOS scheduling or sockets. A complete ESP-IDF build checks
the transport APIs. Real Wi-Fi, SIM/PIN, GNSS antenna, CMUX simultaneous GNSS/PPP,
TLS, route changes, modem sleep current and ignition-during-shutdown tests remain
mandatory hardware commissioning; compile success is not evidence that they
passed.

References: [esp_modem 1.4.1 source](https://github.com/espressif/esp-protocols/tree/modem-v1.4.1/components/esp_modem),
[LILYGO SIM7600 GNSS implementation](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/blob/main/lib/TinyGSM/src/TinyGsmClientSIM7600.h),
[LILYGO pins](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/blob/main/examples/ATdebug/utilities.h),
[LILYGO sleep example](https://github.com/Xinyuan-LilyGO/T-SIM7600X/blob/master/examples/sleep/sleep.ino).

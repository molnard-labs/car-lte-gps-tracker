# Remote GPIO / LED / relay output

The tracker has one **active-high output on GPIO23**. Traccar sends the command in the HTTP response to the board's next OsmAnd upload. Home Assistant exposes a switch backed by the board's reported state. No additional public endpoint or MQTT broker is needed.

## Wiring

For the LED test:

```text
GPIO23 → 1 kΩ resistor → LED anode (+)
                         LED cathode (−) → board GND
```

HIGH is approximately 3.3 V and means ON. A relay board must accept a 3.3 V active-high control input and have its own suitable supply with a common logic ground. A bare relay coil must not be connected directly to the GPIO. Use an external pull-down if the relay interface needs a defined OFF level while the ESP32 is resetting/booting.

GPIO23 is dedicated to this output; it is not a modem signal, the ignition input or a boot-strapping pin. The firmware enables input readback as well as output drive. This confirms the pad's logic level; it does not measure relay contact position.

## Traccar commands

Select `pablo` in Traccar, open **Commands / Parancs**, choose **Custom / Egyéni** and send exactly one of:

```text
OUTPUT ON
OUTPUT OFF
```

The two commands are idempotent: repeated ON never toggles the pin. Surrounding spaces/newlines are accepted. Other text, extra commands, embedded NULs, oversized responses, failed HTTP responses and incomplete/cancelled requests do not switch the output. The maximum response is 64 bytes. Only the configured authenticated HTTPS upload response can deliver commands; the local configuration API has no output-control route.

Traccar queues the command until the device sends a report. Active/bench operation normally gives up to a 30-second reporting delay. During parked deep sleep the command waits for the next wake and successful report, normally on the 30-minute cycle. Network outages add delay. A queued command is not a delivery acknowledgment; retrying ON/OFF is safe when the reported state has not changed. Queued commands are not given a separate expiration time by this firmware.

After applying a command the board requests a prompt fresh-state report. Traccar attributes are:

| Attribute | Meaning |
|---|---|
| `output` | Applied logical ON/OFF state |
| `outputReadback` | Actual GPIO input readback, HIGH/LOW |
| `outputAvailable` | Driver/readback agrees with the applied state |
| `outputGpio` | `23` |
| `outputReportedAt` | Time of this output-state observation, Unix milliseconds; independent of an old backfilled GPS fix |
| `outputResult` | `boot`, `on`, `off`, `rejected` or `gpio_error` |

The local status page also shows applied/rejected command counters. GPS tracking and configuration/history storage continue independently.

## Reset and sleep

Power-on, RST, software reset and watchdog reset start **OFF**. A normal deep-sleep wake retains the last applied state through RTC data and pad hold. An energized relay therefore still draws its own current while the ESP32 sleeps; include that load in the parked energy budget. Cold-reset OFF and ON/OFF readback were checked on the board; sleep retention is host-tested and remains part of full vehicle sleep/power qualification.

## Home Assistant

The installed entity is **`switch.pablo_output`**, shown on the **Autó** dashboard (`/car-tracker/car`) with the car map. The switch state follows the board acknowledgment rather than the button click. It becomes unavailable when output data is missing, inconsistent or stale (two minutes active, forty minutes parked).

To reproduce the installation:

1. Keep the Traccar user restricted to `pablo`. It may remain read-only for user/device editing, but **Limit commands** must be disabled so it can send custom commands. Do not give it administrator access.
2. Add `output`, `outputReadback`, `outputAvailable`, `outputGpio`, `outputReportedAt`, `outputResult` to the Traccar Server integration's custom attributes, preserving the existing attributes.
3. Copy [the package](../../homeassistant/packages/car-output.yaml) to `/config/car_tracker_output.yaml`. Adjust the local Traccar host and numeric device ID in its two REST commands if necessary.
4. Put the existing car-only Traccar token in HA's private `secrets.yaml` as `traccar_pablo_authorization: "Bearer <token>"`.
5. Merge this package into the existing `homeassistant.packages` configuration:

```yaml
homeassistant:
  packages:
    car_tracker_output: !include car_tracker_output.yaml
```

6. Check HA configuration, then reload REST commands and template entities if those integrations are already loaded; otherwise restart HA. Add [the native dashboard card](../../homeassistant/lovelace/dashboard.yaml).

The working deployment preserved the user's other configuration, made backups, retained the Traccar account's read-only/device-only restrictions, and loaded the new HA services without restarting HA. Traccar was restarted once to refresh its command-permission cache. Credentials remain in private NVS/HA secret files and are not stored in this repository.

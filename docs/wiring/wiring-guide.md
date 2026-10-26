# Tracker wiring — current Traccar firmware

This revision uses the diagnosed **LILYGO T-SIM7600X T54-V1.3 / ESP32-WROVER-E**, with 4 MB flash, PSRAM at 40 MHz, and a **SIM7600E-H** modem despite inconsistent G-H label text. See the [board evidence](../../hardware/board-specification.md) and [diagnostics](../../hardware/diagnostics-2026-10-26.md).

## Bench connections

1. Connect the ESP32 USB-C port to a reliable 5 V USB supply/PC. This is the programming console (previously COM9). Attach the LTE and GNSS antennas to their correctly labelled modem connectors.
2. Test outdoors or with the GNSS antenna having a clear sky view. Wi-Fi upload works without a SIM, but the SIM7600 must still run for GNSS.
3. Select `WIFI_ONLY` and bench mode on the setup page. Bench mode disables automatic deep sleep.
4. To test ignition logic, connect **GPIO13 to board GND** through a removable jumper. Low means ignition on; open means off. The firmware enables the internal pull-up. Use only a short logic wire for this bench test.

| Signal | ESP32 pin | Role |
|---|---:|---|
| Modem UART TX / RX | GPIO27 / GPIO26 | Exclusively owned by esp_modem |
| Modem PWRKEY | GPIO4 | Board transistor control; firmware sequencing |
| Modem FLIGHT | GPIO25 | Retained high for modem operation |
| Modem DTR | GPIO32 | Retained high during parked sleep |
| Modem STATUS | GPIO34 | Input only, no internal pull-up |
| Ignition | **GPIO13** | Active low, internal pull-up, RTC ext0 wake |

Do not use the SD-card interface or GPIO13 JTAG function. GPIO26 remains reserved for the modem. The tracker has no relay output.

## Vehicle ignition interface

The GPIO must receive a **protected, isolated, open-collector signal**. Never connect the vehicle ignition/ACC voltage to GPIO13 directly.

```text
Vehicle ignition/ACC +12 V
          │
  protected, isolated detector
          │  isolation barrier
          └── open-collector output ── GPIO13
              output-side emitter ─── board GND

Ignition on  → output sinks GPIO13 → LOW
Ignition off → output releases it → internal pull-up → HIGH
```

The vehicle-side detector must be selected and validated for automotive transients and the actual ignition voltage range before installation. Its output leakage must be low enough for the ESP32's weak internal pull-up, and its voltage/current ratings must suit the circuit. Do not assume a generic optocoupler module qualifies without checking its schematic and ratings.

The firmware debounces the input for 100 ms. Before deep sleep it changes GPIO13 to an RTC input, enables `rtc_gpio_pullup_en`, disables pull-down, retains the RTC peripheral power domain, and enables active-low ext0 wake. It rechecks ignition after modem shutdown. A low level during the final sleep transition immediately wakes the ESP32.

Validate noise immunity and false-wake behavior with the intended short logic-side harness. Add an external pull-up only if this qualification fails; it is not required by the initial design.

## Vehicle supply

Use a fused, transient-protected, reverse-polarity-protected automotive **5 V supply**, with sufficient peak current for ESP32 Wi-Fi and the SIM7600 modem. Add independent starter-battery low-voltage protection. The tracker must receive permanent power to send parked heartbeats.

```text
Permanent vehicle supply → fuse → automotive protection / 5 V converter
                                    → battery cut-off → VBUS + GND
```

The board's regulated 5 V main input is **VBUS**, shared with USB-C. Its white **SOLAR / INPUT 4–6 V** connector is a battery charging input. It is not the vehicle's 12 V supply connection. USB/battery source switching can restart the board.

Specify and test the automotive protection, converter, cut-off threshold, wiring and fuse with the actual installation before connecting to the car.

The board's battery ADC is uncalibrated; firmware neither reports an invented battery level nor uses it for starter-battery protection. Measure complete-system active and parked current, including the converter and ignition interface, before estimating parked runtime. Relay control, immobilization, OTA and motion-based towing detection are outside this revision.

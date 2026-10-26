# Owned board specification

The project uses a **LILYGO T-SIM7600X**, PCB **T54-V1.3**, with ESP32-WROVER-E and a modem identified as **SIM7600E-H**. The current Wi-Fi tracker has been flashed and verified through to Traccar/Home Assistant; see [PoC status](../docs/poc-status.md).

## Verified identity

| Item | Observation |
|---|---|
| MCU module | ESP32-WROVER-E, marking `M0N4R8` |
| Chip | ESP32-D0WD-V3, silicon revision 3.1, 40 MHz crystal; firmware target `esp32` |
| Flash | **4 MB**, manufacturer `0x46`, device `0x4016`; complete read/backup verified |
| PSRAM | **8,388,608 bytes**, all addresses passed six patterns at **40 MHz** |
| USB bridge | CH9102; observed Windows port COM9 |
| Modem | SIM7600E-H, despite inconsistent printed G-H text |
| Modem part number | `S2-10B47-Z30S2` |
| Modem firmware | `LE11B14SIM7600M22_211104` |

The photographed QR part number, matching firmware and live E-H identification agree. SIMCom's [PCN-20231127-001](https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/6546/PCNofSIM7600E-H_20231127.pdf) maps the `S2-10B47` prefix to E-H. This supports using E-H identity; it does not establish G-H global-band capability. Cellular registration/band coverage have not yet been tested. ESP32 application flashing does not update the modem firmware.

The original ESP32 directly maps only part of its external memory. Normal tracker heap allocation does not automatically expose all 8 MiB; the diagnostic used himem to test upper memory. Keep flash DIO and PSRAM at the verified **40 MHz** settings. See [hardware diagnostics](diagnostics-2026-10-26.md#full-8-mib-psram-test) and the [Espressif module datasheet](https://documentation.espressif.com/esp32-wrover-e_esp32-wrover-ie_datasheet_en.html).

## Connections and pins

| Connection | Function |
|---|---|
| USB-C **TTL**, near ESP32 | ESP32 programming/console through CH9102 |
| USB-C **USB**, near modem/SIM holder | Direct modem USB |
| RF **GPS** | External GNSS antenna; active-antenna supply controlled by modem AUXVDD |
| RF **MAIN** / **AUX** | Cellular main / diversity antennas |
| GPIO27 / GPIO26 | ESP32 UART TX / RX, reserved for the modem |
| GPIO4 / GPIO25 / GPIO32 | Modem PWRKEY / FLIGHT / DTR |
| GPIO33 / GPIO34 | Modem RING / STATUS |
| GPIO23 | Active-high LED/relay-board output with input readback and deep-sleep hold |
| GPIO13 | Active-low ignition input in this project, internal RTC pull-up; SD/JTAG use disabled |
| GPIO12 | Onboard LED |
| GPIO35 / GPIO36 | Battery / solar analog sense inputs |
| GPIO14 / 2 / 15 / 13 | Board SD interface; not used by tracker firmware |
| GPIO21 / GPIO22 | Default I2C SDA / SCL |

The active firmware controls antenna supply with `AT+CVAUXV=2800`, `AT+CVAUXS=1` and starts standalone GNSS with `AT+CGPS=1`. AUXVDD's vendor index 127 is not ESP32 GPIO127. One `esp_modem` instance owns UART; no second reader may consume its bytes.

Mapping references: [LILYGO board guide](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/blob/main/docs/en/esp32/sim7600-esp32/README.MD), [pin definitions](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/blob/main/examples/ATdebug/utilities.h).

## Power and battery measurement

Use regulated **5 V** through USB-C or **VBUS**, with the manufacturer's **at least 2 A peak** supply capability. Vehicle voltage requires a qualified external converter, transient/reverse-polarity protection, fuse and independent starter-battery cutoff. See [wiring](../docs/wiring/wiring-guide.md).

Battery support is for one rechargeable Li-ion/LiPo cell, nominal 3.7 V / full 4.2 V; the documented charging limit is 500 mA. USB/battery source changes can reset the board. The white **SOLAR / INPUT 4–6V** connector is a battery charging input; follow LILYGO's documented 5–6 V operating range and do not treat it as a vehicle-voltage input.

Battery sensing is uncalibrated and unused by the tracker. LILYGO documents that the sense circuit disconnects while USB is attached. The linked [family schematic](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/blob/main/schematic/esp32/T-SIM7600X-V1.3.pdf) has older V1.2 title blocks and CP2104/WROVER-B parts, so it is a reference rather than proof of every fitted component on this CH9102/WROVER-E unit. Confirm fitted divider/ADC behavior against a meter before adding battery telemetry.

## Remaining hardware work

The first real GNSS/Wi-Fi/HTTPS location is verified. LTE/SIM/APN, ignition wake/sleep, real vehicle power behavior, complete-board current and long-duration/environmental operation still need qualification. Raw photos/flash and unique MAC/IMEI values remain private. The [acceptance checklist](../docs/acceptance-tests.md) defines the remaining tests.

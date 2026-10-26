# Hardware diagnostics — 2026-10-26

These checks established the owned board's identity and recovery baseline before installing the tracker. Factory-firmware checks were read-only; a standalone PSRAM diagnostic ran only in RAM. The tracker was subsequently installed and its real GNSS → Wi-Fi → Traccar → HA path verified, as recorded in [PoC status](../docs/poc-status.md).

## Confirmed results

| Check | Result |
|---|---|
| ESP32 ROM identity | ESP32-D0WD-V3, revision 3.1, 40 MHz crystal |
| Flash | Manufacturer `0x46`, device `0x4016`, **4,194,304 bytes** read and verified |
| Factory flash configuration | 4 MB, DIO, 40 MHz |
| PSRAM | Auto-detected **8,388,608 bytes**, six full-array patterns, zero errors at **40 MHz** |
| USB bridge/console | CH9102 on COM9, 115200 baud |
| Modem identity | SIM7600E-H, corroborated by live model/profile, QR part number and SIMCom mapping |
| Modem software | `LE11B14SIM7600M22_211104` |
| SIM during diagnostics | Absent; cellular registration/data were not tested |

See [board specification](board-specification.md) for connectors, power limits and the E-H/G-H label discrepancy.

## Full 8 MiB PSRAM test

The standalone ESP-IDF **5.2.5** RAM-only diagnostic is preserved in [tools/psram-ram-test](../tools/psram-ram-test/README.md). It is independent of the tracker's ESP-IDF 5.5.6 build.

Code/data/stacks remained in internal RAM. A 32 KiB himem window covered the upper-memory region, while the direct region covered the rest. Disjoint regions totalled exactly 8 MiB. Every 32-bit word was written and later read for six patterns, including physical-address-derived values and their complements to detect aliasing. Cache writeback/invalidation separated phases.

```text
COVERAGE unique_bytes=8388608 passes=6 total_bytes_written=50331648 total_bytes_verified=50331648 errors=0
PSRAM_TEST_RESULT=PASS
```

The tested RAM image SHA256 was `cf4667b34fb946fa359d1d2a0698d217252d29de913d41e54ed07484a124ed56`. It performed no flash-content writes. Factory firmware was restored by reset and responded to AT. This was an address/function test at 40 MHz under bench conditions, not a temperature/endurance qualification.

## Recovery evidence

The complete factory image was read locally and a final ROM digest verification matched all 4 MiB:

```text
Size:   4194304 bytes
SHA256: 94f7ace335cf7a7bf6ce7d9f9b78a7adacfd4c38895c602a1c863d51087f9600
```

The recovery image, private device identity comparisons and raw logs remain outside Git. Application/provisioning recovery copies taken during tracker commissioning are also private. Preserve them locally; a recovery copy is not evidence that a full restore has been exercised.

Identity checks used `ATI`, `AT+SIMCOMATI`, `AT+CGMR`, `AT+CGMM`, `AT+CSUB`, `AT+CQCNV`, plus SIM/radio/GNSS state queries. Unique identifiers were compared privately and are omitted from this report. A configured RF-band mask was not used to infer physical cellular-band support.

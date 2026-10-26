# Parts for the current tracker

The Wi-Fi PoC runs on the owned board. This list describes present requirements and unqualified vehicle-installation parts; it is not a purchasing recommendation or a price estimate.

## Bench PoC

| Part | Requirement |
|---|---|
| Main board | Owned LILYGO T-SIM7600X T54-V1.3, ESP32-WROVER-E, verified 4 MB flash / 8 MB physical PSRAM, SIM7600E-H |
| USB supply and data cable | Regulated 5 V, at least the board manufacturer's 2 A peak capability; USB-C **TTL** for ESP32 programming |
| GNSS antenna | Compatible active antenna and board-side coax connection, connected to **GPS** |
| Home Wi-Fi | Primary network and optional secondary network |
| Homelab | Home Assistant OS, Traccar app, dedicated MariaDB database and protected HTTPS ingestion |

See the [board specification](../board-specification.md) for connector/supply limits and [firmware setup](../../docs/setup/firmware-setup.md) for provisioning.

## LTE stage

Add a compatible active data SIM and a correctly matched LTE antenna on **MAIN**. Configure the actual carrier's APN and optional SIM PIN. Cellular registration, RF coverage and GNSS/PPP coexistence have not yet been qualified on this unit.

## Vehicle installation

| Part | Qualification needed |
|---|---|
| Automotive 12 V → 5 V supply | Fuse, reverse-polarity/transient protection, adequate peak current and measured quiescent consumption |
| Independent battery cutoff | Starter-battery protection with tested threshold and recovery behavior |
| Ignition interface | Protected isolated open-collector output to GPIO13, compatible with its internal pull-up |
| Harness and enclosure | Appropriate wiring, connectors, mechanical retention and environmental protection |
| Optional single-cell backup battery | Compatible rechargeable Li-ion/LiPo cell; verify source-switch resets and charging behavior |

The board's uncalibrated battery input is not used for energy-budget or battery-protection decisions. The vehicle power/interface design remains pending; see the [wiring guide](../../docs/wiring/wiring-guide.md).

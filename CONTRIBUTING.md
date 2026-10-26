# Contributing

This repository contains the ESP32 tracker firmware and its public setup documentation. Traccar/HA deployment is managed separately; this firmware sends OsmAnd HTTPS requests to that server.

## Workflow

1. Create a branch from `master`.
2. Keep changes within the four active components (`tracker_core`, `tracker_store`, `tracker_net`, `tracker_setup`) and the main scheduler where possible.
3. Run the checks below and update documentation when behavior changes.
4. Open a pull request against `master` with the behavior changed, validation performed and any hardware limitations.

## Checks

With GCC and make installed:

```sh
make -C firmware/test
# Expected: 8 C suites passed
node firmware/components/tracker_setup/test/test_setup_ui.cjs
python tools/test_provision_tracker.py
python tools/test_configure_tracker_access.py
```

On Windows, `tools/ci-local.ps1` runs the complete host/UI/provisioning/static checks using GCC, `mingw32-make`, cppcheck, Node.js and Python in PATH. Build the firmware separately in an **ESP-IDF v5.5.6** environment:

```sh
cd firmware
idf.py set-target esp32
idf.py build
```

GitHub Actions runs host checks, portable static analysis and the ESP32 build. Hardware acceptance is recorded in [the checklist](docs/acceptance-tests.md); a host test does not qualify LTE, power wiring or sleep current.

## Conventions

Use C11, four-space indentation and `snake_case`. Keep portable parsing/policy code independent of ESP-IDF. The network worker exclusively owns modem commands; web status handlers read snapshots. Preserve configuration/queue compatibility and never erase NVS automatically.

Do not commit credentials, factory NVS/flash images, raw location logs or generated binaries. Use synthetic coordinates in tests and redact identifiers and credentials from issue reports. Keep private provisioning files under the ignored `tools/provisioning-private/` directory.

# RAM-only full PSRAM diagnostic

This standalone project targets the original ESP32, verified silicon revision 3.1, using **ESP-IDF v5.2.5**. Build and load it into internal RAM only. It does not replace the installed flash application or use the tracker project. It overwrites all volatile PSRAM while testing.

## Coverage and pass criteria

- Auto-detect the PSRAM device; require exactly **8,388,608 bytes**. `SPIRAM_TYPE_AUTO` avoids forcing a desired capacity, and 2T mode is disabled.
- Keep code/data/stacks/heap in internal memory (`SPIRAM_USE_MEMMAP`, no external BSS/noinit).
- Reserve one 32 KiB himem window: direct physical range `0x000000–0x3F7FFF` (4,161,536 bytes), plus himem physical range `0x3F8000–0x7FFFFF` (4,227,072 bytes). These ranges are disjoint and cover all 8 MiB.
- Allocate the entire free himem region before testing. IDF v5.2.5 allocates its physical blocks in ascending order, which this standalone test uses for expected physical offsets.
- Write **every 32-bit word** across the entire array, then verify every word, for six patterns: zero, all-one, alternating AA, alternating 55, physical-word-address XOR, and its complement.
- Filling the whole array before verification with distinct physical-address patterns detects upper/lower address aliasing that an immediate write/read of each bank would miss.
- Write back and invalidate the cache between bank uses and phases. All executable state is internal RAM; single-core mode avoids cross-core cache access.
- Success requires all six `PASS_OK` lines, `COVERAGE unique_bytes=8388608 ... errors=0`, and **`PSRAM_TEST_RESULT=PASS`**. A panic, timeout, or missing final marker is not success.

This is a complete-address functional memory test at **40 MHz PSRAM** under the current bench conditions, not an endurance/temperature test or validation at 80 MHz.

## Build and run

With an ESP-IDF v5.2.5 environment active, run from this directory:

```text
idf.py set-target esp32
idf.py build
esptool --chip esp32 image-info build/psram_ram_test.bin
esptool --chip esp32 --port COM9 --baud 115200 --no-stub load-ram build/psram_ram_test.bin
```

Before loading, inspect generated `sdkconfig` and the image segments: `APP_BUILD_TYPE_RAM=y`, `APP_BUILD_TYPE_PURE_RAM_APP` disabled, correct PSRAM settings, and **only internal IRAM/DRAM segments**. Do not issue `flash`, `write-flash`, `erase-flash`, or `erase-region`. Capture UART0 at 115200 baud. The test waits two seconds in `app_main` for the monitor and remains idle after reporting PASS/FAIL. A normal hardware reset resumes the installed flash application.

The application supplies a strong no-op override for IDF's weak `bootloader_flash_unlock()` because no flash write is needed. Confirm the linked symbol/call sites when reviewing the ELF. DIO flash mode avoids enabling a flash QE status bit. Standard IDF RAM startup still initializes the flash/PSRAM bus; the project contains no flash-content writes, NVS, networking, modem, or relay code.

## Reference implementation details

- [ESP-IDF v5.2.5 RAM build configuration](https://docs.espressif.com/projects/esp-idf/en/v5.2.5/esp32/api-reference/kconfig.html#config-app-build-type): RAM application mode supports UART `load-ram`; pure-RAM mode would disable PSRAM and therefore must stay off.
- [Himem implementation](https://github.com/espressif/esp-idf/blob/v5.2.5/components/esp_psram/esp32/esp_himem.c): physical boundary, allocator order, and cache writeback during unmap.
- [PSRAM common configuration](https://github.com/espressif/esp-idf/blob/v5.2.5/components/esp_psram/Kconfig.spiram.common): unmanaged memory-map mode and no external heap allocation.
- [ESP32 PSRAM configuration](https://github.com/espressif/esp-idf/blob/v5.2.5/components/esp_psram/esp32/Kconfig.spiram): auto-detection, bank reservation, and 2T capacity caveat.
- [Cache writeback](https://github.com/espressif/esp-idf/blob/v5.2.5/components/esp_psram/esp32/esp_psram_extram_cache.c): write back before cache invalidation.

## Verified board run: 2026-10-26

The RAM diagnostic passed on the project's ESP32-WROVER-E board through COM9 at **40 MHz PSRAM**. It detected 8,388,608 physical bytes and tested every word in the 4,161,536-byte direct region plus the 4,227,072-byte himem region. All six patterns passed, for 50,331,648 bytes written and 50,331,648 bytes verified, with zero errors and the final `PSRAM_TEST_RESULT=PASS` marker. The board then reset into its existing factory application, which responded to `AT`.

The tested RAM image SHA-256 was `cf4667b34fb946fa359d1d2a0698d217252d29de913d41e54ed07484a124ed56`. Build timestamps or different tool environments may change a rebuilt image's hash. See the [board diagnostic record](../../hardware/diagnostics-2026-10-26.md) for the captured evidence and wider hardware results.

ESP-IDF's RAM startup uses a dummy image header, so this run printed a **1 MB flash image-header size** and warned that the detected 4096 KiB flash was larger. That header field is not a physical-capacity reading and does not change the flash contents. The diagnostic used DIO at runtime; the board's physical 4 MB flash was confirmed separately. The factory application's 4 MB PSRAM report likewise describes its mapped/usable memory; the RAM diagnostic verified all 8 MiB using bank switching.

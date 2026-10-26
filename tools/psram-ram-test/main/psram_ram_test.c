/*
 * Full-capacity PSRAM diagnostic, for ESP-IDF v5.2.5 / classic ESP32.
 * Load only into volatile internal RAM. No application flash/NVS writes.
 * The entire PSRAM contents are disposable during this standalone test.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_psram.h"
#include "esp_private/esp_psram_extram.h"
#include "esp32/himem.h"
#include "esp32/rom/cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"

#if !CONFIG_IDF_TARGET_ESP32 || !CONFIG_APP_BUILD_TYPE_RAM
#error This diagnostic must be a RAM-only classic ESP32 application.
#endif
#if CONFIG_APP_BUILD_TYPE_PURE_RAM_APP || !CONFIG_SPIRAM_USE_MEMMAP
#error PSRAM must be initialized but not added to the heap.
#endif
#if !CONFIG_FREERTOS_UNICORE || !CONFIG_SPIRAM_BANKSWITCH_ENABLE
#error This diagnostic requires single-core mode and himem.
#endif
#if CONFIG_SPIRAM_BANKSWITCH_RESERVE != 1 || CONFIG_SPIRAM_2T_MODE
#error Use one himem window and disable the capacity-reducing 2T mode.
#endif
#if !CONFIG_SPIRAM_TYPE_AUTO
#error Capacity must be auto-detected rather than forced by configuration.
#endif
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY || CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#error No application state may be placed in the PSRAM being tested.
#endif

#define EXPECTED_BYTES (8U * 1024U * 1024U)
#define MAPPED_BYTES (4U * 1024U * 1024U)
#define BLOCK_BYTES ESP_HIMEM_BLKSZ
#define PASS_COUNT 6U

static portMUX_TYPE cache_mux = portMUX_INITIALIZER_UNLOCKED;

/* Strong override of IDF's weak function: do not clear flash protection bits. */
esp_err_t IRAM_ATTR bootloader_flash_unlock(void)
{
    return ESP_OK;
}

/*
 * Force writes out to the PSRAM, then discard cached copies so verification
 * reads the physical array. All code, strings, stacks, and scheduler state are
 * internal RAM and CPU1 is disabled, so cache invalidation is safe here.
 */
static void IRAM_ATTR sync_and_invalidate_cache(void)
{
    portENTER_CRITICAL(&cache_mux);
    esp_psram_extram_writeback_cache();
    Cache_Read_Disable(0);
    Cache_Flush(0);
    Cache_Read_Enable(0);
    portEXIT_CRITICAL(&cache_mux);
}

static uint32_t expected_word(unsigned pass, uint32_t physical_word)
{
    switch (pass) {
    case 0: return 0x00000000U;
    case 1: return 0xFFFFFFFFU;
    case 2: return 0xAAAAAAAAU;
    case 3: return 0x55555555U;
    case 4: return physical_word ^ 0xA5A55A5AU;
    default: return ~(physical_word ^ 0xA5A55A5AU);
    }
}

static const char *pass_name(unsigned pass)
{
    static const char *names[PASS_COUNT] = {
        "all-zero", "all-one", "alternating-AA", "alternating-55",
        "physical-address", "inverse-physical-address"
    };
    return names[pass];
}

static void fill_block(volatile uint32_t *memory, size_t physical_offset,
                       unsigned pass)
{
    uint32_t word = (uint32_t)(physical_offset / sizeof(uint32_t));
    for (size_t i = 0; i < BLOCK_BYTES / sizeof(uint32_t); ++i) {
        memory[i] = expected_word(pass, word + (uint32_t)i);
    }
}

static bool verify_block(volatile uint32_t *memory, size_t physical_offset,
                         unsigned pass)
{
    uint32_t word = (uint32_t)(physical_offset / sizeof(uint32_t));
    for (size_t i = 0; i < BLOCK_BYTES / sizeof(uint32_t); ++i) {
        uint32_t expected = expected_word(pass, word + (uint32_t)i);
        uint32_t observed = memory[i];
        if (observed != expected) {
            printf("PSRAM_TEST_FAIL pass=%u physical=0x%08" PRIx32
                   " expected=0x%08" PRIx32 " observed=0x%08" PRIx32 "\n",
                   pass + 1, (uint32_t)(physical_offset + i * sizeof(uint32_t)),
                   expected, observed);
            return false;
        }
    }
    return true;
}

static bool api_ok(esp_err_t result, const char *operation)
{
    if (result == ESP_OK) {
        return true;
    }
    printf("PSRAM_TEST_FAIL operation=%s error=0x%x\n", operation, (unsigned)result);
    return false;
}

static _Noreturn void finish(bool success)
{
    printf("PSRAM_TEST_RESULT=%s\n", success ? "PASS" : "FAIL");
    fflush(stdout);
    /* Retain output and wait for the host's normal hardware reset. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    /* Allow a host switching from esptool to its monitor to capture results. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("PSRAM_DIAGNOSTIC_V1 RAM_ONLY NO_FLASH_WRITES\n");

    if (!esp_psram_is_initialized()) {
        printf("PSRAM_TEST_FAIL not_initialized\n");
        finish(false);
    }
    const size_t physical_bytes = esp_psram_get_size();
    const size_t reserved_bytes = esp_himem_reserved_area_size();
    const size_t high_bytes = esp_himem_get_phys_size();
    const size_t high_free_bytes = esp_himem_get_free_size();
    const size_t low_bytes = MAPPED_BYTES - reserved_bytes;
    printf("CAPACITY physical_bytes=%u direct_bytes=%u himem_bytes=%u"
           " himem_free_bytes=%u reserved_virtual_bytes=%u\n",
           (unsigned)physical_bytes, (unsigned)low_bytes, (unsigned)high_bytes,
           (unsigned)high_free_bytes, (unsigned)reserved_bytes);

    if (physical_bytes != EXPECTED_BYTES || reserved_bytes != BLOCK_BYTES ||
        low_bytes + high_bytes != physical_bytes || high_free_bytes != high_bytes ||
        low_bytes % BLOCK_BYTES != 0 || high_bytes % BLOCK_BYTES != 0) {
        printf("PSRAM_TEST_FAIL capacity_or_coverage_precondition\n");
        finish(false);
    }

    esp_himem_handle_t ram = NULL;
    esp_himem_rangehandle_t window = NULL;
    if (!api_ok(esp_himem_alloc_map_range(BLOCK_BYTES, &window), "allocate_window") ||
        !api_ok(esp_himem_alloc(high_bytes, &ram), "allocate_all_himem")) {
        finish(false);
    }

    for (unsigned pass = 0; pass < PASS_COUNT; ++pass) {
        printf("PASS_BEGIN number=%u pattern=%s\n", pass + 1, pass_name(pass));
        fflush(stdout);

        /* Fill the WHOLE array before verifying any part: catches aliases. */
        for (size_t offset = 0; offset < low_bytes; offset += BLOCK_BYTES) {
            volatile uint32_t *ptr = (volatile uint32_t *)(SOC_EXTRAM_DATA_LOW + offset);
            fill_block(ptr, offset, pass);
            vTaskDelay(1);
        }
        sync_and_invalidate_cache();

        for (size_t offset = 0; offset < high_bytes; offset += BLOCK_BYTES) {
            void *ptr = NULL;
            if (!api_ok(esp_himem_map(ram, window, offset, 0, BLOCK_BYTES, 0, &ptr), "map_write")) {
                finish(false);
            }
            fill_block((volatile uint32_t *)ptr, low_bytes + offset, pass);
            if (!api_ok(esp_himem_unmap(window, ptr, BLOCK_BYTES), "unmap_write")) {
                finish(false);
            }
            sync_and_invalidate_cache();
            vTaskDelay(1);
        }

        sync_and_invalidate_cache();
        for (size_t offset = 0; offset < low_bytes; offset += BLOCK_BYTES) {
            volatile uint32_t *ptr = (volatile uint32_t *)(SOC_EXTRAM_DATA_LOW + offset);
            if (!verify_block(ptr, offset, pass)) {
                finish(false);
            }
            vTaskDelay(1);
        }

        for (size_t offset = 0; offset < high_bytes; offset += BLOCK_BYTES) {
            void *ptr = NULL;
            if (!api_ok(esp_himem_map(ram, window, offset, 0, BLOCK_BYTES, 0, &ptr), "map_read")) {
                finish(false);
            }
            bool matched = verify_block((volatile uint32_t *)ptr, low_bytes + offset, pass);
            if (!api_ok(esp_himem_unmap(window, ptr, BLOCK_BYTES), "unmap_read") || !matched) {
                finish(false);
            }
            sync_and_invalidate_cache();
            vTaskDelay(1);
        }
        printf("PASS_OK number=%u bytes_written=%u bytes_verified=%u\n",
               pass + 1, (unsigned)physical_bytes, (unsigned)physical_bytes);
        fflush(stdout);
    }

    if (!api_ok(esp_himem_free(ram), "free_himem") ||
        !api_ok(esp_himem_free_map_range(window), "free_window")) {
        finish(false);
    }
    printf("COVERAGE unique_bytes=%u passes=%u total_bytes_written=%u total_bytes_verified=%u errors=0\n",
           (unsigned)physical_bytes, PASS_COUNT,
           (unsigned)(physical_bytes * PASS_COUNT), (unsigned)(physical_bytes * PASS_COUNT));
    finish(true);
}

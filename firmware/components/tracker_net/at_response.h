#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum { TRACKER_AT_WAIT, TRACKER_AT_OK, TRACKER_AT_ERROR } tracker_at_result_t;

static inline int tracker_at_terminal_line(const char *response, const char *token)
{
    size_t n = strlen(token);
    const char *p = response;
    while ((p = strstr(p, token)) != NULL) {
        int start = p == response || p[-1] == '\r' || p[-1] == '\n';
        int end = p[n] == '\r' || p[n] == '\n';
        if (start && end) return 1;
        ++p;
    }
    return 0;
}

/* esp_modem 1.4.1 presents accumulated snapshots on UART and, with its
 * inflatable-buffer option enabled, on CMUX too. Replace rather than append:
 * a callback can contain an earlier partial line followed by its completion. */
static inline tracker_at_result_t tracker_at_collect_snapshot(char *response, size_t capacity,
                                                             const uint8_t *data, size_t length)
{
    if (!response || !capacity || !data || length >= capacity) {
        if (response && capacity) response[0] = 0;
        return TRACKER_AT_ERROR;
    }
    memcpy(response, data, length);
    response[length] = 0;
    if (tracker_at_terminal_line(response, "ERROR") ||
        strstr(response, "+CME ERROR:") || strstr(response, "+CMS ERROR:"))
        return TRACKER_AT_ERROR;
    if (tracker_at_terminal_line(response, "OK")) return TRACKER_AT_OK;
    return TRACKER_AT_WAIT;
}

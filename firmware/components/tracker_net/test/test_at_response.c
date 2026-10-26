#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "at_response.h"
#include "tracker_core.h"

static void test_long_gnss_snapshots(void)
{
    /* Synthetic coordinates; UART can deliver an initial 64-byte snapshot
     * which already contains CR/LF but stops in the middle of the GPS line. */
    const char *position = "\r\n+CGPSINFO: 4730.123456,N,01903.123456,E,261026,120354.0,28.8,0.0,\r\n\r\nOK\r\n";
    char response[768];
    assert(tracker_at_collect_snapshot(response, sizeof(response), (const uint8_t *)position, 64) == TRACKER_AT_WAIT);
    assert(tracker_at_collect_snapshot(response, sizeof(response), (const uint8_t *)position, strlen(position)) == TRACKER_AT_OK);
    assert(!strcmp(response, position));
    tracker_fix_t fix;
    assert(tracker_parse_cgpsinfo(response, &fix) == TRACKER_PARSE_OK && fix.valid);

    const char *details = "\r\n+CGNSSINFO: 2,04,03,01,4730.123456,N,01903.123456,E,261026,120354.0,28.8,0.0,,2.1,1.5,\r\n\r\nOK\r\n";
    for (size_t n = 1; n < strlen(details) - 4; ++n)
        assert(tracker_at_collect_snapshot(response, sizeof(response), (const uint8_t *)details, n) == TRACKER_AT_WAIT);
    assert(tracker_at_collect_snapshot(response, sizeof(response), (const uint8_t *)details, strlen(details)) == TRACKER_AT_OK);
    assert(!strcmp(response, details));
    tracker_gnss_status_t status = {0};
    assert(tracker_parse_cgnssinfo(response, &status) == TRACKER_PARSE_OK);
    assert(status.gps_satellites == 4 && status.glonass_satellites == 3 && status.beidou_satellites == 1);
}

static void test_terminal_and_bounds(void)
{
    char response[64];
    const char *partial = "\r\n+VALUE: BOOK\r\n\r\nO";
    assert(tracker_at_collect_snapshot(response, sizeof(response), (const uint8_t *)partial, strlen(partial)) == TRACKER_AT_WAIT);
    const char *complete = "\r\n+VALUE: BOOK\r\n\r\nOK\r\n";
    assert(tracker_at_collect_snapshot(response, sizeof(response), (const uint8_t *)complete, strlen(complete)) == TRACKER_AT_OK);
    assert(!strcmp(response, complete));
    const char *errors[] = {"\r\nERROR\r\n", "\r\n+CME ERROR: SIM not inserted\r\n", "\r\n+CMS ERROR: 500\r\n"};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
        assert(tracker_at_collect_snapshot(response, sizeof(response), (const uint8_t *)errors[i], strlen(errors[i])) == TRACKER_AT_ERROR);
    char oversized[64]; memset(oversized, 'x', sizeof(oversized));
    assert(tracker_at_collect_snapshot(response, sizeof(response), (const uint8_t *)oversized, sizeof(oversized)) == TRACKER_AT_ERROR);
    assert(!response[0]);
    assert(tracker_at_collect_snapshot(response, sizeof(response), NULL, 0) == TRACKER_AT_ERROR);
}

int main(void)
{
    test_long_gnss_snapshots();
    test_terminal_and_bounds();
    puts("AT response: cumulative fragmented GNSS, terminal detection and bounds passed");
    return 0;
}

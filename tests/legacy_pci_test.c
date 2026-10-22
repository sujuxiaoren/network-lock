/* Host-only legacy write/STAT/GET tests. Every DIAG exchange is mocked. */
#include "../native/legacy_pci.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum scenario {
    OK, SEND_FAIL, EFS1_ERROR, EFS1_BUSY, EFS1_UNKNOWN,
    BAD_COMMAND, BAD_OPERATION, BAD_SEQUENCE, SHORT_RECEIPT, LONG_RECEIPT,
    OVERSIZE_RECEIPT, STAT_TRANSPORT, STAT_SHORT, STAT_LONG, STAT_OVERSIZE,
    STAT_HEADER, STAT_ERRNO, STAT_ITEM, STAT_DIRECTORY, STAT_SIZE, STAT_EMPTY,
    GET_TRANSPORT, GET_HEADER, GET_SEQUENCE, GET_MISSING, GET_DENIED,
    GET_SHORT_COUNT, GET_LONG_COUNT, GET_EMPTY, GET_WRONG_DATA
};
static enum scenario scenario;
static unsigned int calls;
static unsigned int frequency_path;
static const char pci_path[] = "/nv/item_files/modem/lte/rrc/csp/pci_lock";
static const char earfcn_path[] = "/nv/item_files/modem/lte/rrc/csp/earfcn_lock";
static const unsigned char cell_data[4] = {0x72, 0x06, 0xf1, 0x00};
static const unsigned char frequency_data[4] = {0x01, 0x00, 0x72, 0x06};
static int invoke(const unsigned char *data, uint32_t *status, char *error, size_t cap) {
    return frequency_path ? legacy_earfcn_put(data, status, error, cap) : legacy_pci_put(data, status, error, cap);
}

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void p32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value; p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16); p[3] = (unsigned char)(value >> 24);
}

int diag_exchange(const unsigned char *request, size_t size, unsigned char *reply,
                  size_t cap, size_t *actual, char *err, size_t errcap)
{
    unsigned int call = calls++;
    const char *path = frequency_path ? earfcn_path : pci_path;
    const unsigned char *data = frequency_path ? frequency_data : cell_data;
    size_t path_size = frequency_path ? sizeof earfcn_path : sizeof pci_path;
    assert(call < 3); /* No retry or PUT38 fallback is allowed. */
    memset(reply, 0, cap);
    if (call == 0) {
        static const unsigned char prefix[13] = {
            0x59, 0x05, 0, 0, 1, 4, 0, 0, 0, 0xff, 0, 0, 0
        };
        assert(size == (frequency_path ? 65u : 62u) && cap == 64);
        assert(memcmp(request, prefix, sizeof prefix) == 0);
        assert(path_size == (frequency_path ? 45u : 42u) && request[13] == path_size);
        assert(memcmp(request + 14, path, path_size) == 0);
        assert(request[frequency_path ? 59 : 56] == 4 && request[frequency_path ? 60 : 57] == 0);
        assert(memcmp(request + (frequency_path ? 61 : 58), data, 4) == 0);
        reply[0] = 0x59; reply[1] = 5; *actual = 4;
        switch (scenario) {
        case SEND_FAIL:
            (void)snprintf(err, errcap, "mock_send_failure:errno=5"); return -1;
        case EFS1_ERROR: reply[2] = 1; break;
        case EFS1_BUSY: reply[2] = 2; break;
        case EFS1_UNKNOWN: reply[2] = 255; break;
        case BAD_COMMAND: reply[0] = 0x4b; break;
        case BAD_OPERATION: reply[1] = 4; break;
        case BAD_SEQUENCE: reply[3] = 1; break;
        case SHORT_RECEIPT: *actual = 3; break;
        case LONG_RECEIPT: *actual = 5; break;
        case OVERSIZE_RECEIPT: *actual = cap + 1; break;
        default: break;
        }
    } else if (call == 1) {
        static const unsigned char header[4] = {0x4b, 0x13, 15, 0};
        assert(size == 4 + path_size && cap == 64);
        assert(memcmp(request, header, sizeof header) == 0);
        assert(memcmp(request + 4, path, path_size) == 0);
        memcpy(reply, header, sizeof header);
        p32(reply + 8, 0100777u); p32(reply + 12, 4); *actual = 32;
        switch (scenario) {
        case STAT_TRANSPORT:
            (void)snprintf(err, errcap, "mock_stat_timeout"); return -1;
        case STAT_SHORT: *actual = 31; break;
        case STAT_LONG: *actual = 33; break;
        case STAT_OVERSIZE: *actual = cap + 1; break;
        case STAT_HEADER: reply[1] = 0x3e; break;
        case STAT_ERRNO: p32(reply + 4, 2); break;
        case STAT_ITEM: p32(reply + 8, 0160777u); break;
        case STAT_DIRECTORY: p32(reply + 8, 0040777u); break;
        case STAT_SIZE: p32(reply + 12, 5); break;
        case STAT_EMPTY: p32(reply + 12, 0); break;
        default: break;
        }
    } else {
        static const unsigned char header[4] = {0x4b, 0x13, 39, 0};
        assert(size == 14 + path_size && cap == 270);
        assert(memcmp(request, header, sizeof header) == 0);
        assert(u32(request + 4) == 4 && u32(request + 8) == path_size);
        assert(request[12] != 0 || request[13] != 0);
        assert(memcmp(request + 14, path, path_size) == 0);
        memcpy(reply, header, sizeof header);
        memcpy(reply + 12, request + 12, 2);
        p32(reply + 4, 4); memcpy(reply + 14, data, 4); *actual = 18;
        switch (scenario) {
        case GET_TRANSPORT:
            (void)snprintf(err, errcap, "mock_get_timeout"); return -1;
        case GET_HEADER: reply[2] = 38; break;
        case GET_SEQUENCE: reply[12] ^= 1; break;
        case GET_MISSING: case GET_DENIED:
            p32(reply + 4, UINT32_MAX); p32(reply + 8, scenario == GET_MISSING ? 2u : 13u);
            *actual = 14; break;
        case GET_SHORT_COUNT: p32(reply + 4, 3); *actual = 17; break;
        case GET_LONG_COUNT: p32(reply + 4, 5); *actual = 19; break;
        case GET_EMPTY: p32(reply + 4, 0); *actual = 14; break;
        case GET_WRONG_DATA: reply[14] ^= 1; break;
        default: break;
        }
    }
    return 0;
}

int main(void)
{
    unsigned int tests = 0;
    uint32_t status;
    char error[512], tiny[2];
    for (frequency_path = 0; frequency_path < 2; ++frequency_path) {
    const unsigned char *data = frequency_path ? frequency_data : cell_data;
    for (scenario = OK; scenario <= GET_WRONG_DATA; scenario = (enum scenario)(scenario + 1)) {
        int result;
        unsigned int expected_calls = scenario == OK || scenario >= GET_TRANSPORT ? 3u :
                                      scenario >= STAT_TRANSPORT ? 2u : 1u;
        calls = 0; status = 123; memset(error, 0x5a, sizeof error);
        result = invoke(data, &status, error, sizeof error);
        assert(result == (scenario == OK ? 0 : -1));
        assert(calls == expected_calls && error[0] != '\0');
        assert(memchr(error, '\0', sizeof error) != NULL);
        if (scenario == EFS1_ERROR || scenario == EFS1_BUSY || scenario == EFS1_UNKNOWN) {
            assert(status == (scenario == EFS1_ERROR ? 1u : scenario == EFS1_BUSY ? 2u : 255u));
            assert(strstr(error, "efs1_status=") != NULL && strstr(error, "errno") == NULL);
            assert(strstr(error, "no_retry") != NULL);
        } else if (expected_calls == 1) {
            assert(status == UINT32_MAX && strstr(error, "no_retry") != NULL);
        } else {
            assert(status == 0 && strstr(error, "legacy_receipt=59050000") != NULL);
        }
        if (scenario == OK) assert(strstr(error, "verified_by_stat_and_get") != NULL);
        if (scenario == SEND_FAIL) assert(strstr(error, "mock_send_failure:errno=5") != NULL);
        if (scenario == STAT_TRANSPORT) assert(strstr(error, "mock_stat_timeout") != NULL);
        if (scenario == GET_TRANSPORT) assert(strstr(error, "mock_get_timeout") != NULL);
        if (scenario >= STAT_ITEM && scenario <= STAT_EMPTY)
            assert(strstr(error, "stat_requires_regular4") != NULL);
        if (scenario >= GET_SHORT_COUNT && scenario <= GET_EMPTY)
            assert(strstr(error, "item_size_mismatch") != NULL);
        ++tests;
    }
    calls = 0; status = 0;
    assert(invoke(NULL, &status, error, sizeof error) == -1);
    assert(status == UINT32_MAX && calls == 0);
    assert(invoke(data, NULL, error, sizeof error) == -1 && calls == 0);
    scenario = OK; calls = 0;
    assert(invoke(data, &status, NULL, 0) == 0 && calls == 3 && status == 0);
    calls = 0; tiny[0] = tiny[1] = 'X';
    assert(invoke(data, &status, tiny, 1) == 0 && calls == 3);
    assert(tiny[0] == '\0' && tiny[1] == 'X');
    scenario = GET_WRONG_DATA; calls = 0; tiny[0] = tiny[1] = 'X';
    assert(invoke(data, &status, tiny, 1) == -1 && calls == 3);
    assert(tiny[0] == '\0' && tiny[1] == 'X');
    scenario = EFS1_ERROR; calls = 0; tiny[0] = tiny[1] = 'X';
    assert(invoke(data, &status, tiny, 0) == -1 && calls == 1 && status == 1);
    assert(tiny[0] == 'X' && tiny[1] == 'X');
    }
    printf("legacy_pci: %u protocol scenarios plus argument/output bounds passed\n", tests);
    return 0;
}

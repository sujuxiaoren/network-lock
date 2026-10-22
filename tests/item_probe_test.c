/* Host-only protocol tests; DIAG is mocked and no device is contacted. */
#include "../native/item_probe.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum test_mode { NORMAL, EMPTY, MAXIMUM, MISSING, MISSING_NEGATIVE_COUNT,
    DENIED_WITH_COUNT, DENIED, RANGE_ERROR, TRANSPORT, SHORT,
    OVERSIZED_FRAME, WRONG_CMD, WRONG_SUBSYS, WRONG_SUBCMD, WRONG_SEQUENCE,
    TOO_MUCH_DATA, COUNT_MISMATCH, EXTRA_BYTE, ERROR_WITH_DATA,
    KNOWN_SHORT, KNOWN_LONG, KNOWN_ZERO };
static enum test_mode mode;
static unsigned int calls;
static unsigned int fault_index;
static uint16_t previous_sequence;
static const char *const expected_paths[] = {
    "/nv/item_files/modem/lte/rrc/csp/pci_lock",
    "/nv/item_files/modem/lte/rrc/efs/cell_restrict_opt_params",
    "/nv/item_files/modem/mmode/lte_bandpref",
    "/nv/item_files/modem/mmode/lte_bandpref_extn_65_256",
    "/nv/item_files/modem/lte/rrc/csp/earfcn_lock"
};
static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void p32(unsigned char *p, uint32_t v)
{
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
int diag_exchange(const unsigned char *req, size_t req_len, unsigned char *resp,
                  size_t resp_cap, size_t *resp_len, char *err, size_t errlen)
{
    size_t n, count = 4;
    uint16_t seq;
    enum test_mode active;
    assert(calls < 5);
    if (calls == 1) count = 36;
    assert(req_len == 14 + strlen(expected_paths[calls]) + 1);
    assert(req[0] == 0x4b && req[1] == 0x13 && req[2] == 39 && req[3] == 0);
    assert(u32(req + 4) == (calls == 0 ? 4u : calls == 1 ? 36u : calls == 4 ? 4u : 256u));
    assert(u32(req + 8) == req_len - 14);
    assert(memcmp(req + 14, expected_paths[calls], req_len - 14) == 0);
    seq = (uint16_t)((uint16_t)req[12] | ((uint16_t)req[13] << 8));
    assert(seq != 0 && seq != previous_sequence);
    previous_sequence = seq;
    active = calls == fault_index || mode <= RANGE_ERROR ? mode : NORMAL;
    /* Unknown-size item probes retain their prior bounded success handling. */
    if ((mode == EMPTY || mode == MAXIMUM) && calls < 2) active = NORMAL;
    ++calls;
    assert(resp_cap == 270);
    memset(resp, 0, resp_cap);
    memcpy(resp, req, 4);
    memcpy(resp + 12, req + 12, 2);
    if (active == EMPTY || active == MISSING || active == MISSING_NEGATIVE_COUNT ||
        active == DENIED_WITH_COUNT || active == DENIED || active == RANGE_ERROR ||
        active == KNOWN_ZERO) count = 0;
    if (active == MAXIMUM) count = 256;
    if (active == KNOWN_SHORT) --count;
    if (active == KNOWN_LONG) ++count;
    p32(resp + 4, (uint32_t)count);
    for (n = 0; n < count; ++n) resp[14 + n] = (unsigned char)n;
    *resp_len = 14 + count;
    switch (active) {
    case NORMAL: case EMPTY: case MAXIMUM:
    case KNOWN_SHORT: case KNOWN_LONG: case KNOWN_ZERO: break;
    case MISSING: p32(resp + 8, 2); break;
    case MISSING_NEGATIVE_COUNT: p32(resp + 4, UINT32_MAX); p32(resp + 8, 2); break;
    case DENIED_WITH_COUNT: p32(resp + 4, 256); p32(resp + 8, 13); break;
    case DENIED: p32(resp + 8, 13); break;
    case RANGE_ERROR: p32(resp + 4, UINT32_MAX); p32(resp + 8, 34); break;
    case TRANSPORT:
        (void)snprintf(err, errlen, "mock_receive_timeout:errno=110");
        return -1;
    case SHORT: *resp_len = 13; break;
    case OVERSIZED_FRAME: *resp_len = resp_cap + 1; break;
    case WRONG_CMD: resp[0] = 0x13; break;
    case WRONG_SUBSYS: resp[1] = 0x3e; break;
    case WRONG_SUBCMD: resp[3] = 1; break;
    case WRONG_SEQUENCE: resp[12] ^= 1; break;
    case TOO_MUCH_DATA: p32(resp + 4, UINT32_MAX); break;
    case COUNT_MISMATCH: p32(resp + 4, 3); break;
    case EXTRA_BYTE: ++*resp_len; break;
    case ERROR_WITH_DATA: p32(resp + 8, 2); break;
    }
    return 0;
}
int main(void)
{
    char output[ITEM_PROBE_SUMMARY_CAP];
    unsigned int tests = 0;
    for (mode = NORMAL; mode <= KNOWN_ZERO; mode = (enum test_mode)(mode + 1)) {
      for (fault_index = 0; fault_index < (mode <= RANGE_ERROR ? 1u : 2u); ++fault_index) {
        unsigned int index;
        for (index = 0; index < LOCK_ITEM_COUNT; ++index) {
            unsigned char data[LOCK_ITEM_LIMIT];
            size_t size = 999;
            uint32_t remote = 123;
            int result;
            int protocol_failure = mode > RANGE_ERROR && index == fault_index;
            calls = index; memset(data, 0xa5, sizeof data);
            result = item_get(index, data, &size, &remote, output, sizeof output);
            int exact_size_mismatch = index == 4 && (mode == EMPTY || mode == MAXIMUM);
            assert(calls == index + 1 && result == (protocol_failure || exact_size_mismatch ? -1 : 0));
            if (protocol_failure) {
                assert(size == 0 && remote == UINT32_MAX && data[0] == 0xa5);
                if (mode >= KNOWN_SHORT) assert(strstr(output, "item_size_mismatch") != NULL);
                if (mode == TRANSPORT) assert(strstr(output, "transport:mock_receive_timeout:errno=110") != NULL);
            } else if (exact_size_mismatch) {
                assert(size == 0 && remote == UINT32_MAX && data[0] == 0xa5);
                assert(strstr(output, "item_size_mismatch") != NULL);
            } else if (mode >= MISSING && mode <= RANGE_ERROR) {
                assert(size == 0 && data[0] == 0xa5);
                assert(remote == (mode <= MISSING_NEGATIVE_COUNT ? 2u : mode == RANGE_ERROR ? 34u : 13u));
            } else {
                size_t expected = index == 1 ? 36u : 4u;
                if (index >= 2 && mode == EMPTY) expected = 0;
                if (index >= 2 && mode == MAXIMUM) expected = LOCK_ITEM_LIMIT;
                assert(remote == 0 && size == expected);
                if (expected) assert(data[0] == 0 && data[expected - 1] == (unsigned char)(expected - 1));
                else assert(data[0] == 0xa5);
            }
        }
        ++tests;
      }
    }
    /* A well-formed successful frame of the wrong known size must never be
     * published to the caller, including an empty or one-byte-longer item. */
    for (mode = KNOWN_SHORT; mode <= KNOWN_ZERO; mode = (enum test_mode)(mode + 1)) {
        for (fault_index = 0; fault_index < 5; ++fault_index) {
            if (fault_index == 2 || fault_index == 3) continue;
            unsigned char data[LOCK_ITEM_LIMIT];
            size_t size = 123; uint32_t remote = 0;
            calls = fault_index; memset(data, 0xa5, sizeof data);
            assert(item_get(fault_index, data, &size, &remote, output, sizeof output) == -1);
            assert(calls == fault_index + 1 && size == 0 && remote == UINT32_MAX);
            assert(data[0] == 0xa5 && data[sizeof data - 1] == 0xa5);
            assert(strstr(output, "item_size_mismatch") != NULL);
            ++tests;
        }
    }
    calls = 0;
    {
        unsigned char data[LOCK_ITEM_LIMIT]; size_t size; uint32_t remote;
        assert(item_get(0, NULL, &size, &remote, output, sizeof output) == -1);
        assert(item_get(0, data, NULL, &remote, output, sizeof output) == -1);
        assert(item_get(0, data, &size, NULL, output, sizeof output) == -1);
        assert(item_get(ALL_LOCK_ITEM_COUNT, data, &size, &remote, output, sizeof output) == -1);
        assert(calls == 0);
    }
    /* Snapshot/probe summary and output-bound cases live in item_snapshot_test. */
    printf("item_probe: %u protocol cases plus null/bounds cases passed\n", tests);
    return 0;
}

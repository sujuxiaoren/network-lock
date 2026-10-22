/* Host-only stateful protocol tests. Every modem exchange is simulated. */
#include "../native/item_probe.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { LockItemSnapshot value; uint32_t links; } MockItem;
static MockItem items[LOCK_ITEM_COUNT];
static const char *const paths[LOCK_ITEM_COUNT] = {
    "/nv/item_files/modem/lte/rrc/csp/pci_lock",
    "/nv/item_files/modem/lte/rrc/efs/cell_restrict_opt_params",
    "/nv/item_files/modem/mmode/lte_bandpref",
    "/nv/item_files/modem/mmode/lte_bandpref_extn_65_256",
    "/nv/item_files/modem/lte/rrc/csp/earfcn_lock"
};
enum Fault { NONE, TRANSPORT, SHORT_REPLY, LONG_REPLY, WRONG_HEADER, OVERSIZE,
    DENIED, RANGE_ERROR, WRONG_SEQUENCE, WRONG_COUNT, ERROR_WITH_DATA,
    TOO_LARGE, WRONG_MODE, FLIP_BYTE, PRESENT_CHANGED };
static enum Fault fault;
static unsigned fault_command, fault_occurrence, fault_seen, fault_after_write;
static unsigned calls, writes, unlinks, legacy_writes, regular_writes, put_calls, tests;
static unsigned open_index;
static uint32_t put_mode;

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void p32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void set_item(unsigned index, uint32_t mode, uint32_t length, unsigned char byte) {
    assert(index < LOCK_ITEM_COUNT && length <= LOCK_ITEM_LIMIT);
    memset(&items[index], 0, sizeof items[index]);
    items[index].value.present = 1; items[index].value.mode = mode;
    items[index].value.length = length;
    memset(items[index].value.data, byte, length);
    items[index].links = (mode & 0170000u) == 0100000u ? 1 : 0;
}
static void reset(void) {
    memset(items, 0, sizeof items);
    set_item(1, 0160777u, 36, 0); set_item(2, 0160777u, 8, 0xff);
    fault = NONE; fault_command = 0; fault_occurrence = 1; fault_seen = 0;
    fault_after_write = 0; calls = writes = unlinks = legacy_writes = regular_writes = put_calls = 0;
    open_index = LOCK_ITEM_COUNT;
    put_mode = 0160777u;
}
static unsigned path_index(const unsigned char *request, size_t size, size_t offset) {
    unsigned i;
    assert(offset < size);
    for (i = 0; i < LOCK_ITEM_COUNT; ++i) {
        size_t n = strlen(paths[i]) + 1;
        if (size - offset == n && memcmp(request + offset, paths[i], n) == 0) return i;
    }
    assert(!"Non-allowlisted path"); return LOCK_ITEM_COUNT;
}

int diag_exchange(const unsigned char *request, size_t size, unsigned char *reply,
                  size_t cap, size_t *actual, char *err, size_t errcap) {
    unsigned command, index = 0;
    uint32_t length = 0, status = 0;
    enum Fault active = NONE;
    MockItem *item;
    ++calls; assert(calls < 500 && size >= 4 && cap >= 24);
    memset(reply, 0, cap);
    if (request[0] == 0x59) {
        assert(size == 62 && request[1] == 5 && request[2] == 0 && request[3] == 0);
        assert(memcmp(request + 14, paths[0], strlen(paths[0]) + 1) == 0);
        assert(request[4] == 1 && u32(request + 5) == 4 && request[56] == 4);
        set_item(0, 0100001u, 4, 0); memcpy(items[0].value.data, request + 58, 4);
        ++writes; ++legacy_writes; command = 0x59;
        reply[0] = 0x59; reply[1] = 5; *actual = 4;
    } else {
        assert(request[0] == 0x4b && request[1] == 0x13 && request[3] == 0);
        command = request[2]; memcpy(reply, request, 4);
        switch (command) {
        case 15: case 16: case 8: index = path_index(request, size, 4); break;
        case 39:
            assert(size >= 14 && u32(request + 4) <= LOCK_ITEM_LIMIT);
            index = path_index(request, size, 14);
            assert(u32(request + 8) == size - 14);
            break;
        case 38:
            length = u32(request + 4); assert(length > 0 && length <= LOCK_ITEM_LIMIT);
            assert(u32(request + 8) == 0x00080040u);
            index = path_index(request, size, 14 + length); break;
        case 2:
            index = path_index(request, size, 12);
            /* Existing regular file updates must neither create nor truncate. */
            assert((index == 0 || index == 4) && u32(request + 4) == 2 && u32(request + 8) == 0);
            open_index = index;
            break;
        case 3: case 5: assert(u32(request + 4) == 7); break;
        default: assert(!"Unexpected command");
        }
        item = &items[index];
        if (!item->value.present) status = 2;
        switch (command) {
        case 15: case 16:
            *actual = command == 15 ? 32 : 24; p32(reply + 4, status);
            if (!status) {
                p32(reply + 8, item->value.mode);
                if (command == 15) {
                    p32(reply + 12, item->value.length); p32(reply + 16, item->links);
                }
            }
            break;
        case 39:
            memcpy(reply + 12, request + 12, 2);
            if (!status && u32(request + 4) != item->value.length) status = 34;
            p32(reply + 8, status);
            if (status) { p32(reply + 4, UINT32_MAX); *actual = 14; }
            else {
                p32(reply + 4, item->value.length); *actual = 14 + item->value.length;
                memcpy(reply + 14, item->value.data, item->value.length);
            }
            break;
        case 38:
            set_item(index, put_mode, length, 0);
            memcpy(item->value.data, request + 14, length);
            ++writes; ++put_calls; *actual = 10; memcpy(reply + 4, request + 12, 2); break;
        case 8:
            memset(item, 0, sizeof *item); ++writes; ++unlinks; *actual = 8; break;
        case 2:
            assert(item->value.present && item->value.mode == 0100001u && item->value.length == 4);
            p32(reply + 4, 7); *actual = 12; break;
        case 5:
            assert(size == 16 && u32(request + 8) == 0);
            assert(open_index == 0 || open_index == 4);
            memcpy(items[open_index].value.data, request + 12, 4);
            p32(reply + 4, 7); p32(reply + 12, 4); *actual = 20;
            ++writes; ++regular_writes; break;
        case 3: assert(size == 8); *actual = 8; break;
        default: assert(0);
        }
    }
    if (fault != NONE && command == fault_command && (!fault_after_write || writes)) {
        ++fault_seen;
        if (fault_seen == fault_occurrence) active = fault;
    }
    switch (active) {
    case NONE: break;
    case TRANSPORT:
        if (err != NULL && errcap) (void)snprintf(err, errcap, "mock_timeout"); return -1;
    case SHORT_REPLY: --*actual; break;
    case LONG_REPLY: ++*actual; break;
    case WRONG_HEADER: reply[1] ^= 1; break;
    case OVERSIZE: *actual = cap + 1; break;
    case DENIED: case RANGE_ERROR:
        if (command == 39) {
            p32(reply + 4, UINT32_MAX); p32(reply + 8, active == DENIED ? 13u : 34u); *actual = 14;
        } else p32(reply + 4, active == DENIED ? 13u : 34u);
        break;
    case WRONG_SEQUENCE: assert(command == 39); reply[12] ^= 1; break;
    case WRONG_COUNT: assert(command == 39); p32(reply + 4, u32(reply + 4) + 1); break;
    case ERROR_WITH_DATA: assert(command == 39 && *actual > 14); p32(reply + 8, 2); break;
    case TOO_LARGE: assert(command == 15); p32(reply + 12, LOCK_ITEM_LIMIT + 1); break;
    case WRONG_MODE: assert(command == 15 || command == 16); p32(reply + 8, 0120777u); break;
    case FLIP_BYTE: assert(command == 39 && *actual > 14); reply[14] ^= 1; break;
    case PRESENT_CHANGED:
        assert(command == 16); p32(reply + 4, 2); memset(&items[index], 0, sizeof items[index]); break;
    }
    return 0;
}

static void snapshot_ok(unsigned index) {
    LockItemSnapshot value;
    char error[512];
    unsigned before = calls;
    assert(item_snapshot(index, &value, error, sizeof error) == 0);
    assert(calls == before + 6 && writes == 0);
    assert(memcmp(&value, &items[index].value, sizeof value) == 0); ++tests;
}
static void snapshot_fails(unsigned index, const char *message) {
    LockItemSnapshot value, untouched;
    char error[512];
    memset(&value, 0xa5, sizeof value); untouched = value;
    assert(item_snapshot(index, &value, error, sizeof error) == -1);
    assert(memcmp(&value, &untouched, sizeof value) == 0 && writes == 0);
    if (strstr(error, message) == NULL) {
        fprintf(stderr, "case %u error=%s expected=%s\n", tests, error, message); assert(0);
    }
    ++tests;
}
static void fault_case(unsigned command, enum Fault injected, unsigned occurrence,
                        const char *message) {
    reset(); fault_command = command; fault = injected; fault_occurrence = occurrence;
    snapshot_fails(2, message);
}
static void typed_write(unsigned index, uint32_t mode, size_t length, int success) {
    unsigned char data[LOCK_ITEM_LIMIT];
    LockItemSnapshot expected_before = items[index].value;
    uint32_t remote = 123;
    char error[512];
    int result;
    memset(data, 0x5a, sizeof data);
    result = item_put_typed(index, data, length, mode, &expected_before,
                            &remote, error, sizeof error);
    if (result != (success ? 0 : -1)) {
        fprintf(stderr, "typed case %u result=%d error=%s\n", tests, result, error); assert(0);
    }
    assert(unlinks == 0);
    if (success) {
        assert(remote == 0 && writes == 1);
        assert(items[index].value.present == 1 && items[index].value.mode == mode);
        assert(items[index].value.length == length && !memcmp(items[index].value.data, data, length));
    }
    ++tests;
}

int main(void) {
    unsigned i, command;
    char error[ITEM_PROBE_SUMMARY_CAP], tiny[2];
    LockItemSnapshot value;
    unsigned char data[LOCK_ITEM_LIMIT];
    uint32_t remote;
    size_t length;

    reset(); for (i = 0; i < LOCK_ITEM_COUNT; ++i) snapshot_ok(i);
    reset(); set_item(0, 0100001u, 4, 0x72); snapshot_ok(0);
    reset(); set_item(2, 0160777u, 0, 0); snapshot_ok(2);
    assert(item_snapshot_supported(2, &items[2].value, error, sizeof error) == -1); ++tests;
    reset(); set_item(4, 0160777u, LOCK_ITEM_LIMIT, 0xff); snapshot_ok(4);
    reset(); set_item(0, 0100001u, 0, 0); snapshot_ok(0);
    reset(); items[0] = items[2]; items[0].value.mode = 0100001u; items[0].links = 2;
    snapshot_fails(0, "regular_link_count");
    reset(); set_item(2, 0040777u, 8, 0); snapshot_fails(2, "unsupported_type");
    for (command = 15; command <= 16; ++command) {
        fault_case(command, TRANSPORT, 1, "transport");
        fault_case(command, SHORT_REPLY, 1, "response_length");
        fault_case(command, LONG_REPLY, 1, "response_length");
        fault_case(command, OVERSIZE, 1, "response_length");
        fault_case(command, WRONG_HEADER, 1, "header_mismatch");
        fault_case(command, DENIED, 1, "errno=13");
        fault_case(command, RANGE_ERROR, 1, "errno=34");
    }
    fault_case(15, TOO_LARGE, 1, "length_out_of_range");
    fault_case(16, WRONG_MODE, 1, "symlink_rejected");
    fault_case(15, WRONG_MODE, 1, "lstat_stat_disagree");
    fault_case(39, TRANSPORT, 1, "transport");
    fault_case(39, SHORT_REPLY, 1, "count_length_mismatch");
    fault_case(39, LONG_REPLY, 1, "count_length_mismatch");
    fault_case(39, OVERSIZE, 1, "response_length");
    fault_case(39, WRONG_HEADER, 1, "header_mismatch");
    fault_case(39, WRONG_SEQUENCE, 1, "sequence_mismatch");
    fault_case(39, WRONG_COUNT, 1, "count_length_mismatch");
    fault_case(39, ERROR_WITH_DATA, 1, "error_with_data");
    fault_case(39, DENIED, 1, "errno=13");
    fault_case(39, RANGE_ERROR, 1, "errno=34");
    fault_case(39, FLIP_BYTE, 2, "changed_between_reads");
    fault_case(16, PRESENT_CHANGED, 2, "changed_between_reads");

    reset(); memset(&value, 0, sizeof value);
    for (i = 0; i < LOCK_ITEM_COUNT; ++i) assert(item_snapshot_supported(i, &value, error, sizeof error) == 0);
    value.present = 2; assert(item_snapshot_supported(0, &value, error, sizeof error) == -1);
    value.present = 0; value.mode = 0160777u;
    assert(item_snapshot_supported(0, &value, error, sizeof error) == -1);
    for (i = 0; i < LOCK_ITEM_COUNT; ++i) {
        set_item(i, 0160777u, i == 0 ? 4u : i == 1 ? 36u : 8u, 0);
        assert(item_snapshot_supported(i, &items[i].value, error, sizeof error) == 0);
        items[i].value.mode = 0160700u;
        assert(item_snapshot_supported(i, &items[i].value, error, sizeof error) == -1);
        items[i].value.mode = 0100001u;
        assert(item_snapshot_supported(i, &items[i].value, error, sizeof error) == (i == 0 ? 0 : -1));
        ++tests;
    }
    set_item(4, 0100001u, 4, 0);
    assert(item_snapshot_supported(4, &items[4].value, error, sizeof error) == 0);
    set_item(4, 0100001u, 3, 0);
    assert(item_snapshot_supported(4, &items[4].value, error, sizeof error) == -1);
    set_item(4, 0100001u, 5, 0);
    assert(item_snapshot_supported(4, &items[4].value, error, sizeof error) == -1);
    ++tests;
    reset(); set_item(0, 0160777u, 3, 0);
    assert(item_snapshot_supported(0, &items[0].value, error, sizeof error) == -1);
    set_item(1, 0160777u, 37, 0);
    assert(item_snapshot_supported(1, &items[1].value, error, sizeof error) == -1);
    assert(item_snapshot_supported(ALL_LOCK_ITEM_COUNT, &value, NULL, 0) == -1);
    assert(item_snapshot_supported(0, NULL, NULL, 0) == -1); ++tests;

    reset(); typed_write(2, 0160777u, 8, 1); assert(put_calls == 1);
    reset(); typed_write(4, 0160777u, LOCK_ITEM_LIMIT, 1); assert(put_calls == 1);
    reset(); typed_write(0, 0100001u, 4, 1); assert(legacy_writes == 1 && !regular_writes);
    reset(); set_item(0, 0100001u, 4, 0); typed_write(0, 0100001u, 4, 1);
    assert(regular_writes == 1 && !legacy_writes);
    reset(); typed_write(1, 0160777u, 36, 1);
    reset(); set_item(0, 0160777u, 4, 0); typed_write(0, 0100001u, 4, 0); assert(!writes);
    reset(); set_item(0, 0100001u, 4, 0); typed_write(0, 0160777u, 4, 0); assert(!writes);
    reset(); set_item(4, 0100001u, 4, 0); typed_write(4, 0100001u, 4, 1);
    assert(regular_writes == 1 && !legacy_writes);
    reset(); set_item(4, 0100001u, 4, 0); typed_write(4, 0160777u, 4, 0); assert(!writes);
    reset(); set_item(4, 0160777u, 4, 0); typed_write(4, 0100001u, 4, 0); assert(!writes);
    reset(); set_item(4, 0100001u, 4, 0); typed_write(4, 0100001u, 3, 0); assert(!calls);
    reset(); set_item(2, 0160700u, 8, 0); typed_write(2, 0160777u, 8, 0); assert(!writes);
    reset(); typed_write(2, 0160777u, 0, 0); assert(!calls);
    reset(); typed_write(1, 0160777u, 35, 0); assert(!calls);
    reset(); typed_write(0, 0100777u, 4, 0); assert(!calls);
    reset(); fault_command = 38; fault = TRANSPORT;
    typed_write(2, 0160777u, 8, 0); assert(writes == 1 && put_calls == 1);
    reset(); fault_command = 5; fault = SHORT_REPLY; set_item(0, 0100001u, 4, 0);
    typed_write(0, 0100001u, 4, 0); assert(regular_writes == 1 && !legacy_writes);
    reset(); fault_command = 3; fault = DENIED; set_item(0, 0100001u, 4, 0);
    typed_write(0, 0100001u, 4, 0); assert(regular_writes == 1);
    reset(); fault_command = 5; fault = SHORT_REPLY; set_item(4, 0100001u, 4, 0);
    typed_write(4, 0100001u, 4, 0); assert(regular_writes == 1 && !legacy_writes);
    reset(); fault_command = 3; fault = DENIED; set_item(4, 0100001u, 4, 0);
    typed_write(4, 0100001u, 4, 0); assert(regular_writes == 1);
    reset(); put_mode = 0100001u;
    typed_write(2, 0160777u, 8, 0); assert(writes == 1);
    reset(); put_mode = 0160700u;
    typed_write(2, 0160777u, 8, 0); assert(writes == 1);
    reset(); fault_command = 39; fault = FLIP_BYTE; fault_after_write = 1;
    typed_write(2, 0160777u, 8, 0); assert(writes == 1);

    /* The caller's already-confirmed state must remain the comparison source.
     * Seeing a different same-mode/same-length value must stop the mutation. */
    reset(); value = items[2].value; items[2].value.data[3] ^= 1;
    memset(data, 0x5a, sizeof data);
    assert(item_put_typed(2, data, 8, 0160777u, &value, &remote, error, sizeof error) == -1);
    assert(strstr(error, "expected_state_changed") && calls == 6 && writes == 0); ++tests;
    reset(); set_item(0, 0100001u, 4, 0); value = items[0].value; items[0].value.data[0] = 1;
    assert(item_put_typed(0, data, 4, 0100001u, &value, &remote, error, sizeof error) == -1);
    assert(strstr(error, "expected_state_changed") && writes == 0 && !regular_writes); ++tests;
    reset(); set_item(4, 0100001u, 4, 0); value = items[4].value; items[4].value.data[0] = 1;
    assert(item_put_typed(4, data, 4, 0100001u, &value, &remote, error, sizeof error) == -1);
    assert(strstr(error, "expected_state_changed") && writes == 0 && !regular_writes); ++tests;
    reset(); memset(&value, 0, sizeof value);
    assert(item_put_typed(2, data, 8, 0160777u, &value, &remote, error, sizeof error) == -1);
    assert(strstr(error, "expected_state_changed") && !writes); ++tests;
    reset(); set_item(4, 0100001u, 4, 0); value = items[4].value; memset(&items[4], 0, sizeof items[4]);
    assert(item_put_typed(4, data, 4, 0100001u, &value, &remote, error, sizeof error) == -1);
    assert(strstr(error, "expected_state_changed") && !writes); ++tests;
    reset(); value = items[2].value; memset(&items[2], 0, sizeof items[2]);
    assert(item_put_typed(2, data, 8, 0160777u, &value, &remote, error, sizeof error) == -1);
    assert(strstr(error, "expected_state_changed") && !writes); ++tests;

    reset(); value = items[2].value; items[2].value.data[2] ^= 1;
    assert(item_unlink_checked(2, &value, &remote, error, sizeof error) == -1);
    assert(strstr(error, "expected_state_changed") && calls == 6 && !writes); ++tests;
    reset(); value = items[2].value;
    assert(item_unlink_checked(2, &value, &remote, error, sizeof error) == 0);
    assert(writes == 1 && unlinks == 1 && !items[2].value.present && calls == 8); ++tests;
    reset(); memset(&value, 0, sizeof value);
    assert(item_unlink_checked(0, &value, &remote, error, sizeof error) == 0);
    assert(remote == 2 && calls == 6 && !writes); ++tests;
    reset(); value = items[2].value; memset(&items[2], 0, sizeof items[2]);
    assert(item_unlink_checked(2, &value, &remote, error, sizeof error) == -1);
    assert(strstr(error, "expected_state_changed") && !writes); ++tests;

    /* Explicit PUT readback must request the actual 8-byte length, not 256. */
    reset(); memset(data, 0xf1, sizeof data);
    assert(item_put(2, data, 8, &remote, error, sizeof error) == 0 && remote == 0 && calls == 2);
    reset(); memset(data, 0xa5, sizeof data); length = 55; remote = 0;
    assert(item_get(2, data, &length, &remote, error, sizeof error) == 0);
    assert(remote == 34 && length == 0 && data[0] == 0xa5); ++tests;

    reset(); assert(probe_lock_items(error, sizeof error) == 0 && calls == 30);
    assert(strstr(error, ":0:8:ffffffffffffffff\n") && strstr(error, ":0:36:00000000"));
    assert(strstr(error, ":2:0:\n") && !strstr(error, "ITEM_ERROR:")); ++tests;
    reset(); fault_command = 16; fault = DENIED;
    assert(probe_lock_items(error, sizeof error) == 0);
    assert(strstr(error, ":13:0:\n") && !strstr(error, "ITEM_ERROR:")); ++tests;
    reset(); fault_command = 39; fault = RANGE_ERROR; fault_occurrence = 5;
    assert(probe_lock_items(error, sizeof error) == 0 && strstr(error, ":34:0:\n")); ++tests;
    reset(); fault_command = 16; fault = SHORT_REPLY;
    assert(probe_lock_items(error, sizeof error) == -1 && strstr(error, "ITEM_ERROR:")); ++tests;
    reset(); assert(probe_lock_items(NULL, sizeof error) == -1 && calls == 0);
    assert(probe_lock_items(error, 0) == -1 && calls == 0);
    tiny[0] = 'x'; tiny[1] = 'y';
    assert(probe_lock_items(tiny, 1) == -1 && calls == 30 && tiny[0] == 0 && tiny[1] == 'y');
    reset(); assert(item_snapshot(ALL_LOCK_ITEM_COUNT, &value, tiny, 1) == -1 && tiny[0] == 0 && calls == 0);
    assert(item_snapshot(0, NULL, NULL, 0) == -1 && calls == 0);
    assert(item_put_typed(0, NULL, 4, 0100001u, &value, &remote, tiny, 1) == -1 && !calls);
    assert(item_put_typed(0, data, 4, 0100001u, &value, NULL, NULL, 0) == -1 && !calls);
    assert(item_put_typed(0, data, 4, 0100001u, NULL, &remote, NULL, 0) == -1 && !calls);
    assert(item_unlink_checked(0, NULL, &remote, NULL, 0) == -1 && !calls);
    assert(item_unlink_checked(0, &value, NULL, NULL, 0) == -1 && !calls);
    ++tests;
    printf("item_snapshot: %u stateful scenarios plus bounds checks passed\n", tests);
    return 0;
}

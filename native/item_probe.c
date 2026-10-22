/*
 * Independent EFS2 item wire implementation. GET layout corroboration:
 * bkerler/edl commit 1cda1a616748d608667b2d13b73a28d3197ceb45,
 * edlclient/Tools/qc_diag.py, efs_get (lines 978-991), retrieved 2026-10-11.
 * Request: header[4], LE32 item_bytes, LE32 path_bytes including NUL,
 *          LE16 sequence, pathNUL.
 * Response: header[4], LE32 num_bytes, LE32 errno, LE16 sequence, data.
 * item_bytes must equal the complete item size, not a maximum capacity.
 * Qualcomm's efs_get contract: boot_images/QcomPkg/Include/api/storage/fs_lib.h,
 * lines 147-175, in David112x/android-firmware-qti-sdm660 commit
 * b5e5bb9ed6c92442fd502b3e726d7c8024dc0763. Both smaller and larger reads fail.
 * The APK also requests a known 36-byte item at dcidaemon 0x363d0..0x363d8.
 * No source implementation is copied; only the protocol layout is used.
 * On remote error, num_bytes is not a valid data length. The connected modem
 * returns 0xffffffff with errno 2 and an exact 14-byte response. Validate the
 * header/sequence and error-only size, then report the remote error.
 */
#include "item_probe.h"
#include "transport.h"
#include "efs.h"
#include "legacy_pci.h"

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { ITEM_LIMIT = 256, GET_HEADER_SIZE = 14 };
static const char *const paths[] = {
    "/nv/item_files/modem/lte/rrc/csp/pci_lock",
    "/nv/item_files/modem/lte/rrc/efs/cell_restrict_opt_params",
    "/nv/item_files/modem/mmode/lte_bandpref",
    "/nv/item_files/modem/mmode/lte_bandpref_extn_65_256",
    "/nv/item_files/modem/lte/rrc/csp/earfcn_lock",
    "/nv/item_files/modem/nr5g/RRC/earfcn_lock",
    "/nv/item_files/modem/nr5g/RRC/pci_lock_info",
    "/nv/item_files/modem/mmode/nr_band_pref",
    "/nv/item_files/modem/mmode/nr_nsa_band_pref"
};
/* Only these three item sizes are used by the experimental write profile.
 * Zero means unknown: retain the 256-byte absence probe for the other paths.
 * That request is not evidence that an existing item has size 256. */
static const uint32_t known_item_sizes[ALL_LOCK_ITEM_COUNT] = {4, 36, 0, 0, 4, 256, 0, 0, 0};
static uint16_t next_sequence = 1;

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static int append_line(char *summary, size_t cap, size_t *used, const char *line)
{
    size_t n = strlen(line);
    if (*used >= cap || n >= cap - *used)
        return -1;
    memcpy(summary + *used, line, n + 1);
    *used += n;
    return 0;
}

static int fail(char *err, size_t cap, const char *fmt, ...)
{
    if (err != NULL && cap != 0) {
        va_list args;
        va_start(args, fmt);
        (void)vsnprintf(err, cap, fmt, args);
        va_end(args);
        err[cap - 1] = '\0';
    }
    return -1;
}

static int parent_directories_ready(const char *const *parents, size_t count, char *err, size_t cap)
{
    /* EFS2 STAT layout: P1sec/QCSuper commit
     * 9582557c8f6e4baaf9da05b3831bf1a364e977a3, stat.py; also used by efs.c. */
    size_t i;
    if (err != NULL && cap != 0) err[0] = '\0';
    for (i = 0; i < count; ++i) {
      unsigned int pass;
      uint32_t original_mode = 0;
      for (pass = 0; pass < 2; ++pass) {
        unsigned char request[4 + 128], response[64];
        char detail[160] = {0};
        const char *operation = pass == 0 ? "lstat" : "stat";
        size_t path_size = strlen(parents[i]) + 1, response_size = 0;
        uint32_t status, mode;
        if (path_size > 128) return fail(err, cap, "parent_stat_path_length");
        request[0] = 0x4b; request[1] = 0x13; request[2] = pass == 0 ? 16 : 15; request[3] = 0;
        memcpy(request + 4, parents[i], path_size);
        if (diag_exchange(request, 4 + path_size, response, sizeof response,
                          &response_size, detail, sizeof detail) != 0)
            return fail(err, cap, "parent_%s_transport:%s:%s", operation, parents[i], detail);
        if (response_size != (pass == 0 ? 24u : 32u))
            return fail(err, cap, "parent_%s_response_length:%s:actual_len=%zu",
                        operation, parents[i], response_size);
        if (memcmp(request, response, 4) != 0)
            return fail(err, cap, "parent_%s_header_mismatch:%s", operation, parents[i]);
        status = get32(response + 4);
        if (status != 0)
            return fail(err, cap, "parent_%s_remote_errno:%s:errno=%lu",
                        operation, parents[i], (unsigned long)status);
        mode = get32(response + 8);
        if ((mode & 0170000u) != 0040000u)
            return fail(err, cap, "parent_%s_not_directory:%s:mode=0x%08lx",
                        operation, parents[i], (unsigned long)mode);
        if (pass == 0) original_mode = mode;
        else if (original_mode != mode)
            return fail(err, cap, "parent_lstat_stat_disagree:%s", parents[i]);
      }
    }
    return 0;
}

int item_parent_directories_ready(char *err, size_t cap)
{
    static const char *const parents[] = {
        "/nv/item_files/modem/lte/rrc/csp",
        "/nv/item_files/modem/lte/rrc/efs",
        "/nv/item_files/modem/mmode"
    };
    return parent_directories_ready(parents, sizeof parents / sizeof parents[0], err, cap);
}

int nr_item_parent_directories_ready(char *err, size_t cap)
{
    static const char *const parents[] = {
        "/nv/item_files/modem/nr5g/RRC", "/nv/item_files/modem/mmode"
    };
    return parent_directories_ready(parents, sizeof parents / sizeof parents[0], err, cap);
}

static int item_get_size(unsigned int index, uint32_t requested_size, int exact,
                         unsigned char out[LOCK_ITEM_LIMIT], size_t *len,
                         uint32_t *remote_errno, char *err, size_t errcap)
{
    unsigned char request[GET_HEADER_SIZE + 128];
    unsigned char response[GET_HEADER_SIZE + ITEM_LIMIT];
    char transport_error[160] = {0};
    const char *reason = NULL;
    size_t path_size, response_size = 0;
    uint32_t count = 0, status = 0;
    uint16_t sequence;
    if (len != NULL) *len = 0;
    if (remote_errno != NULL) *remote_errno = UINT32_MAX;
    if (err != NULL && errcap != 0) err[0] = '\0';
    if (index >= ALL_LOCK_ITEM_COUNT || requested_size > ITEM_LIMIT || out == NULL ||
        len == NULL || remote_errno == NULL)
        return fail(err, errcap, "invalid_argument");
    path_size = strlen(paths[index]) + 1;
    sequence = next_sequence++;
    if (next_sequence == 0) next_sequence = 1;
    request[0] = 0x4b; request[1] = 0x13; request[2] = 39; request[3] = 0;
    put32(request + 4, requested_size);
    put32(request + 8, (uint32_t)path_size);
    request[12] = (unsigned char)sequence;
    request[13] = (unsigned char)(sequence >> 8);
    if (path_size > sizeof request - GET_HEADER_SIZE)
        return fail(err, errcap, "path_length");
    memcpy(request + GET_HEADER_SIZE, paths[index], path_size);
    if (diag_exchange(request, GET_HEADER_SIZE + path_size, response, sizeof response,
                      &response_size, transport_error, sizeof transport_error) != 0)
        return fail(err, errcap, "transport:%s", transport_error);
    if (response_size < GET_HEADER_SIZE || response_size > sizeof response)
        return fail(err, errcap, "response_length:actual_len=%zu", response_size);
    count = get32(response + 4);
    status = get32(response + 8);
    if (memcmp(request, response, 4) != 0)
        reason = "header_mismatch";
    else if (get16(response + 12) != sequence)
        reason = "sequence_mismatch";
    else if (status != 0) {
        /* Count is undefined on error; do not reject 0xffffffff or use it as
         * a payload length. Error replies on this modem contain no data. */
        if (response_size != GET_HEADER_SIZE) reason = "error_with_data";
    } else if (count > ITEM_LIMIT || response_size != GET_HEADER_SIZE + count)
        reason = "count_length_mismatch";
    else if (exact && count != requested_size)
        reason = "item_size_mismatch";
    if (reason != NULL)
        return fail(err, errcap,
                    "%s:count=%lu:errno=%lu:seq=%u:expected_seq=%u:actual_len=%zu:requested=%lu",
                    reason, (unsigned long)count, (unsigned long)status,
                    (unsigned int)get16(response + 12), (unsigned int)sequence, response_size,
                    (unsigned long)requested_size);
    *remote_errno = status;
    if (status == 0) {
        memcpy(out, response + GET_HEADER_SIZE, count);
        *len = count;
    }
    return 0;
}

int item_get(unsigned int index, unsigned char out[LOCK_ITEM_LIMIT],
             size_t *len, uint32_t *remote_errno, char *err, size_t errcap)
{
    uint32_t size = index < ALL_LOCK_ITEM_COUNT ? known_item_sizes[index] : 0;
    return item_get_size(index, size ? size : ITEM_LIMIT, size != 0,
                         out, len, remote_errno, err, errcap);
}

typedef struct { uint32_t status, mode, length, links; } ItemStat;

static int item_stat(unsigned int index, int follow, ItemStat *out, char *err, size_t cap)
{
    unsigned char request[4 + 128], response[64];
    size_t path_size, response_size = 0;
    char detail[192] = {0};
    const char *operation = follow ? "stat" : "lstat";
    if (index >= ALL_LOCK_ITEM_COUNT || out == NULL)
        return fail(err, cap, "invalid_stat_argument");
    path_size = strlen(paths[index]) + 1;
    if (path_size > 128) return fail(err, cap, "stat_path_length");
    request[0] = 0x4b; request[1] = 0x13; request[2] = follow ? 15 : 16; request[3] = 0;
    memcpy(request + 4, paths[index], path_size);
    if (diag_exchange(request, 4 + path_size, response, sizeof response,
                      &response_size, detail, sizeof detail))
        return fail(err, cap, "%s_transport:%s", operation, detail);
    if (response_size != (follow ? 32u : 24u))
        return fail(err, cap, "%s_response_length:actual_len=%zu", operation, response_size);
    if (memcmp(request, response, 4))
        return fail(err, cap, "%s_header_mismatch", operation);
    memset(out, 0, sizeof *out);
    out->status = get32(response + 4);
    if (out->status == 0) {
        out->mode = get32(response + 8);
        if (follow) {
            out->length = get32(response + 12);
            out->links = get32(response + 16);
        }
    }
    return 0;
}

static int snapshot_once(unsigned int index, LockItemSnapshot *out, uint32_t *links,
                          uint32_t *remote_errno, char *err, size_t cap)
{
    ItemStat stat, nofollow;
    unsigned char data[LOCK_ITEM_LIMIT];
    size_t length = 0;
    uint32_t remote = UINT32_MAX, requested;
    *remote_errno = UINT32_MAX;
    /* Independently observed LSTAT16 is exactly 24 bytes on the guarded
     * nuwa modem (2026-10-12 v015/initial-metadata.txt). Unlike STAT15 it
     * reports the path itself, so a symlink cannot pass merely via its target. */
    if (item_stat(index, 0, &nofollow, err, cap)) return -1;
    if (nofollow.status != 0 && nofollow.status != 2) {
        *remote_errno = nofollow.status;
        return fail(err, cap, "snapshot_lstat_remote_errno:errno=%lu",
                    (unsigned long)nofollow.status);
    }
    if (nofollow.status == 0 && (nofollow.mode & 0170000u) == 0120000u)
        return fail(err, cap, "snapshot_symlink_rejected");
    if (item_stat(index, 1, &stat, err, cap)) return -1;
    if (stat.status != 0 && stat.status != 2) {
        *remote_errno = stat.status;
        return fail(err, cap, "snapshot_stat_remote_errno:errno=%lu",
                    (unsigned long)stat.status);
    }
    if (nofollow.status != stat.status || nofollow.mode != stat.mode)
        return fail(err, cap, "snapshot_lstat_stat_disagree");
    if (stat.status == 0) {
        uint32_t type = stat.mode & 0170000u;
        if (type != 0100000u && type != 0160000u)
            return fail(err, cap, "snapshot_unsupported_type:mode=0o%lo",
                        (unsigned long)stat.mode);
        if (stat.length > LOCK_ITEM_LIMIT)
            return fail(err, cap, "snapshot_length_out_of_range:length=%lu",
                        (unsigned long)stat.length);
        if (type == 0100000u && stat.links != 1)
            return fail(err, cap, "snapshot_regular_link_count:links=%lu",
                        (unsigned long)stat.links);
        requested = stat.length;
    } else requested = known_item_sizes[index] ? known_item_sizes[index] : ITEM_LIMIT;
    if (item_get_size(index, requested, 1, data, &length, &remote, err, cap)) return -1;
    if (remote != 0 && remote != 2) {
        *remote_errno = remote;
        return fail(err, cap, "snapshot_get_remote_errno:errno=%lu", (unsigned long)remote);
    }
    if (remote != stat.status)
        return fail(err, cap, "snapshot_stat_get_disagree:stat_errno=%lu:get_errno=%lu",
                    (unsigned long)stat.status, (unsigned long)remote);
    memset(out, 0, sizeof *out);
    *links = stat.links;
    if (remote == 0) {
        out->present = 1; out->mode = stat.mode; out->length = (uint32_t)length;
        memcpy(out->data, data, length);
    }
    *remote_errno = remote;
    return 0;
}

static int snapshot_read(unsigned int index, LockItemSnapshot *out,
                          uint32_t *remote_errno, char *err, size_t cap)
{
    LockItemSnapshot first, second;
    uint32_t first_links = 0, second_links = 0;
    *remote_errno = UINT32_MAX;
    if (err != NULL && cap) err[0] = '\0';
    if (index >= ALL_LOCK_ITEM_COUNT || out == NULL)
        return fail(err, cap, "invalid_snapshot_argument");
    if (snapshot_once(index, &first, &first_links, remote_errno, err, cap) ||
        snapshot_once(index, &second, &second_links, remote_errno, err, cap)) return -1;
    if (first_links != second_links || memcmp(&first, &second, sizeof first)) {
        *remote_errno = UINT32_MAX;
        return fail(err, cap, "snapshot_changed_between_reads");
    }
    *out = first;
    return 0;
}

int item_snapshot(unsigned int index, LockItemSnapshot *out, char *err, size_t cap)
{
    uint32_t remote_errno;
    return snapshot_read(index, out, &remote_errno, err, cap);
}

int item_snapshot_supported(unsigned int index, const LockItemSnapshot *value,
                            char *err, size_t cap)
{
    if (err != NULL && cap) err[0] = '\0';
    if (index >= ALL_LOCK_ITEM_COUNT || value == NULL || value->present > 1)
        return fail(err, cap, "invalid_snapshot_argument");
    if (!value->present)
        return value->mode == 0 && value->length == 0 ? 0 :
            fail(err, cap, "absent_snapshot_has_metadata");
    if (value->length == 0 || value->length > LOCK_ITEM_LIMIT)
        return fail(err, cap, "unsupported_baseline_length:length=%lu",
                    (unsigned long)value->length);
    if ((index == 0 && value->length != 4) || (index == 1 && value->length != 36))
        return fail(err, cap, "unsupported_fixed_item_length:index=%u:length=%lu",
                    index, (unsigned long)value->length);
    if (value->mode == 0160777u) return 0;
    /* NR PUT38 uses an ordinary file. PCI has two recovered bounded formats;
     * activation still requires exact type, length and complete readback. */
    if (index == NR_ITEM_BASE && value->length == 256 && value->mode == 0100777u) return 0;
    if (index == NR_ITEM_BASE + 1 && (value->length == 12 || value->length == 28) && value->mode == 0100777u) return 0;
    if ((index == 0 || index == 4) && value->length == 4 && value->mode == 0100001u) return 0;
    return fail(err, cap, "unsupported_baseline_mode:index=%u:mode=0o%lo",
                index, (unsigned long)value->mode);
}

int item_put(unsigned int index, const unsigned char *data, size_t len,
             uint32_t *remote_errno, char *err, size_t errcap)
{
    unsigned char request[14 + ITEM_LIMIT + 128], receipt[64], actual[ITEM_LIMIT];
    char detail[192] = {0}, receipt_hex[29];
    static const char digits[] = "0123456789abcdef";
    size_t path_size, receipt_size = 0, actual_size = 0, prefix_size, i;
    uint32_t read_errno = UINT32_MAX;
    uint16_t sequence;
    if (remote_errno != NULL) *remote_errno = UINT32_MAX;
    if (err != NULL && errcap != 0) err[0] = '\0';
    if (index >= ALL_LOCK_ITEM_COUNT || data == NULL || len == 0 ||
        len > ITEM_LIMIT || remote_errno == NULL)
        return fail(err, errcap, "invalid_put_argument");
    path_size = strlen(paths[index]) + 1;
    if (path_size > 128) return fail(err, errcap, "path_length");
    sequence = next_sequence++;
    if (next_sequence == 0) next_sequence = 1;
    request[0] = 0x4b; request[1] = 0x13; request[2] = 38; request[3] = 0;
    put32(request + 4, (uint32_t)len);
    /* Recovered from dcidaemon 0x2c22c..0x2c238, not host fcntl flags. */
    put32(request + 8, 0x00080040u);
    request[12] = (unsigned char)sequence;
    request[13] = (unsigned char)(sequence >> 8);
    memcpy(request + 14, data, len);
    memcpy(request + 14 + len, paths[index], path_size);
    if (diag_exchange(request, 14 + len + path_size, receipt, sizeof receipt,
                      &receipt_size, detail, sizeof detail) != 0)
        return fail(err, errcap, "put_transport:state_unknown:no_retry:%s", detail);
    if (receipt_size < 4 || receipt_size > sizeof receipt ||
        memcmp(request, receipt, 4) != 0)
        return fail(err, errcap, "put_receipt_invalid:len=%zu:state_unknown", receipt_size);
    prefix_size = receipt_size < 14 ? receipt_size : 14;
    for (i = 0; i < prefix_size; ++i) {
        receipt_hex[2 * i] = digits[receipt[i] >> 4];
        receipt_hex[2 * i + 1] = digits[receipt[i] & 15];
    }
    receipt_hex[2 * prefix_size] = '\0';
    if (item_get_size(index, (uint32_t)len, 1, actual, &actual_size, &read_errno, detail, sizeof detail) != 0)
        return fail(err, errcap, "put_receipt_len=%zu:prefix=%s:get_failed=%s",
                    receipt_size, receipt_hex, detail);
    *remote_errno = read_errno;
    if (read_errno != 0 || actual_size != len || memcmp(actual, data, len) != 0)
        return fail(err, errcap,
                    "put_receipt_len=%zu:prefix=%s:readback_mismatch:errno=%lu:actual_size=%zu",
                    receipt_size, receipt_hex, (unsigned long)read_errno, actual_size);
    if (err != NULL && errcap != 0)
        (void)snprintf(err, errcap, "put_receipt_len=%zu:prefix=%s:verified_by_get",
                       receipt_size, receipt_hex);
    return 0;
}

int item_put_typed(unsigned int index, const unsigned char *data, size_t len,
                   uint32_t mode, const LockItemSnapshot *expected_before,
                   uint32_t *remote_errno, char *err, size_t cap)
{
    LockItemSnapshot wanted, before, after;
    char detail[320] = {0}, receipt[320] = {0};
    uint32_t remote = UINT32_MAX, legacy_status = UINT32_MAX;
    int result;
    if (remote_errno != NULL) *remote_errno = UINT32_MAX;
    if (err != NULL && cap) err[0] = '\0';
    if (data == NULL || expected_before == NULL || remote_errno == NULL || len > LOCK_ITEM_LIMIT)
        return fail(err, cap, "invalid_typed_put_argument");
    memset(&wanted, 0, sizeof wanted);
    wanted.present = 1; wanted.mode = mode; wanted.length = (uint32_t)len;
    memcpy(wanted.data, data, len);
    if (item_snapshot_supported(index, &wanted, err, cap)) return -1;
    if (snapshot_read(index, &before, &remote, detail, sizeof detail)) {
        *remote_errno = remote;
        return fail(err, cap, "typed_put_preflight:%s", detail);
    }
    if (memcmp(&before, expected_before, sizeof before))
        return fail(err, cap, "typed_put_expected_state_changed:no_write");
    if (before.present && (before.mode != mode ||
        ((mode == 0100001u || mode == 0100777u) && before.length != len)))
        return fail(err, cap, "typed_put_representation_mismatch:no_implicit_unlink");
    if (mode == 0160777u || ((index == NR_ITEM_BASE || index == NR_ITEM_BASE + 1) && mode == 0100777u))
        result = item_put(index, data, len, remote_errno, receipt, sizeof receipt);
    else if (before.present)
        result = index == 0 ? efs_write_lock(data, receipt, sizeof receipt)
                            : efs_write_earfcn(data, receipt, sizeof receipt);
    else
        result = index == 0 ? legacy_pci_put(data, &legacy_status, receipt, sizeof receipt)
                            : legacy_earfcn_put(data, &legacy_status, receipt, sizeof receipt);
    if (result) return fail(err, cap, "typed_put_failed:no_retry:%s", receipt);
    if (snapshot_read(index, &after, &remote, detail, sizeof detail)) {
        *remote_errno = remote;
        return fail(err, cap, "typed_put_verify:%s", detail);
    }
    if (memcmp(&wanted, &after, sizeof wanted)) {
        *remote_errno = remote;
        return fail(err, cap, "typed_put_readback_mismatch:mode=0o%lo:length=%lu",
                    (unsigned long)after.mode, (unsigned long)after.length);
    }
    *remote_errno = 0;
    if (err != NULL && cap)
        (void)snprintf(err, cap, "typed_put_verified_by_stat_and_get:%.250s", receipt);
    return 0;
}

int item_unlink(unsigned int index, uint32_t *remote_errno, char *err, size_t errcap)
{
    unsigned char request[4 + 128], response[64], actual[ITEM_LIMIT];
    char detail[192] = {0};
    size_t path_size, response_size = 0, actual_size = 0;
    uint32_t status, read_errno = UINT32_MAX;
    if (remote_errno != NULL) *remote_errno = UINT32_MAX;
    if (err != NULL && errcap != 0) err[0] = '\0';
    if (index >= ALL_LOCK_ITEM_COUNT || remote_errno == NULL)
        return fail(err, errcap, "invalid_unlink_argument");
    path_size = strlen(paths[index]) + 1;
    if (path_size > 128) return fail(err, errcap, "path_length");
    request[0] = 0x4b; request[1] = 0x13; request[2] = 8; request[3] = 0;
    memcpy(request + 4, paths[index], path_size);
    if (diag_exchange(request, 4 + path_size, response, sizeof response,
                      &response_size, detail, sizeof detail) != 0)
        return fail(err, errcap, "unlink_transport:state_unknown:no_retry:%s", detail);
    if (response_size != 8 || memcmp(request, response, 4) != 0)
        return fail(err, errcap, "unlink_reply_invalid:len=%zu:state_unknown", response_size);
    status = get32(response + 4);
    *remote_errno = status;
    if (status != 0 && status != 2)
        return fail(err, errcap, "unlink_remote_errno=%lu", (unsigned long)status);
    if (item_get(index, actual, &actual_size, &read_errno, detail, sizeof detail) != 0)
        return fail(err, errcap, "unlink_get_failed=%s", detail);
    if (read_errno != 2)
        return fail(err, errcap, "unlink_not_absent:errno=%lu:size=%zu",
                    (unsigned long)read_errno, actual_size);
    return 0;
}

int item_unlink_checked(unsigned int index, const LockItemSnapshot *expected_before,
                        uint32_t *remote_errno, char *err, size_t cap)
{
    LockItemSnapshot actual;
    char detail[320] = {0};
    uint32_t remote = UINT32_MAX;
    if (remote_errno != NULL) *remote_errno = UINT32_MAX;
    if (err != NULL && cap) err[0] = '\0';
    if (index >= ALL_LOCK_ITEM_COUNT || expected_before == NULL || remote_errno == NULL)
        return fail(err, cap, "invalid_checked_unlink_argument");
    if (snapshot_read(index, &actual, &remote, detail, sizeof detail)) {
        *remote_errno = remote;
        return fail(err, cap, "checked_unlink_preflight:%s", detail);
    }
    if (memcmp(&actual, expected_before, sizeof actual))
        return fail(err, cap, "checked_unlink_expected_state_changed:no_write");
    if (!actual.present) {
        *remote_errno = 2;
        return 0;
    }
    return item_unlink(index, remote_errno, err, cap);
}

static int probe_items(unsigned start, unsigned count_items, char *summary, size_t cap)
{
    size_t item, used = 0;
    int failed = 0;
    if (summary == NULL || cap == 0)
        return -1;
    summary[0] = '\0';
    for (item = start; item < start + count_items; ++item) {
        LockItemSnapshot snapshot;
        char error[256] = {0};
        char line[768], hex[ITEM_LIMIT * 2 + 1];
        size_t count = 0, i;
        uint32_t remote_errno = UINT32_MAX;
        int result = snapshot_read((unsigned int)item, &snapshot, &remote_errno, error, sizeof error);
        if (result != 0 && (remote_errno == UINT32_MAX || remote_errno == 0 || remote_errno == 2)) {
            (void)snprintf(line, sizeof line, "ITEM_ERROR:%s:%s\n", paths[item], error);
            failed = 1;
        } else {
            static const char digits[] = "0123456789abcdef";
            if (result == 0) count = snapshot.length;
            for (i = 0; i < count; ++i) {
                unsigned char byte = snapshot.data[i];
                hex[i * 2] = digits[byte >> 4];
                hex[i * 2 + 1] = digits[byte & 15];
            }
            hex[(size_t)count * 2] = '\0';
            (void)snprintf(line, sizeof line, "ITEM:%s:%lu:%lu:%s\n",
                           paths[item], (unsigned long)remote_errno,
                           (unsigned long)count, hex);
        }
        if (append_line(summary, cap, &used, line) != 0)
            failed = 1;
    }
    return failed ? -1 : 0;
}

int probe_lock_items(char *summary, size_t cap)
{
    return probe_items(0, LOCK_ITEM_COUNT, summary, cap);
}

int probe_nr_lock_items(char *summary, size_t cap)
{
    return probe_items(NR_ITEM_BASE, NR_ITEM_COUNT, summary, cap);
}

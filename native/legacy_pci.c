/* Independent implementation of one fixed legacy FS_WRITE operation.
 * Packet bytes: supplied APK dcidaemon template 0x114d65 (XOR 0x11),
 * loaded by 0x2abe8; four parameter bytes replaced at 0x2b1ec..0x2b1f8.
 * Packed request/reply and EFS1 status definitions: Qualcomm fs_diag_i.h,
 * main/core/storage/efs/src, chinahby/1110 commit
 * d2f5b81c80b23bc07dfacf6d8063f0f2a04487f8, lines 1444-1579.
 * The matching fs_diag.c handler uses efs_open/efs_write, unlike PUT38.
 * This historical protocol evidence does not establish modem compatibility;
 * verify regular-file type and exact GET content on each actual operation.
 * No original source implementation or target binary is included here. */
#include "legacy_pci.h"
#include "item_probe.h"
#include "transport.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char pci_path[] = "/nv/item_files/modem/lte/rrc/csp/pci_lock";
static const char earfcn_path[] = "/nv/item_files/modem/lte/rrc/csp/earfcn_lock";
_Static_assert(sizeof pci_path == 42, "Legacy pci_lock path must include 42 bytes");
_Static_assert(sizeof earfcn_path == 45, "Legacy earfcn_lock path must include 45 bytes");

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

/* The caller always supplies a buffer of at least four bytes. */
static void prefix4(char out[9], const unsigned char *data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    size_t i, count = size < 4 ? size : 4;
    for (i = 0; i < count; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * count] = '\0';
}

static int legacy_fixed_put(unsigned index, const char *path, size_t path_size,
                            const unsigned char data[4], uint32_t *status, char *err, size_t cap)
{
    /* seq=0, more=0, truncate=1, file_size=4, attr=255, filename_size=42/45.
     * Preserve the original fields; do not reinterpret attr as POSIX flags. */
    unsigned char request[65] = {
        0x59, 0x05, 0, 0, 1, 4, 0, 0, 0, 0xff, 0, 0, 0, 0
    };
    unsigned char receipt[64] = {0}, stat_request[4 + sizeof earfcn_path];
    unsigned char stat_reply[64] = {0}, actual[LOCK_ITEM_LIMIT];
    char detail[192] = {0}, receipt_hex[9], actual_hex[9];
    size_t receipt_size = 0, stat_size = 0, actual_size = 0;
    uint32_t stat_errno, stat_mode, file_size, get_errno = UINT32_MAX;
    int result;

    if (status != NULL) *status = UINT32_MAX;
    if (err != NULL && cap != 0) err[0] = '\0';
    if (data == NULL || status == NULL)
        return fail(err, cap, "legacy_pci_invalid_argument");

    request[13] = (unsigned char)path_size;
    memcpy(request + 14, path, path_size);
    request[14 + path_size] = 4; /* LE16 block length; next byte is already zero. */
    memcpy(request + 16 + path_size, data, 4);
    result = diag_exchange(request, 20 + path_size, receipt, sizeof receipt,
                           &receipt_size, detail, sizeof detail);
    detail[sizeof detail - 1] = '\0';
    if (result != 0)
        return fail(err, cap, "legacy_pci_transport:state_unknown:no_retry:%s", detail);
    prefix4(receipt_hex, receipt, receipt_size);
    if (receipt_size != 4 || receipt[0] != 0x59 || receipt[1] != 5 || receipt[3] != 0)
        return fail(err, cap,
                    "legacy_pci_receipt_invalid:len=%zu:prefix=%s:state_unknown:no_retry",
                    receipt_size, receipt_hex);
    *status = receipt[2];
    if (*status != 0)
        return fail(err, cap, "legacy_pci_efs1_status=%lu:receipt=%s:no_retry",
                    (unsigned long)*status, receipt_hex);

    stat_request[0] = 0x4b; stat_request[1] = 0x13;
    stat_request[2] = 15; stat_request[3] = 0;
    memcpy(stat_request + 4, path, path_size);
    detail[0] = '\0';
    result = diag_exchange(stat_request, 4 + path_size, stat_reply, sizeof stat_reply,
                           &stat_size, detail, sizeof detail);
    detail[sizeof detail - 1] = '\0';
    if (result != 0)
        return fail(err, cap, "legacy_receipt=%s:stat_transport:%s", receipt_hex, detail);
    if (stat_size != 32 || memcmp(stat_request, stat_reply, 4) != 0)
        return fail(err, cap, "legacy_receipt=%s:stat_reply_invalid:len=%zu",
                    receipt_hex, stat_size);
    stat_errno = get32(stat_reply + 4);
    if (stat_errno != 0)
        return fail(err, cap, "legacy_receipt=%s:stat_errno=%lu",
                    receipt_hex, (unsigned long)stat_errno);
    stat_mode = get32(stat_reply + 8);
    file_size = get32(stat_reply + 12);
    if ((stat_mode & 0170000u) != 0100000u || file_size != 4)
        return fail(err, cap,
                    "legacy_receipt=%s:stat_requires_regular4:mode=0o%lo:size=%lu",
                    receipt_hex, (unsigned long)stat_mode, (unsigned long)file_size);

    result = item_get(index, actual, &actual_size, &get_errno, detail, sizeof detail);
    detail[sizeof detail - 1] = '\0';
    if (result != 0)
        return fail(err, cap,
                    "legacy_receipt=%s:stat_mode=0o%lo:stat_size=4:get_failed=%s",
                    receipt_hex, (unsigned long)stat_mode, detail);
    if (get_errno != 0 || actual_size != 4)
        return fail(err, cap,
                    "legacy_receipt=%s:stat_mode=0o%lo:stat_size=4:get_errno=%lu:get_size=%zu",
                    receipt_hex, (unsigned long)stat_mode, (unsigned long)get_errno, actual_size);
    if (memcmp(actual, data, 4) != 0) {
        prefix4(actual_hex, actual, actual_size);
        return fail(err, cap,
                    "legacy_receipt=%s:stat_mode=0o%lo:stat_size=4:get_data_mismatch=%s",
                    receipt_hex, (unsigned long)stat_mode, actual_hex);
    }
    if (err != NULL && cap != 0) {
        (void)snprintf(err, cap,
                       "legacy_receipt=%s:stat_mode=0o%lo:stat_size=4:get_size=4:verified_by_stat_and_get",
                       receipt_hex, (unsigned long)stat_mode);
        err[cap - 1] = '\0';
    }
    return 0;
}

int legacy_pci_put(const unsigned char data[4], uint32_t *status, char *err, size_t cap)
{
    return legacy_fixed_put(0, pci_path, sizeof pci_path, data, status, err, cap);
}

/* Original dcidaemon!0x325d4, construction at 0x33128..0x332f4: same FS_WRITE
 * fields, 45-byte path including NUL, 65 bytes total; no caller-supplied path. */
int legacy_earfcn_put(const unsigned char data[4], uint32_t *status, char *err, size_t cap)
{
    return legacy_fixed_put(4, earfcn_path, sizeof earfcn_path, data, status, err, cap);
}

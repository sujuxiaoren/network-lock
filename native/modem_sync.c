/*
 * Independently encoded fixed request observed in dcidaemon!0x32278:
 *   4b 13 30 00 69 27 2f 00
 * EFS2 command 48, original sequence 0x2769, fixed path slash + NUL.
 * The strict SYNC48 layout and exact restart receipt were also observed on
 * the guarded nuwa profile. There is still no confirmed STATUS49 mapping.
 * These helpers report request receipts, not sync completion or recovery.
 * Evidence: .analysis/efs-sync48-49-evidence.md and activation-helper.md.
 */
#include "modem_sync.h"
#include "transport.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const unsigned char sync_request[8] = {
    0x4b, 0x13, 0x30, 0x00, 0x69, 0x27, 0x2f, 0x00
};
static const unsigned char restart_request[4] = {0x4b, 0x25, 0x03, 0x00};
static enum modem_request_progress restart_progress_in_process;

static int fail(char *error, size_t cap, const char *message)
{
    if (error != NULL && cap != 0) {
        (void)snprintf(error, cap, "%s", message);
        error[cap - 1] = '\0';
    }
    return -1;
}

static int exchange_raw(const unsigned char *request, size_t request_length,
                        struct modem_sync_receipt *receipt,
                        char *error, size_t error_cap)
{
    int result;
    if (error != NULL && error_cap != 0) error[0] = '\0';
    if (receipt == NULL)
        return fail(error, error_cap, "Modem request receipt storage is required");
    memset(receipt, 0, sizeof *receipt);
    receipt->progress = MODEM_REQUEST_ATTEMPTED_OUTCOME_UNKNOWN;
    result = diag_exchange(request, request_length, receipt->bytes,
                           sizeof receipt->bytes, &receipt->length,
                           error, error_cap);
    if (error != NULL && error_cap != 0) error[error_cap - 1] = '\0';
    if (receipt->length > sizeof receipt->bytes) {
        receipt->length = sizeof receipt->bytes;
        return fail(error, error_cap, "Modem transport returned oversized response");
    }
    if (result != 0) {
        if (error != NULL && error_cap != 0 && error[0] == '\0')
            (void)fail(error, error_cap, "Modem transport exchange failed; outcome unknown");
        return -1;
    }
    if (receipt->length < 4)
        return fail(error, error_cap, "Modem response is shorter than DIAG header");
    if (memcmp(receipt->bytes, request, 4) != 0)
        return fail(error, error_cap, "Modem response command/subsystem mismatch");
    if (error != NULL && error_cap != 0) error[0] = '\0';
    return 0;
}

int modem_sync_start_raw(struct modem_sync_receipt *receipt,
                         char *error, size_t error_cap)
{
    return exchange_raw(sync_request, sizeof sync_request, receipt, error, error_cap);
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int boottime_ms(uint64_t *value, char *error, size_t cap)
{
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now) || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L ||
        (uint64_t)now.tv_sec > (UINT64_MAX - 999u) / 1000u)
        return fail(error, cap, "Cannot read modem preparation clock");
    *value = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    return 0;
}

static int wait_five_seconds(char *error, size_t cap)
{
    uint64_t start, now;
    if (boottime_ms(&start, error, cap)) return -1;
    if (start > UINT64_MAX - 5000u)
        return fail(error, cap, "Modem preparation clock is out of range");
    const uint64_t deadline = start + 5000u;
    now = start;
    while (now < deadline) {
        uint64_t remaining = deadline - now;
        struct timespec delay;
        delay.tv_sec = (time_t)(remaining / 1000u);
        delay.tv_nsec = (long)((remaining % 1000u) * 1000000u);
        if (nanosleep(&delay, NULL) && errno != EINTR)
            return fail(error, cap, "Modem preparation wait failed");
        if (boottime_ms(&now, error, cap)) return -1;
        if (now < start)
            return fail(error, cap, "Modem preparation clock moved backwards");
    }
    return 0;
}

int modem_sync_prepare(struct modem_sync_receipt *receipt,
                       char *error, size_t error_cap)
{
    if (modem_sync_start_raw(receipt, error, error_cap)) return -1;
    if (receipt->length != 14)
        return fail(error, error_cap, "SYNC48 response length is not exactly 14");
    if (memcmp(receipt->bytes, sync_request, 6) != 0)
        return fail(error, error_cap, "SYNC48 sequence/header mismatch");
    uint32_t remote_error = get32(receipt->bytes + 10);
    if (remote_error != 0) {
        if (error != NULL && error_cap != 0) {
            (void)snprintf(error, error_cap, "SYNC48 rejected with remote errno 0x%08lx",
                           (unsigned long)remote_error);
            error[error_cap - 1] = '\0';
        }
        return -1;
    }
    /* Token is preserved verbatim; there is no evidence that zero is invalid. */
    receipt->progress = MODEM_REQUEST_RECEIPT_VALIDATED;
    return wait_five_seconds(error, error_cap);
}

int modem_restart_once(struct modem_sync_receipt *receipt,
                       char *error, size_t error_cap)
{
    if (error != NULL && error_cap != 0) error[0] = '\0';
    if (receipt == NULL)
        return fail(error, error_cap, "Restart receipt storage is required");
    memset(receipt, 0, sizeof *receipt);
    if (restart_progress_in_process != MODEM_REQUEST_NOT_ATTEMPTED)
        return fail(error, error_cap, "Restart was already attempted in this process; not repeated");
    /* The durable cross-process intent must already exist in the caller. */
    restart_progress_in_process = MODEM_REQUEST_ATTEMPTED_OUTCOME_UNKNOWN;
    if (exchange_raw(restart_request, sizeof restart_request, receipt, error, error_cap))
        return -1;
    if (receipt->length != sizeof restart_request ||
        memcmp(receipt->bytes, restart_request, sizeof restart_request) != 0)
        return fail(error, error_cap, "Restart response is not the exact four-byte receipt");
    receipt->progress = MODEM_REQUEST_RECEIPT_VALIDATED;
    restart_progress_in_process = MODEM_REQUEST_RECEIPT_VALIDATED;
    return 0;
}

enum modem_request_progress modem_restart_progress(void)
{
    return restart_progress_in_process;
}

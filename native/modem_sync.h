#pragma once

#include <stddef.h>

enum modem_request_progress {
    MODEM_REQUEST_NOT_ATTEMPTED = 0,
    /* Set BEFORE calling transport. Actual delivery may still be unknown. */
    MODEM_REQUEST_ATTEMPTED_OUTCOME_UNKNOWN = 1,
    /* A request receipt only: not sync completion, recovery, or cell lock. */
    MODEM_REQUEST_RECEIPT_VALIDATED = 2
};

/* A request receipt only. No field below asserts modem recovery/completion. */
struct modem_sync_receipt {
    unsigned char bytes[128];
    size_t length;
    enum modem_request_progress progress;
};

/*
 * Sends the original daemon's fixed-root SYNC_NO_WAIT request once.
 * Returns 0 only for a bounded response with the matching DIAG header.
 * The payload stays uninterpreted: even return 0 does not establish that
 * the remote filesystem accepted or completed a sync. On failure the
 * bounded raw bytes returned by transport remain available for diagnosis.
 * The caller owns device/session guards and its operation deadline.
 * The current diag_exchange has a six-second transport wait.
 */
int modem_sync_start_raw(struct modem_sync_receipt *receipt,
                         char *error, size_t error_cap);

/*
 * One fixed-root SYNC48 request, strict observed 14-byte reply validation,
 * then a five-second CLOCK_BOOTTIME wait. Neither receipt nor wait proves
 * filesystem synchronization complete; STATUS49 is deliberately not used.
 * A failure never automatically sends a restart or repeats a request.
 */
int modem_sync_prepare(struct modem_sync_receipt *receipt,
                       char *error, size_t error_cap);

/*
 * Caller must durably journal restart intent BEFORE calling this function.
 * Sends the original four-byte forced modem crash request at most once per
 * process and accepts only its exact four-byte receipt. This may interrupt
 * service on BOTH SIMs. Return 0 is not modem recovery or cell-lock proof.
 * Timeout/error after ATTEMPTED_OUTCOME_UNKNOWN requires retaining recovery
 * state; do not retry. Reopen a fresh guarded DIAG session for later reads.
 * The caller owns cross-process once-only enforcement, identity/session
 * guards, sequence after modem_sync_prepare, and the total operation budget.
 */
int modem_restart_once(struct modem_sync_receipt *receipt,
                       char *error, size_t error_cap);

/* This process only; never inferred from a prior durable journal stage. */
enum modem_request_progress modem_restart_progress(void);

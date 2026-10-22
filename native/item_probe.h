#ifndef LTE_CELL_LOCK_ITEM_PROBE_H
#define LTE_CELL_LOCK_ITEM_PROBE_H

#include <stddef.h>
#include <stdint.h>

/* Recommended summary capacity for all five bounded item replies. */
#define ITEM_PROBE_SUMMARY_CAP 4096u
/* Frozen LTE snapshot count: CLREC002..005 compatibility depends on it. */
#define LOCK_ITEM_COUNT 5u
#define NR_ITEM_BASE 5u
#define NR_ITEM_COUNT 4u
#define ALL_LOCK_ITEM_COUNT 9u
#define LOCK_ITEM_LIMIT 256u

/* A bounded configuration snapshot, not a whole-EFS or metadata backup.
 * Absent snapshots have present/mode/length zero. present is otherwise one.
 * STAT does not expose ownership; timestamps are not restored by these APIs. */
typedef struct {
    uint32_t present, mode, length;
    unsigned char data[LOCK_ITEM_LIMIT];
} LockItemSnapshot;

/* Two LSTAT/STAT/exact-GET reads must agree before publishing the snapshot.
 * LSTAT rejects symlinks; regular files must have one link. The two passes
 * detect observed changes, not an atomic snapshot against external writers.
 * Raw zero-length objects are retained for interrupted-write diagnosis;
 * item_snapshot_supported rejects them as a new transaction baseline.
 * Any remote error other than confirmed absence fails, with its number in err. */
int item_snapshot(unsigned int index, LockItemSnapshot *out, char *err, size_t cap);
int item_snapshot_supported(unsigned int index, const LockItemSnapshot *value,
                            char *err, size_t cap);

/* Write a supported representation without any implicit UNLINK. A pre-existing
 * object must already have the requested full mode; regular files also retain
 * their length. Caller owns the durable transaction and any explicit type
 * conversion. Success requires stable STAT/exact-GET verification of mode,
 * length and bytes. Failed operations are never retried. */
int item_put_typed(unsigned int index, const unsigned char *data, size_t len,
                   uint32_t mode, const LockItemSnapshot *expected_before,
                   uint32_t *remote_errno, char *err, size_t cap);

/* Transactions must use this checked delete, retaining their previously
 * confirmed expected state. A newly read differing value is never adopted.
 * Comparison and mutation are separate modem requests, not an atomic CAS. */
int item_unlink_checked(unsigned int index, const LockItemSnapshot *expected_before,
                        uint32_t *remote_errno, char *err, size_t cap);

/* index 0..4 is pci_lock, cell_restrict_opt_params, lte_bandpref,
 * lte_bandpref_extn_65_256, earfcn_lock. Indices 5..8 are NR earfcn_lock,
 * pci_lock_info, nr_band_pref, nr_nsa_band_pref. All use subscription 0.
 * Return 0 means a protocol-valid reply: always check *remote_errno.
 * GET errno=2 reports absence; len is zero on every remote error.
 * Return -1 means invalid arguments, transport failure, or malformed reply.
 * out is changed only after a validated successful GET. Calls are serial. */
int item_get(unsigned int index, unsigned char out[LOCK_ITEM_LIMIT],
             size_t *len, uint32_t *remote_errno, char *err, size_t errcap);

/* Read-only LSTAT/STAT of the fixed csp, efs and mmode parents. Requires all
 * three to exist as directories without symlinks before PUT uses O_AUTODIR.
 * Never creates or removes a directory; returns 0 only on verified replies. */
int item_parent_directories_ready(char *err, size_t cap);
/* Same validated protocol, restricted to NR RRC and mmode parents. */
int nr_item_parent_directories_ready(char *err, size_t cap);

/* Mutating primitives: caller MUST own a durable recovery record first.
 * Unlike GET, return 0 means the intended result was also verified by GET.
 * PUT uses the statically observed 0x00080040 field and LE16 sequence. Its
 * reply is only a same-command receipt: no unknown ACK fields are treated as
 * success. GET must confirm the exact length and bytes before return 0.
 * On PUT success remote_errno is GET's zero; err carries receipt metadata.
 * UNLINK validates its documented errno field, then requires GET errno 2.
 * A failed mutation may already have changed the item; never blindly retry.
 * These functions do not implement rollback or reboot/radio operations. */
int item_put(unsigned int index, const unsigned char *data, size_t len,
             uint32_t *remote_errno, char *err, size_t errcap);
/* Raw protocol primitive; transaction callers should use item_unlink_checked. */
int item_unlink(unsigned int index, uint32_t *remote_errno, char *err, size_t errcap);

/*
 * Read-only STAT/exact-GET snapshot probe for five fixed slot-0 paths.
 * Caller must open the DIAG transport first; calls must be serial.
 * Valid replies: ITEM:<path>:<errno-decimal>:<size-decimal>:<hex>\n
 * Failed validation/transport: ITEM_ERROR:<path>:<fixed-reason>\n
 * Returns 0 only when every reply is protocol-valid and summary is complete.
 * Remote errno != 0 is reported as a valid reply, not interpreted as success.
 * errno 2 is published only after agreeing valid STAT and GET absence replies.
 * Null/zero-capacity output causes failure without sending any request.
 */
int probe_lock_items(char *summary, size_t cap);
int probe_nr_lock_items(char *summary, size_t cap);

#endif

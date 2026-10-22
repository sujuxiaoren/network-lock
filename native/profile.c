/* Bounded reversible profile: nuwa / Android 13, first subscription.
 * A durable CLREC007 snapshot owns every attempted mutation. Original
 * presence, full mode, length and bytes are restored, never guessed zeroes.
 * Only fixed, supported types/modes are accepted; unknown states retain the
 * journal. Static lock/cleanup evidence: .analysis/lte-full-lock-sequence.md.
 */
#include "profile.h"
#include "item_probe.h"
#include "legacy_pci.h"
#include "modem_sync.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#ifndef JOURNAL_DIR
#define JOURNAL_DIR "/data/adb/lte-cell-lock"
#endif
#define JOURNAL JOURNAL_DIR "/lock-recovery-v1.bin"
#define JOURNAL_TEMP JOURNAL_DIR "/lock-recovery-v1.pending"
#define MUTEX_FILE JOURNAL_DIR "/operation-v1.lock"
#define SESSION_FILE JOURNAL_DIR "/diag-session.lock"
enum {
    WRITE_ITEMS = 3u,
    LOCK_RESTART_ATTEMPT = 4u,
    LOCK_RESTART_ACK = 8u,
    RESTORE_RESTART_ATTEMPT = 16u,
    RESTORE_RESTART_ACK = 32u,
    RECOVERY_FLAGS = 63u
};
typedef struct {
    char magic[8];
    uint32_t earfcn, pci;
    /* Low two bits precede item writes and clear only after confirmed
     * absence. CLREC003 upper bits persist each phase's restart intent
     * before submission and separately record a validated receipt. An
     * intent without an ACK never authorizes automatic retransmission. */
    uint32_t write_intent_mask;
    char fingerprint[1024];
    char baseband[1024];
    uint32_t checksum;
} RecoveryLegacy;
_Static_assert(sizeof(RecoveryLegacy) == 2072, "Legacy recovery layout changed");

typedef struct {
    char magic[8];
    uint32_t earfcn, pci;
    /* Restart flags keep CLREC003 positions; low bits mirror items 0 and 1. */
    uint32_t write_intent_mask;
    char fingerprint[1024];
    char baseband[1024];
    uint32_t item_intent_mask;
    /* Saved BEFORE removal or creation; admit only known intermediates. */
    uint32_t absent_intent_mask, empty_intent_mask;
    uint32_t legacy_record;
    LockItemSnapshot original[LOCK_ITEM_COUNT];
    uint32_t checksum;
} RecoveryV4;

typedef struct {
    char magic[8];
    uint32_t earfcn, pci;
    /* Restart flags keep CLREC003 positions; low bits mirror items 0 and 1. */
    uint32_t write_intent_mask;
    char fingerprint[1024];
    char baseband[1024];
    uint32_t item_intent_mask;
    /* Saved BEFORE removal or creation; admit only known intermediates. */
    uint32_t absent_intent_mask, empty_intent_mask;
    uint32_t legacy_record;
    LockItemSnapshot original[LOCK_ITEM_COUNT];
    /* Persisted mode disambiguates a real PCI value from frequency-only.
     * This field occupies CLREC004's old checksum offset. */
    uint32_t lock_mode;
    uint32_t checksum;
} RecoveryV5;
typedef struct {
    char magic[8];
    uint32_t earfcn, pci;
    uint32_t write_intent_mask;
    char fingerprint[1024];
    char baseband[1024];
    uint32_t item_intent_mask, absent_intent_mask, empty_intent_mask, legacy_record;
    LockItemSnapshot original[LOCK_ITEM_COUNT];
    uint32_t lock_mode;
    uint32_t radio_profile, nr_arfcn, nr_scs;
    uint32_t checksum;
} RecoveryV6;
typedef struct {
    char magic[8];
    uint32_t earfcn, pci;
    uint32_t write_intent_mask;
    char fingerprint[1024];
    char baseband[1024];
    uint32_t item_intent_mask, absent_intent_mask, empty_intent_mask, legacy_record;
    LockItemSnapshot original[LOCK_ITEM_COUNT];
    uint32_t lock_mode, radio_profile, nr_arfcn, nr_scs;
    uint32_t nr_band, nr_cell_version;
    uint32_t checksum;
} Recovery;
_Static_assert(sizeof(LockItemSnapshot) == 268, "Snapshot layout changed");
_Static_assert(sizeof(RecoveryV4) == 3428, "CLREC004 layout changed");
_Static_assert(sizeof(RecoveryV5) == 3432, "CLREC005 layout changed");
_Static_assert(sizeof(RecoveryV6) == 3444, "CLREC006 layout changed");
_Static_assert(sizeof(Recovery) == 3452, "CLREC007 layout changed");
_Static_assert(offsetof(Recovery, lock_mode) == offsetof(RecoveryV4, checksum),
               "CLREC004 prefix compatibility changed");
_Static_assert(offsetof(Recovery, radio_profile) == offsetof(RecoveryV5, checksum),
               "CLREC005 prefix compatibility changed");
_Static_assert(offsetof(Recovery, nr_band) == offsetof(RecoveryV6, checksum),
               "CLREC006 prefix compatibility changed");
enum { ALL_ITEMS = 31u, REGULAR_PCI_MODE = 0100001u, ITEM_FILE_MODE = 0160777u,
       REGULAR_NR_MODE = 0100777u };
enum { LOCK_MODE_CELL = 1u, LOCK_MODE_EARFCN = 2u };
enum { RADIO_LTE = 0u, RADIO_NR_SA = 1u, LOCK_MODE_NR_ARFCN = 3u, LOCK_MODE_NR_CELL = 4u };

static unsigned item_count(const Recovery *r) {
    return r->radio_profile == RADIO_NR_SA ? NR_ITEM_COUNT : LOCK_ITEM_COUNT;
}

static unsigned item_index(const Recovery *r, unsigned index) {
    return r->radio_profile == RADIO_NR_SA ? NR_ITEM_BASE + index : index;
}

static int parents_ready(const Recovery *r, char *error, size_t cap) {
    return r->radio_profile == RADIO_NR_SA ? nr_item_parent_directories_ready(error, cap) :
                                           item_parent_directories_ready(error, cap);
}

static int fail(char *error, size_t cap, const char *text) {
    if (cap) snprintf(error, cap, "%s", text);
    return -1;
}

/* Only fixed lock-item identities, status, and bounded diagnostics are logged.
 * Never include firmware identity strings or unrelated modem item contents. */
static void log_stage(const char *operation, const char *stage, int item, int result,
                      uint32_t remote, size_t actual, size_t expected, const char *detail) {
    char remote_text[24], actual_text[24], expected_text[24];
    if (remote == UINT32_MAX) snprintf(remote_text, sizeof remote_text, "unknown");
    else snprintf(remote_text, sizeof remote_text, "%u", (unsigned)remote);
    if (actual == SIZE_MAX) snprintf(actual_text, sizeof actual_text, "unknown");
    else snprintf(actual_text, sizeof actual_text, "%zu", actual);
    if (expected == SIZE_MAX) snprintf(expected_text, sizeof expected_text, "unknown");
    else snprintf(expected_text, sizeof expected_text, "%zu", expected);
    fprintf(stderr, "PROFILE_%s stage=%s item=%d result=%d remote_errno=%s actual_len=%s expected_len=%s detail=%s\n",
            operation, stage, item, result, remote_text, actual_text, expected_text,
            detail && detail[0] ? detail : "none");
}

static uint32_t checksum_bytes(const void *record, size_t count) {
    const unsigned char *p = (const unsigned char *)record;
    uint32_t sum = 2166136261u;
    for (size_t i = 0; i < count; ++i) sum = (sum ^ p[i]) * 16777619u;
    return sum;
}

static uint32_t checksum(const Recovery *r) {
    return checksum_bytes(r, offsetof(Recovery, checksum));
}

static int device_ok(char *error, size_t cap) {
    char device[1024] = {0}, sdk[1024] = {0};
    __system_property_get("ro.product.device", device);
    __system_property_get("ro.build.version.sdk", sdk);
    if (strcmp(device, "nuwa") || strcmp(sdk, "33"))
        return fail(error, cap, "Profile restricted to nuwa / Android 13");
    return 0;
}

int profile_has_recovery(void) {
    struct stat st;
    /* An inaccessible record must never be mistaken for no record. */
    return lstat(JOURNAL, &st) == 0 || errno != ENOENT;
}

static int sync_directory(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = fsync(fd);
    if (close(fd)) result = -1;
    return result;
}

static int firmware_identity(Recovery *r, char *error, size_t cap) {
    memset(r->fingerprint, 0, sizeof r->fingerprint);
    memset(r->baseband, 0, sizeof r->baseband);
    if (__system_property_get("ro.build.fingerprint", r->fingerprint) <= 0 ||
        __system_property_get("gsm.version.baseband", r->baseband) <= 0)
        return fail(error, cap, "Cannot establish firmware and baseband identity");
    return 0;
}

static int snapshot_equal(const LockItemSnapshot *a, const LockItemSnapshot *b) {
    return a->present == b->present && a->mode == b->mode && a->length == b->length &&
           a->length <= LOCK_ITEM_LIMIT && !memcmp(a->data, b->data, a->length);
}

static int read_snapshot(const Recovery *r, unsigned i, LockItemSnapshot *out, char *error, size_t cap) {
    memset(out, 0, sizeof *out);
    int result = item_snapshot(item_index(r, i), out, error, cap);
    if (!result && (out->present > 1 || out->length > LOCK_ITEM_LIMIT ||
                    (!out->present && (out->mode || out->length))))
        return fail(error, cap, "Invalid bounded item snapshot");
    return result;
}

static int capture_baseline(Recovery *r, char *error, size_t cap) {
    for (unsigned i = 0; i < item_count(r); ++i) {
        if (read_snapshot(r, i, &r->original[i], error, cap) ||
            item_snapshot_supported(item_index(r, i), &r->original[i], error, cap)) return -1;
        if (r->radio_profile == RADIO_NR_SA && r->original[i].present) {
            const LockItemSnapshot *s = &r->original[i];
            if (i == 0 && s->mode != REGULAR_NR_MODE)
                return fail(error, cap, "NR frequency baseline is not a supported regular representation; exact restoration cannot be guaranteed");
            /* Live PUT38 creates special PCI items on the guarded modem. An
             * ordinary PCI baseline cannot be recreated after removal. */
            if (i == 1 && (s->mode != ITEM_FILE_MODE || (s->length != 12 && s->length != 28)))
                return fail(error, cap, "NR PCI baseline is not a supported special 12/28-byte representation; exact restoration cannot be guaranteed");
        }
    }
    return 0;
}

static int verify_baseline(const Recovery *r, char *error, size_t cap) {
    for (unsigned i = 0; i < item_count(r); ++i) {
        LockItemSnapshot actual;
        if (read_snapshot(r, i, &actual, error, cap)) return -1;
        if (!snapshot_equal(&actual, &r->original[i]))
            return fail(error, cap, "Configuration no longer matches the saved original snapshot; recovery record retained");
    }
    return 0;
}

static int readiness_checks(const Recovery *r, char *error, size_t cap) {
    if (device_ok(error, cap)) return -1;
    if (profile_has_recovery()) return fail(error, cap, "Recovery record exists; restore it before another lock");
    return parents_ready(r, error, cap);
}

int profile_ready(char *error, size_t cap) {
    Recovery probe = {0};
    if (readiness_checks(&probe, error, cap)) return -1;
    return capture_baseline(&probe, error, cap);
}

int profile_nr_ready(char *error, size_t cap) {
    Recovery probe = {0};
    probe.radio_profile = RADIO_NR_SA;
    if (readiness_checks(&probe, error, cap)) return -1;
    return capture_baseline(&probe, error, cap);
}

static int acquire_named(const char *path, char *error, size_t cap) {
    if (getuid() != 0) { fail(error, cap, "Root required"); return -1; }
    if (mkdir(JOURNAL_DIR, 0700) && errno != EEXIST) {
        fail(error, cap, "Cannot create persistent recovery directory"); return -1;
    }
    struct stat directory;
    if (lstat(JOURNAL_DIR, &directory) || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != 0 || (directory.st_mode & 0077)) {
        fail(error, cap, "Recovery directory ownership or permissions are invalid"); return -1;
    }
    /* The recovery directory's own directory entry must survive power loss. */
    char parent[sizeof JOURNAL_DIR];
    memcpy(parent, JOURNAL_DIR, sizeof parent);
    char *slash = strrchr(parent, '/');
    if (!slash) { fail(error, cap, "Recovery directory must be absolute"); return -1; }
    if (slash == parent) slash[1] = '\0'; else *slash = '\0';
    if (sync_directory(parent)) {
        fail(error, cap, "Cannot synchronize recovery parent directory"); return -1;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { fail(error, cap, "Cannot open application operation lock"); return -1; }
    struct stat mutex;
    if (fstat(fd, &mutex) || !S_ISREG(mutex.st_mode) || mutex.st_uid != 0 ||
        mutex.st_nlink != 1 || (mutex.st_mode & 0077)) {
        close(fd); fail(error, cap, "Operation lock ownership or permissions are invalid"); return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); fail(error, cap, "Another operation is running"); return -1; }
    return fd;
}

/* Held by the caller across diag_open, all commands, and diag_close.  It is
 * deliberately separate from the recovery transaction mutex. */
int profile_acquire_session(char *error, size_t cap) {
    return acquire_named(SESSION_FILE, error, cap);
}

static int acquire(char *error, size_t cap) {
    return acquire_named(MUTEX_FILE, error, cap);
}

static int save_recovery(Recovery *r, int initial, char *error, size_t cap) {
    if (initial && profile_has_recovery())
        return fail(error, cap, "Recovery record already exists");
    /* A leftover staging file cannot authorize modem writes: callers submit
     * PUT only after rename and directory fsync have both succeeded. */
    if (unlink(JOURNAL_TEMP) && errno != ENOENT)
        return fail(error, cap, "Cannot clear uncommitted recovery staging file");
    /* Validated old records migrate to explicit absent snapshots. */
    memcpy(r->magic, "CLREC007", sizeof r->magic);
    r->checksum = checksum(r);
    int fd = open(JOURNAL_TEMP, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return fail(error, cap, "Cannot stage recovery record; no further modem writes performed");
    const unsigned char *data = (const unsigned char *)r;
    size_t done = 0;
    while (done < sizeof *r) {
        ssize_t n = write(fd, data + done, sizeof *r - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return fail(error, cap, "Recovery record write failed; no further modem writes performed"); }
        done += (size_t)n;
    }
    int sync_result = fsync(fd);
    int close_result = close(fd);
    if (sync_result || close_result) return fail(error, cap, "Recovery record durability failed; no further modem writes performed");
    if (rename(JOURNAL_TEMP, JOURNAL))
        return fail(error, cap, "Cannot commit recovery record; no further modem writes performed");
    return sync_directory(JOURNAL_DIR) ?
        fail(error, cap, "Recovery directory synchronization failed; no further modem writes performed") : 0;
}

static int read_record_bytes(int fd, void *record, size_t length) {
    size_t done = 0;
    while (done < length) {
        ssize_t n = read(fd, (unsigned char *)record + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int flags_valid(uint32_t flags, uint32_t items) {
    return !(flags & ~RECOVERY_FLAGS) && (flags & WRITE_ITEMS) == (items & WRITE_ITEMS) &&
        (!(flags & LOCK_RESTART_ACK) || (flags & LOCK_RESTART_ATTEMPT)) &&
        (!(flags & RESTORE_RESTART_ACK) || (flags & RESTORE_RESTART_ATTEMPT)) &&
        (!(flags & RESTORE_RESTART_ATTEMPT) || !items);
}

static int load_recovery(Recovery *r, char *error, size_t cap) {
    int fd = open(JOURNAL, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return fail(error, cap, "No readable recovery record");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        (st.st_size != (off_t)sizeof *r && st.st_size != (off_t)sizeof(RecoveryV6) &&
         st.st_size != (off_t)sizeof(RecoveryV5) && st.st_size != (off_t)sizeof(RecoveryV4) &&
         st.st_size != (off_t)sizeof(RecoveryLegacy)) ||
        st.st_uid != 0 || st.st_nlink != 1 || (st.st_mode & 0077)) {
        close(fd); return fail(error, cap, "Recovery record has an invalid size or type");
    }
    memset(r, 0, sizeof *r);
    int invalid = 0;
    if (st.st_size == (off_t)sizeof(RecoveryLegacy)) {
        RecoveryLegacy old = {0};
        invalid = read_record_bytes(fd, &old, sizeof old);
        int v2 = !memcmp(old.magic, "CLREC002", 8);
        int v3 = !memcmp(old.magic, "CLREC003", 8);
        if ((!v2 && !v3) || (v2 && (old.write_intent_mask & ~WRITE_ITEMS)) ||
            !flags_valid(old.write_intent_mask, old.write_intent_mask & WRITE_ITEMS) ||
            old.checksum != checksum_bytes(&old, offsetof(RecoveryLegacy, checksum))) invalid = 1;
        if (!invalid) {
            memcpy(r, &old, offsetof(RecoveryLegacy, checksum));
            memcpy(r->magic, "CLREC007", 8);
            r->item_intent_mask = old.write_intent_mask & WRITE_ITEMS;
            r->legacy_record = 1;
            r->lock_mode = LOCK_MODE_CELL;
        }
    } else if (st.st_size == (off_t)sizeof(RecoveryV4)) {
        RecoveryV4 old = {0};
        invalid = read_record_bytes(fd, &old, sizeof old);
        if (memcmp(old.magic, "CLREC004", 8) || (old.empty_intent_mask & ~1u) ||
            old.original[4].mode == REGULAR_PCI_MODE ||
            old.checksum != checksum_bytes(&old, offsetof(RecoveryV4, checksum))) invalid = 1;
        if (!invalid) {
            memcpy(r, &old, offsetof(RecoveryV4, checksum));
            memcpy(r->magic, "CLREC007", 8);
            r->lock_mode = LOCK_MODE_CELL;
        }
    } else if (st.st_size == (off_t)sizeof(RecoveryV5)) {
        RecoveryV5 old = {0};
        invalid = read_record_bytes(fd, &old, sizeof old);
        if (memcmp(old.magic, "CLREC005", 8) ||
            old.checksum != checksum_bytes(&old, offsetof(RecoveryV5, checksum))) invalid = 1;
        if (!invalid) {
            memcpy(r, &old, offsetof(RecoveryV5, checksum));
            memcpy(r->magic, "CLREC007", 8);
        }
    } else if (st.st_size == (off_t)sizeof(RecoveryV6)) {
        RecoveryV6 old = {0};
        invalid = read_record_bytes(fd, &old, sizeof old);
        if (memcmp(old.magic, "CLREC006", 8) ||
            old.checksum != checksum_bytes(&old, offsetof(RecoveryV6, checksum)) ||
            old.lock_mode == LOCK_MODE_NR_CELL) invalid = 1;
        if (!invalid) {
            memcpy(r, &old, offsetof(RecoveryV6, checksum));
            memcpy(r->magic, "CLREC007", 8);
        }
    } else {
        invalid = read_record_bytes(fd, r, sizeof *r);
        if (memcmp(r->magic, "CLREC007", 8) || r->checksum != checksum(r)) invalid = 1;
    }
    close(fd);
    uint32_t items = r->item_intent_mask;
    int nr = r->radio_profile == RADIO_NR_SA;
    int nr_cell = nr && r->lock_mode == LOCK_MODE_NR_CELL;
    if (invalid || r->radio_profile > RADIO_NR_SA || r->earfcn > 65535 || r->pci > (nr_cell ? 1007u : 503u) ||
        (items & ~(nr ? 15u : ALL_ITEMS)) ||
        (nr ? ((r->lock_mode != LOCK_MODE_NR_ARFCN && !nr_cell) || r->earfcn || (!nr_cell && r->pci) ||
               r->nr_arfcn > 3279165u || r->nr_scs > 4 || r->legacy_record || r->empty_intent_mask) :
              (r->nr_arfcn || r->nr_scs ||
               (r->lock_mode != LOCK_MODE_CELL && r->lock_mode != LOCK_MODE_EARFCN))) ||
        (nr_cell ? (!r->nr_band || r->nr_band > 261u ||
                    (r->nr_cell_version != 1u && r->nr_cell_version != 2u)) :
                   (r->nr_band || r->nr_cell_version)) ||
        (r->lock_mode == LOCK_MODE_EARFCN && (r->pci || r->legacy_record)) ||
        !flags_valid(r->write_intent_mask, items) || r->legacy_record > 1 ||
        (r->absent_intent_mask & ~items) || (r->empty_intent_mask & ~items) ||
        (r->empty_intent_mask & ~17u) ||
        (r->lock_mode == LOCK_MODE_CELL && (r->empty_intent_mask & 16u) &&
            r->original[4].mode != REGULAR_PCI_MODE) ||
        (r->lock_mode == LOCK_MODE_EARFCN && (r->empty_intent_mask & 1u) &&
            r->original[0].mode != REGULAR_PCI_MODE) ||
        ((r->write_intent_mask & RESTORE_RESTART_ATTEMPT) &&
            (r->absent_intent_mask || r->empty_intent_mask)) ||
        !r->fingerprint[0] || !r->baseband[0] ||
        !memchr(r->fingerprint, 0, sizeof r->fingerprint) ||
        !memchr(r->baseband, 0, sizeof r->baseband))
        return fail(error, cap, "Recovery record validation failed");
    for (unsigned i = 0; i < LOCK_ITEM_COUNT; ++i) {
        const LockItemSnapshot *s = &r->original[i];
        if (i >= item_count(r)) {
            LockItemSnapshot empty = {0};
            if (memcmp(s, &empty, sizeof empty)) return fail(error, cap, "Recovery spare snapshot is not empty");
            continue;
        }
        if (s->present > 1 || s->length > LOCK_ITEM_LIMIT ||
            (!s->present && (s->mode || s->length)) ||
            (r->legacy_record && s->present) || item_snapshot_supported(item_index(r, i), s, error, cap))
            return fail(error, cap, "Recovery snapshot validation failed");
        if (nr_cell && s->present &&
            ((i == 0 && s->mode != REGULAR_NR_MODE) ||
             (i == 1 && ((s->mode != REGULAR_NR_MODE && s->mode != ITEM_FILE_MODE) ||
                         (s->length != 12 && s->length != 28)))))
            return fail(error, cap, "NR cell recovery baseline mode or length is unsupported");
        for (size_t j = s->length; j < sizeof s->data; ++j)
            if (s->data[j]) return fail(error, cap, "Recovery snapshot has noncanonical trailing bytes");
    }
    Recovery current = {0};
    if (firmware_identity(&current, error, cap)) return -1;
    if (memcmp(r->fingerprint, current.fingerprint, sizeof r->fingerprint) ||
        memcmp(r->baseband, current.baseband, sizeof r->baseband))
        return fail(error, cap, "Firmware or baseband changed since backup; automatic restoration stopped");
    return 0;
}

static size_t expected_item(const Recovery *r, unsigned index, unsigned char out[LOCK_ITEM_LIMIT]) {
    memset(out, 0, LOCK_ITEM_LIMIT);
    if (r->radio_profile == RADIO_NR_SA) {
        if (r->lock_mode == LOCK_MODE_NR_CELL) {
            unsigned arfcn_offset = r->nr_cell_version == 2u ? 20u : 4u;
            unsigned scs_offset = r->nr_cell_version == 2u ? 24u : 8u;
            unsigned pci_offset = r->nr_cell_version == 2u ? 2u : 0u;
            /* Child12 v2 adds one to the caller's SCS code before PUT. */
            if (r->nr_cell_version == 2u) out[0] = 1;
            for (unsigned i = 0; i < 4; ++i) out[arfcn_offset + i] = (unsigned char)(r->nr_arfcn >> (8 * i));
            out[pci_offset] = (unsigned char)r->pci;
            out[pci_offset + 1] = (unsigned char)(r->pci >> 8);
            out[scs_offset] = (unsigned char)(r->nr_scs + (r->nr_cell_version == 2u ? 1u : 0u));
            out[scs_offset + 2] = (unsigned char)(r->nr_band - 1u);
            out[scs_offset + 3] = (unsigned char)((r->nr_band - 1u) >> 8);
            return r->nr_cell_version == 2u ? 28u : 12u;
        }
        for (unsigned i = 0; i < 4; ++i) {
            out[i] = (unsigned char)(r->nr_arfcn >> (8 * i));
            out[4 + i] = (unsigned char)(r->nr_scs >> (8 * i));
        }
        return 256;
    }
    if (r->lock_mode == LOCK_MODE_EARFCN) {
        /* Supplied daemon 0x325d4: second=0 becomes first. The legacy
         * earfcn_lock payload has two LE16 copies; the 36-byte item has
         * two LE32 copies at 28 and 32. There is no PCI wildcard here. */
        if (index == 4) {
            out[0] = out[2] = (unsigned char)r->earfcn;
            out[1] = out[3] = (unsigned char)(r->earfcn >> 8);
            return 4;
        }
        out[0] = 1;
        for (unsigned i = 0; i < 4; ++i)
            out[28 + i] = out[32 + i] = (unsigned char)(r->earfcn >> (8 * i));
        return 36;
    }
    if (index == 0) {
        out[0] = (unsigned char)r->earfcn; out[1] = (unsigned char)(r->earfcn >> 8);
        out[2] = (unsigned char)r->pci; out[3] = (unsigned char)(r->pci >> 8);
        return 4;
    }
    out[0] = 1;
    for (unsigned i = 0; i < 4; ++i) out[20 + i] = (unsigned char)(r->earfcn >> (8 * i));
    out[24] = (unsigned char)r->pci; out[25] = (unsigned char)(r->pci >> 8);
    return 36;
}

static void locked_snapshot(const Recovery *r, unsigned i, LockItemSnapshot *out) {
    memset(out, 0, sizeof *out);
    if (r->radio_profile == RADIO_NR_SA) {
        if (i != (r->lock_mode == LOCK_MODE_NR_CELL ? 1u : 0u)) return;
    } else if (r->lock_mode == LOCK_MODE_EARFCN) {
        if (i != 1 && i != 4) return;
    } else if (i >= 2) return;
    out->present = 1;
    if (r->radio_profile == RADIO_NR_SA) {
        /* Live evidence: frequency PUT38 creates regular256; PCI PUT38
         * creates special12. Retain the old ordinary PCI interpretation for
         * existing journals; new baselines admit only reproducible types. */
        if (r->lock_mode == LOCK_MODE_NR_CELL)
            out->mode = r->original[i].present && r->original[i].mode == REGULAR_NR_MODE ?
                        REGULAR_NR_MODE : ITEM_FILE_MODE;
        else out->mode = r->original[i].present && r->original[i].mode == ITEM_FILE_MODE ?
                         ITEM_FILE_MODE : REGULAR_NR_MODE;
    } else out->mode = i == 1 ? ITEM_FILE_MODE : REGULAR_PCI_MODE;
    out->length = (uint32_t)expected_item(r, i, out->data);
}

static int is_locked(const Recovery *r, unsigned i, const LockItemSnapshot *actual) {
    LockItemSnapshot expected;
    locked_snapshot(r, i, &expected);
    if (snapshot_equal(actual, &expected)) return 1;
    /* Old builds used both pci PUT38 and legacy FS_WRITE; old journals lack
     * modes. Accept their exact bytes and only these two supported modes. */
    if (r->legacy_record && !i) {
        expected.mode = ITEM_FILE_MODE;
        if (snapshot_equal(actual, &expected)) return 1;
    }
    return 0;
}

static int owned_state(const Recovery *r, unsigned i, const LockItemSnapshot *actual) {
    if (!(r->item_intent_mask & (1u << i)))
        return snapshot_equal(actual, &r->original[i]);
    if (snapshot_equal(actual, &r->original[i]) || is_locked(r, i, actual)) return 1;
    /* Older CLREC007 expected ordinary PCI creation. Either historical
     * representation is owned only with the full recorded target; an
     * unexpected type still cannot qualify as successful activation. */
    if (r->lock_mode == LOCK_MODE_NR_CELL && i == 1 &&
        (actual->mode == ITEM_FILE_MODE || actual->mode == REGULAR_NR_MODE)) {
        LockItemSnapshot target;
        locked_snapshot(r, i, &target);
        target.mode = actual->mode;
        if (snapshot_equal(actual, &target)) return 1;
    }
    if ((r->absent_intent_mask & (1u << i)) && !actual->present) return 1;
    /* Only the known regular-create empty intermediate; never partial data. */
    return (r->empty_intent_mask & (1u << i)) && actual->present &&
        actual->mode == REGULAR_PCI_MODE && !actual->length;
}

/* EFS has no compare-and-swap. The last read narrows but cannot eliminate
 * a race with an outside root/DIAG writer between validation and mutation. */
static int require_current(const Recovery *r, unsigned i, const LockItemSnapshot *expected, char *error, size_t cap) {
    LockItemSnapshot actual;
    if (read_snapshot(r, i, &actual, error, cap)) return -1;
    if (!snapshot_equal(&actual, expected))
        return fail(error, cap, "Configuration changed immediately before mutation; recovery record retained");
    return 0;
}

static int commit_item_intent(Recovery *r, unsigned i, int allow_absent, int allow_empty,
                              char *error, size_t cap) {
    Recovery intended = *r;
    intended.item_intent_mask |= 1u << i;
    intended.write_intent_mask = (intended.write_intent_mask & ~WRITE_ITEMS) |
                                (intended.item_intent_mask & WRITE_ITEMS);
    if (allow_absent) intended.absent_intent_mask |= 1u << i;
    if (allow_empty) intended.empty_intent_mask |= 1u << i;
    if (save_recovery(&intended, 0, error, cap)) return -1;
    *r = intended;
    return 0;
}

static int confirmed_unlink(const Recovery *r, unsigned i, const LockItemSnapshot *before, char *error, size_t cap) {
    uint32_t remote = UINT32_MAX;
    int result = item_unlink_checked(item_index(r, i), before, &remote, error, cap);
    log_stage("MUTATION", "unlink_and_get", (int)i, result, remote, SIZE_MAX, 0, error);
    if (result || (remote && remote != 2)) return -1;
    LockItemSnapshot actual;
    if (read_snapshot(r, i, &actual, error, cap)) return -1;
    if (actual.present) return fail(error, cap, "Path removal did not confirm absence; recovery record retained");
    return 0;
}

static int confirmed_put(const Recovery *r, unsigned i, const LockItemSnapshot *before, const LockItemSnapshot *desired, char *error, size_t cap) {
    uint32_t remote = UINT32_MAX;
    int result = item_put_typed(item_index(r, i), desired->data, desired->length, desired->mode,
                                before, &remote, error, cap);
    log_stage("MUTATION", "typed_put_and_get", (int)i, result, remote, SIZE_MAX, desired->length, error);
    if (result || remote) return -1;
    return require_current(r, i, desired, error, cap);
}

static int restore_owned(Recovery *r, char *error, size_t cap) {
    if (parents_ready(r, error, cap)) return -1;
    fprintf(stderr, "PROFILE_RECOVERY_BEGIN intent_mask=%u item_mask=%u\n",
            (unsigned)r->write_intent_mask, (unsigned)r->item_intent_mask);
    for (unsigned i = 0; i < item_count(r); ++i) {
        LockItemSnapshot actual;
        if (read_snapshot(r, i, &actual, error, cap)) return -1;
        if (!owned_state(r, i, &actual))
            return fail(error, cap, "Configuration changed outside this operation; recovery record retained");
    }
    static const unsigned cell_restore_order[LOCK_ITEM_COUNT] = {4, 3, 2, 1, 0};
    static const unsigned frequency_restore_order[LOCK_ITEM_COUNT] = {0, 3, 2, 1, 4};
    static const unsigned nr_restore_order[NR_ITEM_COUNT] = {1, 3, 2, 0};
    static const unsigned nr_cell_restore_order[NR_ITEM_COUNT] = {0, 3, 2, 1};
    const unsigned *order = r->radio_profile == RADIO_NR_SA ?
                            (r->lock_mode == LOCK_MODE_NR_CELL ? nr_cell_restore_order : nr_restore_order) : r->lock_mode == LOCK_MODE_EARFCN ?
                            frequency_restore_order : cell_restore_order;
    for (unsigned step = 0; step < item_count(r); ++step) {
        unsigned i = order[step];
        if (!(r->item_intent_mask & (1u << i))) continue;
        LockItemSnapshot actual;
        const LockItemSnapshot *original = &r->original[i];
        if (read_snapshot(r, i, &actual, error, cap)) return -1;
        if (!owned_state(r, i, &actual))
            return fail(error, cap, "Configuration changed before recovery; record retained");
        if (!snapshot_equal(&actual, original)) {
            int drop = actual.present && (!original->present || actual.mode != original->mode ||
                ((original->mode == REGULAR_PCI_MODE || original->mode == REGULAR_NR_MODE ||
                  (r->radio_profile == RADIO_NR_SA && i == 1)) && actual.length != original->length));
            int create_regular = original->present && original->mode == REGULAR_PCI_MODE &&
                                 (!actual.present || drop);
            if (commit_item_intent(r, i, drop, create_regular, error, cap)) return -1;
            if (require_current(r, i, &actual, error, cap)) return -1;
            if (drop) {
                if (confirmed_unlink(r, i, &actual, error, cap)) return -1;
                memset(&actual, 0, sizeof actual);
            }
            if (original->present) {
                if (require_current(r, i, &actual, error, cap) ||
                    confirmed_put(r, i, &actual, original, error, cap)) return -1;
            }
            if (require_current(r, i, original, error, cap)) return -1;
        }
        Recovery cleared = *r;
        cleared.item_intent_mask &= ~(1u << i);
        cleared.absent_intent_mask &= ~(1u << i);
        cleared.empty_intent_mask &= ~(1u << i);
        cleared.write_intent_mask = (cleared.write_intent_mask & ~WRITE_ITEMS) |
                                   (cleared.item_intent_mask & WRITE_ITEMS);
        if (save_recovery(&cleared, 0, error, cap)) return -1;
        *r = cleared;
    }
    int verified = verify_baseline(r, error, cap);
    log_stage("RECOVERY", "verify_original_snapshot", -1, verified, UINT32_MAX,
              item_count(r), item_count(r), verified ? error : "presence_mode_length_bytes_match");
    return verified;
}

static int remove_recovery(char *error, size_t cap) {
    if (unlink(JOURNAL)) {
        fprintf(stderr, "PROFILE_RECOVERY stage=remove_journal result=-1 local_errno=%d\n", errno);
        return fail(error, cap, "Modem restored, but recovery record could not be removed");
    }
    if (sync_directory(JOURNAL_DIR)) {
        fprintf(stderr, "PROFILE_RECOVERY stage=sync_removed_journal result=-1 local_errno=%d\n", errno);
        return fail(error, cap, "Modem restored, but recovery record removal could not be synchronized");
    }
    fprintf(stderr, "PROFILE_RECOVERY_END result=0 intent_mask=0\n");
    return 0;
}

/* Only pre-activation failures may clean up in the same lock operation.
 * Once a restart might have been sent, recovery is a separate user action. */
static int rollback_unactivated(Recovery *r, char *error, size_t cap) {
    if (r->write_intent_mask & (LOCK_RESTART_ATTEMPT | RESTORE_RESTART_ATTEMPT))
        return fail(error, cap, "Restart may have been submitted; automatic rollback stopped and recovery record retained");
    /* Exact readback establishes the current file state, not durability or
     * applied radio state. Retain the only backup until an explicit restore
     * completes sync, its separately guarded restart, and final cleanup. */
    return restore_owned(r, error, cap);
}

/* The caller must already have the fixed sync receipt and settle interval.
 * Intent is committed before the only restart call. Receipt validation is
 * recorded separately; neither flag establishes successful radio recovery. */
static int restart_after_sync(Recovery *r, int restoring, struct modem_sync_receipt *receipt,
                              char *error, size_t cap) {
    uint32_t attempt = restoring ? RESTORE_RESTART_ATTEMPT : LOCK_RESTART_ATTEMPT;
    uint32_t acknowledged = restoring ? RESTORE_RESTART_ACK : LOCK_RESTART_ACK;
    const char *phase = restoring ? "RESTORE" : "LOCK";
    if (r->write_intent_mask & attempt)
        return fail(error, cap, "Restart phase was already attempted; no automatic duplicate request sent");
    Recovery intended = *r;
    intended.write_intent_mask |= attempt;
    int result = save_recovery(&intended, 0, error, cap);
    log_stage(phase, "commit_restart_intent", -1, result, UINT32_MAX, SIZE_MAX, SIZE_MAX, result ? error : NULL);
    if (result) return -1;
    *r = intended;
    result = modem_restart_once(receipt, error, cap);
    log_stage(phase, "restart_receipt", -1, result, UINT32_MAX,
              receipt->length, 4, result ? error : NULL);
    if (result || receipt->progress != MODEM_REQUEST_RECEIPT_VALIDATED) {
        char cause[256];
        snprintf(cause, sizeof cause, "%s", error && error[0] ? error : "No validated restart receipt");
        if (cap) snprintf(error, cap, "Restart outcome unknown; no duplicate sent, recovery record retained; %.250s", cause);
        return -1;
    }
    Recovery confirmed = *r;
    confirmed.write_intent_mask |= acknowledged;
    result = save_recovery(&confirmed, 0, error, cap);
    log_stage(phase, "commit_restart_ack", -1, result, UINT32_MAX, SIZE_MAX, SIZE_MAX, result ? error : NULL);
    if (result) return -1;
    *r = confirmed;
    return 0;
}

static int lock_item(Recovery *r, unsigned i, char *error, size_t cap) {
    LockItemSnapshot actual, desired;
    locked_snapshot(r, i, &desired);
    if (read_snapshot(r, i, &actual, error, cap)) return -1;
    if (!snapshot_equal(&actual, &r->original[i]))
        return fail(error, cap, "Configuration changed since the original snapshot; lock stopped");
    if (!desired.present && !actual.present) return 0;
    int drop = actual.present && (!desired.present || actual.mode != desired.mode ||
        ((desired.mode == REGULAR_NR_MODE || (r->radio_profile == RADIO_NR_SA && i == 1)) &&
         actual.length != desired.length));
    int create_regular = desired.present && desired.mode == REGULAR_PCI_MODE &&
                         (!actual.present || drop);
    if (commit_item_intent(r, i, drop, create_regular, error, cap)) return -2;
    if (require_current(r, i, &actual, error, cap)) return -1;
    if (drop) {
        if (confirmed_unlink(r, i, &actual, error, cap)) return -1;
        memset(&actual, 0, sizeof actual);
    }
    if (desired.present) {
        if (require_current(r, i, &actual, error, cap) || confirmed_put(r, i, &actual, &desired, error, cap)) return -1;
    }
    return 0;
}

static void describe_rollback(Recovery *r, const char *stage, char *error, size_t cap) {
    char original[256], rollback[256] = {0};
    snprintf(original, sizeof original, "%s", error && error[0] ? error : "No detailed protocol error");
    int recovered = rollback_unactivated(r, rollback, sizeof rollback);
    log_stage("ROLLBACK_RESULT", stage, -1, recovered, UINT32_MAX, SIZE_MAX, SIZE_MAX,
              recovered ? rollback : "original_snapshot_readback_restored_completion_pending");
    if (!cap) return;
    if (!recovered)
        snprintf(error, cap, "Lock failed at %s; original snapshot readback restored; recovery record retained, complete Restore to sync and activate; cause: %.180s", stage, original);
    else snprintf(error, cap, "Lock failed at %s: %.180s; recovery still required: %.180s", stage, original, rollback);
}

static int profile_lock_mode(unsigned mode, unsigned earfcn, unsigned pci,
                             unsigned nr_arfcn, unsigned nr_scs, unsigned nr_band,
                             unsigned nr_cell_version, char *error, size_t cap) {
    if (!cap) error = NULL;
    int nr_cell = mode == LOCK_MODE_NR_CELL;
    int nr = mode == LOCK_MODE_NR_ARFCN || nr_cell;
    if (earfcn > 65535 || pci > (nr_cell ? 1007u : 503u) ||
        (mode != LOCK_MODE_CELL && mode != LOCK_MODE_EARFCN && !nr) ||
        (mode == LOCK_MODE_EARFCN && pci) || nr_arfcn > 3279165u || nr_scs > 4 ||
        (nr ? (earfcn || (!nr_cell && pci)) : (nr_arfcn || nr_scs)) ||
        (nr_cell ? (!nr_band || nr_band > 261u || (nr_cell_version != 1u && nr_cell_version != 2u)) :
                   (nr_band || nr_cell_version)))
        return fail(error, cap, "Invalid lock mode or frequency parameters");
    int guard = acquire(error, cap);
    if (guard < 0) return -1;
    Recovery recovery = {0};
    memcpy(recovery.magic, "CLREC007", 8);
    recovery.lock_mode = mode;
    recovery.earfcn = earfcn; recovery.pci = pci;
    recovery.radio_profile = nr ? RADIO_NR_SA : RADIO_LTE;
    recovery.nr_arfcn = nr_arfcn; recovery.nr_scs = nr_scs;
    recovery.nr_band = nr_band; recovery.nr_cell_version = nr_cell_version;
    int result = readiness_checks(&recovery, error, cap);
    if (!result) result = capture_baseline(&recovery, error, cap);
    if (!result) result = firmware_identity(&recovery, error, cap);
    if (!result) result = save_recovery(&recovery, 1, error, cap);
    log_stage("LOCK", "commit_original_snapshot", -1, result, UINT32_MAX, SIZE_MAX, SIZE_MAX,
              result ? error : NULL);
    if (!result) {
        for (unsigned i = 0; i < item_count(&recovery); ++i)
            fprintf(stderr, "PROFILE_SNAPSHOT item=%u present=%u mode=0o%o length=%u durable=1\n",
                    i, (unsigned)recovery.original[i].present, (unsigned)recovery.original[i].mode,
                    (unsigned)recovery.original[i].length);
        fprintf(stderr, "PROFILE_LOCK_MODE mode=%s earfcn=%u pci=%u nr_arfcn=%u nr_scs=%u nr_band=%u nr_cell_version=%u\n",
                nr_cell ? "nr_arfcn_pci" : mode == LOCK_MODE_NR_ARFCN ? "nr_arfcn_only" : mode == LOCK_MODE_EARFCN ? "earfcn_only" : "earfcn_pci",
                earfcn, pci, nr_arfcn, nr_scs, nr_band, nr_cell_version);
        /* Recovered slot-0 routines: cell starts with pci and clears earfcn;
         * frequency starts with earfcn and clears pci after band cleanup. */
        static const unsigned cell_order[LOCK_ITEM_COUNT] = {0, 1, 2, 3, 4};
        static const unsigned frequency_order[LOCK_ITEM_COUNT] = {4, 1, 2, 3, 0};
        static const unsigned nr_order[NR_ITEM_COUNT] = {0, 2, 3, 1};
        static const unsigned nr_cell_order[NR_ITEM_COUNT] = {1, 2, 3, 0};
        const unsigned *order = nr_cell ? nr_cell_order : mode == LOCK_MODE_NR_ARFCN ? nr_order : mode == LOCK_MODE_EARFCN ? frequency_order : cell_order;
        for (unsigned step = 0; step < item_count(&recovery); ++step) {
            unsigned i = order[step];
            result = lock_item(&recovery, i, error, cap);
            if (result) {
                /* A failed persistence operation never authorizes more
                 * writes in this process, even if the old record is readable. */
                if (result != -2) {
                    char stage[32]; snprintf(stage, sizeof stage, "item_%u", i);
                    describe_rollback(&recovery, stage, error, cap);
                }
                result = -1;
                break;
            }
        }
    }
    if (!result) {
        for (unsigned i = 0; i < item_count(&recovery); ++i) {
            LockItemSnapshot actual;
            result = read_snapshot(&recovery, i, &actual, error, cap);
            if (!result && !is_locked(&recovery, i, &actual))
                result = fail(error, cap, "Configuration changed before activation; lock stopped");
            if (result) break;
        }
        if (result) describe_rollback(&recovery, "before_sync", error, cap);
    }
    if (!result) {
        struct modem_sync_receipt receipt = {0};
        result = modem_sync_prepare(&receipt, error, cap);
        log_stage("LOCK", "sync_receipt_and_settle", -1, result, UINT32_MAX,
                  receipt.length, 14, result ? error : NULL);
        if (result) describe_rollback(&recovery, "sync_prepare", error, cap);
        else result = restart_after_sync(&recovery, 0, &receipt, error, cap);
    }
    close(guard);
    return result;
}

int profile_lock(unsigned earfcn, unsigned pci, char *error, size_t cap) {
    return profile_lock_mode(LOCK_MODE_CELL, earfcn, pci, 0, 0, 0, 0, error, cap);
}

int profile_lock_earfcn(unsigned earfcn, char *error, size_t cap) {
    return profile_lock_mode(LOCK_MODE_EARFCN, earfcn, 0, 0, 0, 0, 0, error, cap);
}

int profile_lock_nr_arfcn(unsigned arfcn, unsigned scs, char *error, size_t cap) {
    return profile_lock_mode(LOCK_MODE_NR_ARFCN, 0, 0, arfcn, scs, 0, 0, error, cap);
}

int profile_lock_nr_cell(unsigned arfcn, unsigned pci, unsigned scs,
                         unsigned band, unsigned version, char *error, size_t cap) {
    return profile_lock_mode(LOCK_MODE_NR_CELL, 0, pci, arfcn, scs, band, version, error, cap);
}

int profile_restore(char *error, size_t cap) {
    if (!cap) error = NULL;
    if (device_ok(error, cap)) return -1;
    int guard = acquire(error, cap);
    if (guard < 0) return -1;
    Recovery recovery;
    int result = load_recovery(&recovery, error, cap);
    log_stage("RESTORE", "load_recovery", -1, result, UINT32_MAX, SIZE_MAX, SIZE_MAX, result ? error : NULL);
    const char *stage = "load_recovery";
    if (result == 0 && (recovery.write_intent_mask & RESTORE_RESTART_ATTEMPT)) {
        stage = "saved_restore_restart";
        if (!(recovery.write_intent_mask & RESTORE_RESTART_ACK))
            result = fail(error, cap, "Restore restart outcome unknown; no duplicate request sent, recovery record retained");
        else {
            /* An ACK cannot authorize new writes or another restart. */
            stage = "saved_restore_original_snapshot";
            result = verify_baseline(&recovery, error, cap);
            if (result == 0) result = remove_recovery(error, cap);
        }
    } else if (result == 0) {
        stage = "restore_owned";
        result = restore_owned(&recovery, error, cap);
        if (result == 0) {
            struct modem_sync_receipt receipt = {0};
            stage = "sync_receipt_and_settle";
            result = modem_sync_prepare(&receipt, error, cap);
            log_stage("RESTORE", stage, -1, result, UINT32_MAX, receipt.length, 14, result ? error : NULL);
            if (result == 0) {
                stage = "restart_and_commit_ack";
                result = restart_after_sync(&recovery, 1, &receipt, error, cap);
            }
            if (result == 0) {
                stage = "remove_journal";
                result = remove_recovery(error, cap);
            }
        }
    }
    log_stage("RESTORE_RESULT", stage, -1, result, UINT32_MAX, SIZE_MAX, SIZE_MAX, result ? error : NULL);
    close(guard);
    return result;
}

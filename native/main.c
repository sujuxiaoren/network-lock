#include "efs.h"
#include "item_probe.h"
#include "profile.h"
#include "modem_sync.h"
#include "transport.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

static const char supported_baseband[] = "MPSS.DE.3.0-Jun  1 2023-07:50:50";
static const char supported_diag_hash[] = "942c44917e383139e1d595aa040a929d1a6a4797efd06b32a3370072e9181d21";
static uint64_t operation_deadline_ms;

static int fail(char *error, size_t cap, const char *message) {
    if (cap) snprintf(error, cap, "%s", message);
    return -1;
}

static int boottime_ms(uint64_t *value, char *error, size_t cap) {
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now) || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L ||
        (uint64_t)now.tv_sec > (UINT64_MAX - 999u) / 1000u)
        return fail(error, cap, "Cannot read the operation deadline clock");
    *value = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
    return 0;
}

static int decimal_u64(const char *text, uint64_t *value) {
    if (!text || !*text) return -1;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return -1;
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || *end || parsed > UINT64_MAX) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int arm_remaining(char *error, size_t cap) {
    uint64_t now;
    if (boottime_ms(&now, error, cap)) return -1;
    if (now >= operation_deadline_ms)
        return fail(error, cap, "Operation deadline expired; DIAG request was not started");
    uint64_t remaining = operation_deadline_ms - now;
    /* Upward rounding is bounded by the initial 45-second process budget. */
    unsigned seconds = remaining >= 45000u ? 45u : (unsigned)((remaining + 999u) / 1000u);
    alarm(seconds);
    return 0;
}

static int native_compatible(char *error, size_t cap) {
    char device[1024] = {0}, sdk[1024] = {0}, baseband[1024] = {0};
    __system_property_get("ro.product.device", device);
    __system_property_get("ro.build.version.sdk", sdk);
    if (strcmp(device, "nuwa") || strcmp(sdk, "33"))
        return fail(error, cap, "Native profile supports only nuwa / Android 13");
    if (__system_property_get("gsm.version.baseband", baseband) <= 0)
        return fail(error, cap, "Cannot confirm the native baseband profile");
    const char *start = baseband;
    for (;;) {
        const char *comma = strchr(start, ',');
        const char *end = comma ? comma : start + strlen(start);
        /* Match Java's trim at segment boundaries; internal spacing is exact. */
        while (start < end && (unsigned char)*start <= ' ') ++start;
        while (end > start && (unsigned char)end[-1] <= ' ') --end;
        if ((size_t)(end - start) != sizeof supported_baseband - 1 ||
            memcmp(start, supported_baseband, sizeof supported_baseband - 1))
            return fail(error, cap, "Baseband version is outside the native profile");
        if (!comma) break;
        start = comma + 1;
    }

    /* All command text is fixed. No user argument is passed through a shell. */
    FILE *pipe = popen("/system/bin/sha256sum /vendor/lib64/libdiag.so", "r");
    if (!pipe) return fail(error, cap, "Cannot start native diagnostic library verification");
    char output[256];
    size_t length = 0;
    int ch, overflow = 0;
    while ((ch = fgetc(pipe)) != EOF) {
        if (length < sizeof output - 1) output[length++] = (char)ch;
        else overflow = 1;
    }
    int read_error = ferror(pipe);
    int status = pclose(pipe);
    if (read_error || overflow || status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail(error, cap, "Native diagnostic library hash command failed");
    while (length && (output[length - 1] == '\n' || output[length - 1] == '\r')) --length;
    output[length] = '\0';
    static const char suffix[] = "  /vendor/lib64/libdiag.so";
    if (length != 64 + sizeof suffix - 1 || memcmp(output, supported_diag_hash, 64) ||
        memcmp(output + 64, suffix, sizeof suffix - 1))
        return fail(error, cap, "Diagnostic library hash differs from the verified native profile");
    return 0;
}

int main(int argc, char **argv) {
    /* Never inherit an ignored/blocked timeout from the launching shell. */
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) return 2;
    alarm(45); /* Also bounds property/hash compatibility checks. */
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, SIGALRM);
    if (sigprocmask(SIG_UNBLOCK, &unblocked, NULL)) return 2;
    char err[512] = {0};
    char summary[ITEM_PROBE_SUMMARY_CAP] = {0};
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getuid() != 0) { puts("CELLLOCK_ERROR=Root required"); return 2; }
    uint64_t started;
    if (boottime_ms(&started, err, sizeof err) || started > UINT64_MAX - 45000u) {
        printf("CELLLOCK_ERROR=%s\n", err[0] ? err : "Invalid operation deadline"); return 2;
    }
    operation_deadline_ms = started + 45000u;
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "--deadline-ms")) {
        uint64_t requested;
        if (argc < 4 || decimal_u64(argv[2], &requested)) {
            puts("CELLLOCK_ERROR=Invalid --deadline-ms value"); return 2;
        }
        if (requested < operation_deadline_ms) operation_deadline_ms = requested;
        first = 3;
    }
    if (arm_remaining(err, sizeof err)) { printf("CELLLOCK_ERROR=%s\n", err); return 2; }
    int remaining_args = argc - first;
    int read_mode = remaining_args == 1 && !strcmp(argv[first], "read");
    int read_nr_mode = remaining_args == 1 && !strcmp(argv[first], "read-nr");
    int restore_mode = remaining_args == 1 && !strcmp(argv[first], "restore");
    int lock_mode = remaining_args == 3 && !strcmp(argv[first], "lock");
    int earfcn_mode = remaining_args == 2 && !strcmp(argv[first], "lock-earfcn");
    int nr_mode = remaining_args == 3 && !strcmp(argv[first], "lock-nr-arfcn");
    int nr_cell_mode = remaining_args == 6 && !strcmp(argv[first], "lock-nr-cell");
    int write_mode = lock_mode || earfcn_mode || nr_mode || nr_cell_mode;
    printf("CELLLOCK_RECOVERY=%d\n", profile_has_recovery());
    if (remaining_args == 1 && !strcmp(argv[first], "status")) return 0;
    if (!read_mode && !read_nr_mode && !restore_mode && !write_mode) return 2;
    uint64_t earfcn = 0, pci = 0, nr_arfcn = 0, nr_scs = 0, nr_band = 0, nr_cell_version = 0;
    if (nr_cell_mode) {
        if (decimal_u64(argv[first + 1], &nr_arfcn) || nr_arfcn > 3279165u ||
            decimal_u64(argv[first + 2], &pci) || pci > 1007u ||
            decimal_u64(argv[first + 3], &nr_scs) || nr_scs > 4u ||
            decimal_u64(argv[first + 4], &nr_band) || !nr_band || nr_band > 261u ||
            decimal_u64(argv[first + 5], &nr_cell_version) || (nr_cell_version != 1u && nr_cell_version != 2u)) {
            puts("CELLLOCK_ERROR=Invalid NR cell parameters"); return 2;
        }
    } else if (nr_mode) {
        if (decimal_u64(argv[first + 1], &nr_arfcn) || nr_arfcn > 3279165u ||
            decimal_u64(argv[first + 2], &nr_scs) || nr_scs > 4) {
            puts("CELLLOCK_ERROR=Invalid NR-ARFCN or SCS"); return 2;
        }
    } else if (write_mode) {
        if (decimal_u64(argv[first + 1], &earfcn) || earfcn > 65535 ||
            (lock_mode && (decimal_u64(argv[first + 2], &pci) || pci > 503))) {
            puts("CELLLOCK_ERROR=Invalid EARFCN or PCI"); return 2;
        }
    }
    if (native_compatible(err, sizeof err) || arm_remaining(err, sizeof err)) {
        printf("CELLLOCK_ERROR=%s\n", err); return 4;
    }
    int session = profile_acquire_session(err, sizeof err);
    if (session < 0) { printf("CELLLOCK_ERROR=%s\n", err); return 4; }
    if (arm_remaining(err, sizeof err)) { printf("CELLLOCK_ERROR=%s\n", err); close(session); return 4; }
    if (diag_open(err, sizeof err) != 0) {
        printf("CELLLOCK_ERROR=%s\n", err); diag_close(); close(session); return 4;
    }
    int rc;
    if (read_mode) {
        rc = probe_lock_items(summary, sizeof summary);
        printf("%s", summary);
        if (rc == 0) puts("CELLLOCK_PROFILE_READ=1");
        int available = rc == 0 && profile_ready(err, sizeof err) == 0;
        printf("CELLLOCK_WRITE_READY=%d\n", available);
        if (!available) printf("CELLLOCK_REASON=%s\n", err[0] ? err : "Invalid item query response");
    } else if (read_nr_mode) {
        rc = probe_nr_lock_items(summary, sizeof summary);
        printf("%s", summary);
        if (rc == 0) puts("CELLLOCK_NR_PROFILE_READ=1");
        int available = rc == 0 && profile_nr_ready(err, sizeof err) == 0;
        printf("CELLLOCK_NR_PROFILE_READY=%d\n", available);
        if (!available) printf("CELLLOCK_REASON=%s\n", err[0] ? err : "Invalid NR item query response");
    } else if (nr_cell_mode) rc = profile_lock_nr_cell((unsigned)nr_arfcn, (unsigned)pci, (unsigned)nr_scs,
                                                    (unsigned)nr_band, (unsigned)nr_cell_version, err, sizeof err);
    else if (nr_mode) rc = profile_lock_nr_arfcn((unsigned)nr_arfcn, (unsigned)nr_scs, err, sizeof err);
    else if (earfcn_mode) rc = profile_lock_earfcn((unsigned)earfcn, err, sizeof err);
    else if (lock_mode) rc = profile_lock((unsigned)earfcn, (unsigned)pci, err, sizeof err);
    else rc = profile_restore(err, sizeof err);
    diag_close();
    close(session);
    printf("CELLLOCK_RECOVERY=%d\n", profile_has_recovery());
    /* This describes only this completed invocation, not a previous attempt
     * retained in the journal. Missing output after a crash stays unknown. */
    if ((write_mode || restore_mode) && modem_restart_progress() == MODEM_REQUEST_NOT_ATTEMPTED)
        puts("CELLLOCK_RESTART_NOT_REQUESTED=1");
    /* A current-process four-byte DIAG receipt, not proof of recovered radio
     * service or persistent cell locking. Stored journal ACKs do not set it. */
    if (modem_restart_progress() == MODEM_REQUEST_RECEIPT_VALIDATED)
        puts("CELLLOCK_RESTART_REQUEST_SENT=1");
    /* Successful explicit lock/restore either persisted this call's receipt
     * or completed a restore with a previously persisted validated receipt. */
    if (rc == 0 && (write_mode || restore_mode))
        puts("CELLLOCK_RESTART_RECEIPT_CONFIRMED=1");
    if (rc != 0) printf("CELLLOCK_ERROR=%s\n", err[0] ? err : "Operation could not be verified");
    else if (write_mode) puts("CELLLOCK_CONFIG_WRITTEN_AND_READBACK=1");
    else if (restore_mode) puts("CELLLOCK_ORIGINAL_CONFIG_RESTORED=1");
    return rc == 0 ? 0 : 5;
}

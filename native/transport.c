/* Independent client of the phone's system libdiag. No vendor binary is bundled.
 * ABI verified against .analysis/device-libdiag.so (Xiaomi nuwa, API 33),
 * SHA256 942c44917e383139e1d595aa040a929d1a6a4797efd06b32a3370072e9181d21.
 * Wire framing reference: P1sec/QCSuper docs/The Diag protocol.md.
 */
#include "transport.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void *library;
static int (*lsm_init)(void *);
static int (*lsm_deinit)(void);
static void (*register_callback)(int (*)(unsigned char *, int, void *), void *);
static int (*switch_logging)(int, const char *);
static int (*send_packet)(int, unsigned char *, int);
static int initialized;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static unsigned char expected[4], reply[4096], frame[8192];
static size_t reply_len, frame_len;
static int awaiting, escaped, dropping, response_error;

static uint16_t crc16(const unsigned char *p, size_t n) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
    }
    return crc;
}

static int matches_reply_header(const unsigned char *p, size_t n) {
    if (n < 4) return 0;
    if (expected[0] == 0x59 && expected[1] == 0x05) {
        /* Legacy FS_WRITE: request sequence is byte 2; response is
         * cmd, operation, EFS1 status, sequence. Pass every status to the
         * caller, which validates the exact response size and result.
         * See the pinned Qualcomm fs_diag.h / fs_diag_i.h definitions. */
        return p[0] == 0x59 && p[1] == 0x05 && p[3] == expected[2];
    }
    return memcmp(p, expected, 4) == 0;
}

static void accept_packet(const unsigned char *p, size_t n) {
    if (!awaiting || reply_len || !n) return;
    /* DIAG negative responses echo the original request header, including
     * for FS_WRITE; they do not use the shifted success-response layout. */
    if ((p[0] == 0x13 || p[0] == 0x14 || p[0] == 0x15 || p[0] == 0x18) &&
        n >= 5 && memcmp(p + 1, expected, 4) == 0) {
        response_error = p[0];
        pthread_cond_signal(&ready);
    } else if (n <= sizeof reply && matches_reply_header(p, n)) {
        memcpy(reply, p, n);
        reply_len = n;
        pthread_cond_signal(&ready);
    }
}

static int on_data(unsigned char *data, int length, void *context) {
    (void)context;
    if (!data || length <= 0 || length > 1024 * 1024) return 0;
    pthread_mutex_lock(&mutex);
    if (length >= 7)
        fprintf(stderr, "RX len=%d first=%02x%02x%02x%02x last=%02x%02x%02x\n",
                length, data[0], data[1], data[2], data[3],
                data[length - 3], data[length - 2], data[length - 1]);
    else
        fprintf(stderr, "RX len=%d short-packet\n", length);
    /* This nuwa/API-33 libdiag profile delivers HDLC callback records, verified
     * by the device GET probe. Only a complete frame with valid FCS is accepted;
     * escaped bytes and partial frames may cross callback boundaries. */
    for (int i = 0; i < length; ++i) {
        unsigned char c = data[i];
        if (c == 0x7e) {
            if (!dropping && !escaped && frame_len >= 3 && crc16(frame, frame_len) == 0xf0b8) {
                accept_packet(frame, frame_len - 2);
            }
            frame_len = 0;
            escaped = dropping = 0;
        } else if (!dropping) {
            if (escaped) { c ^= 0x20; escaped = 0; }
            else if (c == 0x7d) { escaped = 1; continue; }
            if (frame_len < sizeof frame) frame[frame_len++] = c;
            else dropping = 1;
        }
    }
    pthread_mutex_unlock(&mutex);
    return 0;
}

int diag_open(char *err, size_t cap) {
    if (initialized) return 0;
    library = dlopen("/vendor/lib64/libdiag.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { snprintf(err, cap, "dlopen libdiag: %s", dlerror()); return -1; }
    *(void **)(&lsm_init) = dlsym(library, "Diag_LSM_Init");
    *(void **)(&lsm_deinit) = dlsym(library, "Diag_LSM_DeInit");
    *(void **)(&register_callback) = dlsym(library, "diag_register_callback");
    *(void **)(&switch_logging) = dlsym(library, "diag_switch_logging");
    *(void **)(&send_packet) = dlsym(library, "diag_callback_send_data");
    if (!lsm_init || !lsm_deinit || !register_callback || !switch_logging || !send_packet) {
        snprintf(err, cap, "system libdiag lacks required callback API"); return -1;
    }
    if (!lsm_init(NULL)) { snprintf(err, cap, "Diag_LSM_Init rejected initialization"); return -1; }
    initialized = 1;
    register_callback(on_data, NULL);
    int switched = switch_logging(6, NULL); /* CALLBACK_MODE */
    if (switched != 0) {
        snprintf(err, cap, "diag_switch_logging returned %d", switched);
        diag_close();
        return -1;
    }
    return 0;
}

void diag_close(void) {
    if (initialized) { lsm_deinit(); initialized = 0; }
    /* libdiag may retain shutdown threads. Leave its mapping until exit. */
}

int diag_exchange(const unsigned char *request, size_t n, unsigned char *response,
                  size_t cap, size_t *actual, char *err, size_t errcap) {
    if (!initialized || !request || n < 4 || n > 4096 || !response || !actual) {
        snprintf(err, errcap, "invalid or unopened transport"); return -1;
    }
    pthread_mutex_lock(&mutex);
    memcpy(expected, request, 4);
    reply_len = frame_len = 0;
    escaped = dropping = response_error = 0;
    awaiting = 1;
    pthread_mutex_unlock(&mutex);
    int result = send_packet(0, (unsigned char *)request, (int)n);
    pthread_mutex_lock(&mutex);
    if (result != 0) {
        awaiting = 0;
        pthread_mutex_unlock(&mutex);
        snprintf(err, errcap, "diag_callback_send_data returned %d", result);
        return -1;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 6;
    while (!reply_len && !response_error) {
        int wait = pthread_cond_timedwait(&ready, &mutex, &deadline);
        if (wait != 0) break;
    }
    awaiting = 0;
    if (response_error) {
        snprintf(err, errcap, "DIAG error response 0x%02x", response_error);
        result = -1;
    } else if (!reply_len) {
        snprintf(err, errcap, "no matching DIAG response within 6 seconds");
        result = -1;
    } else if (reply_len > cap) {
        snprintf(err, errcap, "DIAG response exceeds receiver capacity");
        result = -1;
    } else {
        memcpy(response, reply, reply_len);
        *actual = reply_len;
        result = 0;
    }
    pthread_mutex_unlock(&mutex);
    return result;
}

/*
 * Independent EFS2 wire-format implementation, restricted to two fixed existing files.
 * Protocol reference: P1sec/QCSuper, commit
 * 9582557c8f6e4baaf9da05b3831bf1a364e977a3 (retrieved 2026-10-11):
 * https://github.com/P1sec/QCSuper/tree/9582557c8f6e4baaf9da05b3831bf1a364e977a3/src/qcsuper
 *   protocol/{messages,subsystems,efs2}.py
 *   modules/efs_shell_commands/{cat,put,stat}.py
 * Only protocol constants/layouts are used; this is not a port of their code.
 * Flags 0 (O_RDONLY) and 2 (O_RDWR) are protocol/Linux ABI values, deliberately
 * independent of the compiling host's fcntl.h. No CREATE or TRUNCATE flag exists
 * in this implementation. A successful CLOSE reply has no echoed fd field.
 */
#include "efs.h"
#include "transport.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { EFS_OPEN = 2, EFS_CLOSE = 3, EFS_READ = 4, EFS_WRITE = 5, EFS_STAT = 15 };
static const char lock_path[] = "/nv/item_files/modem/lte/rrc/csp/pci_lock";
static const char earfcn_path[] = "/nv/item_files/modem/lte/rrc/csp/earfcn_lock";

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

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void header(unsigned char *p, unsigned int cmd)
{
    p[0] = 0x4b;
    p[1] = 0x13;
    p[2] = (unsigned char)cmd;
    p[3] = 0;
}

static int exchange(const unsigned char *req, size_t req_len,
                    unsigned char *resp, size_t cap, size_t *len,
                    const char *operation, char *err, size_t errlen)
{
    int result;
    *len = 0;
    result = diag_exchange(req, req_len, resp, cap, len, err, errlen);
    if (err != NULL && errlen != 0)
        err[errlen - 1] = '\0';
    if (result != 0) {
        if (err != NULL && errlen != 0 && err[0] == '\0')
            return fail(err, errlen, "EFS %s transport failed", operation);
        return -1;
    }
    if (*len > cap || *len < 4)
        return fail(err, errlen, "EFS %s invalid response length: %zu", operation, *len);
    if (memcmp(req, resp, 4) != 0)
        return fail(err, errlen, "EFS %s response command/subsystem mismatch", operation);
    return 0;
}

static int exact_size(size_t actual, size_t expected, const char *operation,
                      char *err, size_t errlen)
{
    if (actual != expected)
        return fail(err, errlen, "EFS %s response length %zu, expected %zu",
                    operation, actual, expected);
    return 0;
}

static int remote_status(uint32_t status, const char *operation,
                         char *err, size_t errlen)
{
    if (status != 0)
        return fail(err, errlen, "EFS %s remote errno=0x%08lx",
                    operation, (unsigned long)status);
    return 0;
}

static int stat_lock(const char *path, size_t path_size, char *err, size_t errlen)
{
    unsigned char req[4 + sizeof earfcn_path];
    unsigned char resp[64];
    size_t len;
    header(req, EFS_STAT);
    memcpy(req + 4, path, path_size);
    if (exchange(req, 4 + path_size, resp, sizeof resp, &len, "STAT", err, errlen) != 0 ||
        exact_size(len, 32, "STAT", err, errlen) != 0 ||
        remote_status(get32(resp + 4), "STAT", err, errlen) != 0)
        return -1;
    if ((get32(resp + 8) & 0170000u) != 0100000u)
        return fail(err, errlen, "EFS STAT: fixed lock path is not a regular file");
    if (get32(resp + 12) != 4)
        return fail(err, errlen, "EFS STAT: fixed lock path size is %lu, expected 4",
                    (unsigned long)get32(resp + 12));
    return 0;
}

static int open_lock(const char *path, size_t path_size, uint32_t flags, uint32_t *fd, char *err, size_t errlen)
{
    unsigned char req[12 + sizeof earfcn_path];
    unsigned char resp[64];
    size_t len;
    header(req, EFS_OPEN);
    put32(req + 4, flags);
    put32(req + 8, 0); /* mode ignored: this function cannot create a file */
    memcpy(req + 12, path, path_size);
    if (exchange(req, 12 + path_size, resp, sizeof resp, &len, "OPEN", err, errlen) != 0 ||
        exact_size(len, 12, "OPEN", err, errlen) != 0 ||
        remote_status(get32(resp + 8), "OPEN", err, errlen) != 0)
        return -1;
    *fd = get32(resp + 4);
    if (*fd > INT32_MAX)
        return fail(err, errlen, "EFS OPEN returned invalid negative file descriptor");
    return 0;
}

static int close_lock(uint32_t fd, char *err, size_t errlen)
{
    unsigned char req[8], resp[64];
    size_t len;
    header(req, EFS_CLOSE);
    put32(req + 4, fd);
    if (exchange(req, sizeof req, resp, sizeof resp, &len, "CLOSE", err, errlen) != 0 ||
        exact_size(len, 8, "CLOSE", err, errlen) != 0)
        return -1;
    return remote_status(get32(resp + 4), "CLOSE", err, errlen);
}

static int finish(uint32_t fd, int result, char *err, size_t errlen)
{
    char close_error[192] = {0};
    if (close_lock(fd, close_error, sizeof close_error) != 0) {
        if (result == 0)
            return fail(err, errlen, "%s", close_error);
        if (err != NULL && errlen != 0) {
            size_t used = strlen(err);
            if (used < errlen - 1)
                (void)snprintf(err + used, errlen - used, "; additionally %s", close_error);
        }
        return -1;
    }
    return result;
}

static int transfer(uint32_t fd, int writing, const unsigned char data[4],
                    unsigned char out[4], char *err, size_t errlen)
{
    unsigned char req[16], resp[64];
    size_t len;
    const char *operation = writing ? "WRITE" : "READ";
    header(req, writing ? EFS_WRITE : EFS_READ);
    put32(req + 4, fd);
    if (writing) {
        put32(req + 8, 0); /* write offset, followed immediately by data */
        memcpy(req + 12, data, 4);
    } else {
        put32(req + 8, 4); /* requested count */
        put32(req + 12, 0); /* read offset */
    }
    if (exchange(req, sizeof req, resp, sizeof resp, &len, operation, err, errlen) != 0)
        return -1;
    if (len < 20)
        return fail(err, errlen, "EFS %s response too short: %zu", operation, len);
    if (get32(resp + 4) != fd || get32(resp + 8) != 0)
        return fail(err, errlen, "EFS %s response fd/offset mismatch", operation);
    if (remote_status(get32(resp + 16), operation, err, errlen) != 0)
        return -1;
    if (get32(resp + 12) != 4)
        return fail(err, errlen, "EFS %s partial/invalid count %lu, expected 4",
                    operation, (unsigned long)get32(resp + 12));
    if (exact_size(len, writing ? 20u : 24u, operation, err, errlen) != 0)
        return -1;
    if (!writing)
        memcpy(out, resp + 20, 4);
    return 0;
}

int efs_read_lock(unsigned char out[4], char *err, size_t errlen)
{
    uint32_t fd;
    unsigned char value[4];
    int result;
    if (err != NULL && errlen != 0)
        err[0] = '\0';
    if (out == NULL)
        return fail(err, errlen, "EFS READ: null output");
    if (stat_lock(lock_path, sizeof lock_path, err, errlen) != 0 ||
        open_lock(lock_path, sizeof lock_path, 0, &fd, err, errlen) != 0)
        return -1;
    result = transfer(fd, 0, NULL, value, err, errlen);
    result = finish(fd, result, err, errlen);
    if (result == 0)
        memcpy(out, value, 4); /* never expose bytes from an unconfirmed operation */
    return result;
}

static int write_fixed_lock(const char *path, size_t path_size, const unsigned char data[4], char *err, size_t errlen)
{
    uint32_t fd;
    int result;
    if (err != NULL && errlen != 0)
        err[0] = '\0';
    if (data == NULL)
        return fail(err, errlen, "EFS WRITE: null input");
    if (stat_lock(path, path_size, err, errlen) != 0 ||
        open_lock(path, path_size, 2, &fd, err, errlen) != 0)
        return -1;
    result = transfer(fd, 1, data, NULL, err, errlen);
    return finish(fd, result, err, errlen);
}

int efs_write_lock(const unsigned char data[4], char *err, size_t errlen)
{
    return write_fixed_lock(lock_path, sizeof lock_path, data, err, errlen);
}

int efs_write_earfcn(const unsigned char data[4], char *err, size_t errlen)
{
    return write_fixed_lock(earfcn_path, sizeof earfcn_path, data, err, errlen);
}

#ifndef LTE_CELL_LOCK_EFS_H
#define LTE_CELL_LOCK_EFS_H

#include <stddef.h>

/*
 * Read/write exactly four bytes in the existing, regular pci_lock EFS file.
 * Return 0 only after validated I/O and CLOSE responses; otherwise return -1.
 * A failed write may have changed the remote file: never blindly retry it.
 * The caller owns backup and read-after-write verification. Calls are serial.
 */
int efs_read_lock(unsigned char out[4], char *err, size_t errlen);
int efs_write_lock(const unsigned char data[4], char *err, size_t errlen);
/* Same guarded four-byte update for the fixed existing earfcn_lock path. */
int efs_write_earfcn(const unsigned char data[4], char *err, size_t errlen);

#endif

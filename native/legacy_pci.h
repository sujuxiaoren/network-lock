#ifndef LTE_CELL_LOCK_LEGACY_PCI_H
#define LTE_CELL_LOCK_LEGACY_PCI_H

#include <stddef.h>
#include <stdint.h>

/* Write four bytes to the fixed slot-0 pci_lock path with the APK's legacy
 * 62-byte FS_WRITE packet. The caller must hold the exclusive DIAG session,
 * confirm the absent baseline/parent directories, and durably record write
 * intent before calling: the request includes the original truncate=1 flag.
 * No retry, alternate write, deletion, radio action, or journal update occurs.
 *
 * status is the EFS1 status byte from a validated legacy reply, NOT an errno.
 * It remains UINT32_MAX if no valid legacy reply was received, and remains
 * zero if the acknowledgement succeeded but a later STAT/GET check failed.
 * Return 0 requires a zero-status four-byte reply, STAT regular-file/size=4,
 * and exact GET39 content verification. It does not prove cell residence.
 * err receives bounded receipt/STAT evidence or the reason for failure.
 */
int legacy_pci_put(const unsigned char data[4], uint32_t *status,
                   char *err, size_t cap);
/* Same contract, fixed slot-0 earfcn_lock path and 65-byte FS_WRITE packet. */
int legacy_earfcn_put(const unsigned char data[4], uint32_t *status,
                      char *err, size_t cap);

#endif

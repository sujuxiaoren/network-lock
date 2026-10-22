#pragma once
#include <stddef.h>
/* nuwa / Android 13 subscription 0 only. Readiness accepts a fully read,
 * supported five-item LTE or four-item NR baseline. Every lock journals original presence, mode,
 * length and bytes before mutation; restore verifies and replays that exact
 * snapshot. Unknown states retain the journal and never imply absence. */
int profile_ready(char *error, size_t capacity);
int profile_nr_ready(char *error, size_t capacity);
int profile_lock(unsigned earfcn, unsigned pci, char *error, size_t capacity);
/* Frequency-only mode carries no PCI; the explicit journal mode remains
 * distinct from a cell lock whose valid PCI happens to be zero. */
int profile_lock_earfcn(unsigned earfcn, char *error, size_t capacity);
/* NR item representation is a 256-byte frequency/SCS pair; no PCI field. */
int profile_lock_nr_arfcn(unsigned arfcn, unsigned scs, char *error, size_t capacity);
/* Version selects the recovered 12-byte (1) or 28-byte (2) PCI format.
 * Band is the actual NR band number; the item carries band minus one. */
int profile_lock_nr_cell(unsigned arfcn, unsigned pci, unsigned scs,
                         unsigned band, unsigned version, char *error, size_t capacity);
int profile_restore(char *error, size_t capacity);
int profile_has_recovery(void);
/* Close the returned fd only after diag_close; negative means no session. */
int profile_acquire_session(char *error, size_t capacity);

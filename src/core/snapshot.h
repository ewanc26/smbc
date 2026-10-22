/* core/snapshot.h — serialised simulation state (save states #25) and a
 * bounded rewind history (#27).
 *
 * Owns: a versioned, little-endian byte image of everything the state hash
 * covers (RAM, both joypads, the PPU, the APU register image), plus a ring
 * of such images for stepping backwards. The images hold no pointers: the
 * CHR and area/enemy streams are host-supplied and stay bound to the Smbc
 * they were loaded into.
 *
 * Not owned: file I/O (the host writes the bytes to disk) and storage (the
 * caller supplies every buffer, so nothing here allocates).
 *
 * Image layout, format v2 (all integers little-endian):
 *   char[8]  magic "SMBCSNAP"
 *   u16      version (= 2)
 *   u16      reserved (0)
 *   ...      payload, in the order snapshot.c writes it
 *
 * The APU write log is per-frame and is drained by the synth, so it is not
 * part of the image; it reads back empty.
 */
#ifndef SMBC_CORE_SNAPSHOT_H
#define SMBC_CORE_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame.h"

#define SMBC_SNAPSHOT_VERSION 2u

/* Exact size of one image. Constant for the format. */
size_t smbc_snapshot_size(void);

/* Write the image into out (cap bytes). False if cap is too small. */
bool smbc_snapshot_write(const Smbc *smbc, uint8_t *out, size_t cap);

/* Read an image back into smbc. Validates the magic, the version and the
 * exact length first; on failure smbc is left untouched and false is
 * returned. Host-bound pointers are kept as they were. */
bool smbc_snapshot_read(Smbc *smbc, const uint8_t *in, size_t len);

/* Rewind history: a ring of the most recent images. The caller supplies
 * storage for exactly `capacity` images (capacity * smbc_snapshot_size()
 * bytes). Pushing past capacity drops the oldest image. */
typedef struct {
  uint8_t *storage;
  uint32_t capacity;
  uint32_t count; /* images currently held */
  uint32_t head;  /* slot the next push writes */
} SmbcHistory;

void history_init(SmbcHistory *history, uint8_t *storage, uint32_t capacity);

/* Record the current state. Always succeeds; the oldest image drops when
 * the ring is full. */
void history_push(SmbcHistory *history, const Smbc *smbc);

/* Restore the most recent image into smbc and remove it from the ring.
 * False when the history is empty (smbc is untouched). */
bool history_pop(SmbcHistory *history, Smbc *smbc);

#endif /* SMBC_CORE_SNAPSHOT_H */

/* tests/core/snapshot.c — save-state round trips, malformed images and the
 * rewind history (#25, #27). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/core/replay.h"
#include "../../src/core/snapshot.h"
#include "../../src/hw/joypad.h"

/* same script shape as the replay tests: Start, then Right with A presses */
static uint8_t script_input(unsigned frame) {
  if (frame == 30) return 0x10u;
  if (frame == 31) return 0x00u;
  if (frame >= 60) return (frame % 90 < 45) ? 0x81u : 0x01u;
  return 0x00u;
}

static void run_frames(Smbc *smbc, unsigned from, unsigned to) {
  for (unsigned i = from; i < to; i++) {
    joypad_set_buttons(&smbc->joypad1, script_input(i));
    smbc_frame(smbc);
  }
}

int main(void) {
  size_t size = smbc_snapshot_size();
  uint8_t *image = malloc(size);
  assert(image != NULL);

  /* the image is a fixed size and refuses a short buffer */
  {
    Smbc smbc;
    smbc_init(&smbc);
    assert(!smbc_snapshot_write(&smbc, image, size - 1));
    assert(smbc_snapshot_write(&smbc, image, size));
    assert(memcmp(image, "SMBCSNAP", 8) == 0);
  }

  /* round trip: a loaded state hashes like the saved one and then runs in
   * lock-step with the original, frame for frame */
  {
    Smbc a, b;
    smbc_init(&a);
    run_frames(&a, 0, 200);
    assert(smbc_snapshot_write(&a, image, size));

    smbc_init(&b);
    run_frames(&b, 0, 37); /* b starts somewhere else entirely */
    assert(smbc_snapshot_read(&b, image, size));
    assert(smbc_state_hash(&a) == smbc_state_hash(&b));

    for (unsigned i = 200; i < 320; i++) {
      joypad_set_buttons(&a.joypad1, script_input(i));
      joypad_set_buttons(&b.joypad1, script_input(i));
      smbc_frame(&a);
      smbc_frame(&b);
      assert(smbc_state_hash(&a) == smbc_state_hash(&b));
    }
  }

  /* a rewritten image is byte-identical: writing is a pure function of the
   * state */
  {
    Smbc a;
    smbc_init(&a);
    run_frames(&a, 0, 90);
    uint8_t *again = malloc(size);
    assert(again != NULL);
    assert(smbc_snapshot_write(&a, image, size));
    assert(smbc_snapshot_write(&a, again, size));
    assert(memcmp(image, again, size) == 0);
    free(again);
  }

  /* malformed images are rejected and leave the machine untouched */
  {
    Smbc good, victim;
    smbc_init(&good);
    run_frames(&good, 0, 150);
    assert(smbc_snapshot_write(&good, image, size));

    smbc_init(&victim);
    run_frames(&victim, 0, 60);
    uint64_t before = smbc_state_hash(&victim);

    assert(!smbc_snapshot_read(&victim, image, size - 1));  /* truncated */
    assert(!smbc_snapshot_read(&victim, image, size + 1));  /* padded */
    assert(!smbc_snapshot_read(&victim, NULL, size));

    uint8_t *bad = malloc(size);
    assert(bad != NULL);

    memcpy(bad, image, size);
    bad[0] = 'X';                                           /* magic */
    assert(!smbc_snapshot_read(&victim, bad, size));

    memcpy(bad, image, size);
    bad[8] = 3;                                             /* version */
    assert(!smbc_snapshot_read(&victim, bad, size));

    /* scroll-log count past its array (before the APU image) */
    memcpy(bad, image, size);
    size_t count_at = size - (SMBC_APU_REGISTER_COUNT + 1u) - (4u + 1u) -
                      SMBC_PPU_SCROLL_LOG * 5u - 1u -
                      SMBC_PPU_SCROLL_LOG * 5u;
    bad[count_at] = SMBC_PPU_SCROLL_LOG + 1u;
    assert(!smbc_snapshot_read(&victim, bad, size));

    /* overrun-carry count past its array (the byte before the carry entries) */
    memcpy(bad, image, size);
    bad[size - SMBC_PPU_SCROLL_LOG * 5u - 1u] = SMBC_PPU_SCROLL_LOG + 1u;
    assert(!smbc_snapshot_read(&victim, bad, size));

    free(bad);
    assert(smbc_state_hash(&victim) == before);
  }

  /* the rewind history restores snapshots newest first and forgets the
   * oldest once full */
  {
    enum { CAP = 3 };
    uint8_t *store = malloc(CAP * size);
    assert(store != NULL);
    SmbcHistory h;
    history_init(&h, store, CAP);

    Smbc smbc;
    smbc_init(&smbc);
    uint64_t hashes[5];
    for (unsigned i = 0; i < 5; i++) {
      run_frames(&smbc, i * 40, i * 40 + 40);
      hashes[i] = smbc_state_hash(&smbc);
      history_push(&h, &smbc);
    }
    assert(h.count == CAP);

    Smbc target;
    smbc_init(&target);
    for (int i = 4; i >= 2; i--) {
      assert(history_pop(&h, &target));
      assert(smbc_state_hash(&target) == hashes[i]);
    }
    assert(h.count == 0);
    assert(!history_pop(&h, &target)); /* the first two were dropped */

    /* after draining, the ring keeps working */
    history_push(&h, &smbc);
    assert(history_pop(&h, &target));
    assert(smbc_state_hash(&target) == hashes[4]);

    free(store);
  }

  free(image);
  printf("snapshot: ok\n");
  return 0;
}

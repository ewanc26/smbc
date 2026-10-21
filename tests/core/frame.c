/* tests/core/frame.c — exercises src/core/frame.c end to end: boot has run,
 * and each frame stores joypad bits in the game's layout and advances the
 * frame counter. */
#include <assert.h>
#include <stdio.h>

#include "../../src/core/frame.h"
#include "../../src/core/state.h"
#include "../../src/hw/joypad.h"

int main(void) {
  Smbc smbc;
  smbc_init(&smbc);

  /* boot ran: warm boot validation and the PRNG seed are set */
  assert(smbc.state.ram[SMBC_WARM_BOOT_VALIDATION] == 0xa5u);
  assert(smbc.state.ram[SMBC_PSEUDO_RANDOM_BIT_REG] == 0xa5u);

  joypad_set_buttons(&smbc.joypad1, SMBC_BUTTON_B | SMBC_BUTTON_UP);
  joypad_set_buttons(&smbc.joypad2, SMBC_BUTTON_START);

  /* frame one runs the title screen's first task — InitializeGame clears
   * the working RAM, freshly-stored joypad bits included */
  smbc_frame(&smbc);
  /* the frame counter lives in the cleared page too */
  assert(smbc.state.ram[SMBC_FRAME_COUNTER] == 0u);

  smbc_frame(&smbc);
  /* game layout: A=d7 ... Right=d0 (B=0x40, Up=0x08, Start=0x10) */
  assert(smbc.state.ram[SMBC_SAVED_JOYPAD1_BITS] == 0x48u);
  assert(smbc.state.ram[SMBC_SAVED_JOYPAD2_BITS] == 0x10u);
  assert(smbc.state.ram[SMBC_FRAME_COUNTER] == 1u);

  /* #41: a first title NMI of 71600 cycles (the issue's measurement, about
   * 2.4 frames) ends 98995 cycles into its frame. Its overrun blocks the
   * vblanks of the next two frames, and the handler runs again at frame 6 —
   * the emulator's pattern: no NMI in frames 4 and 5. */
  {
    uint32_t owed = 0;
    bool ran[4];
    for (unsigned f = 0; f < 4; f++) {
      ran[f] = nmi_timeline_runs(owed);
      uint32_t end = ran[f] ? SMBC_CPU_CYCLES_TO_VBLANK + (f == 0 ? 71600u : 0u)
                            : owed;
      owed = nmi_timeline_carry(end);
    }
    assert(ran[0] && !ran[1] && !ran[2] && ran[3]);
    assert(owed == 0u); /* the last handler (frame 6) was short */
  }

  /* #41: a handler that runs past the frame boundary is owed to the next frame;
   * the next NMI still runs while the owed time is short of the vblank point */
  {
    Smbc owed;
    smbc_init(&owed);
    for (unsigned f = 0; f < 10 && owed.nmi_owed_cycles == 0u; f++) {
      smbc_frame(&owed);
    }
    if (owed.nmi_owed_cycles != 0u) {
      uint32_t carried = owed.nmi_owed_cycles;
      uint8_t before = owed.state.ram[SMBC_FRAME_COUNTER];
      smbc_frame(&owed);
      assert((uint8_t)(owed.state.ram[SMBC_FRAME_COUNTER] - before) == 1u);
      assert(carried <= SMBC_CPU_CYCLES_TO_VBLANK);
    }
  }

  /* charging accumulates into the running handler's cost */
  {
    Smbc charged;
    smbc_init(&charged);
    smbc_charge_cycles(&charged, 100u);
    smbc_charge_cycles(&charged, 25u);
    assert(charged.nmi_cycles == 125u);
  }

  printf("tests/core/frame: ok\n");
  return 0;
}

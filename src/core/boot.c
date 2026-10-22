/* core/boot.c — see core/boot.h for the contract. */
#include "boot.h"

#include "../hw/apu.h"
#include "frame.h"
#include "memory.h"
#include "screen.h"
#include "sprites.h"
#include "state.h"

void boot_run(Smbc *smbc) {
  /* smb.asm: Start */
  CoreState *state = &smbc->state;
  smbc->reset_frames = SMBC_RESET_FRAMES;

  ppu_write(&smbc->ppu, 0, 0x10u); /* lda #%00010000; sta PPU_CTRL_REG1 */

  ppu_await_vblank(&smbc->ppu); /* smb.asm: VBlank1 */
  ppu_await_vblank(&smbc->ppu); /* smb.asm: VBlank2 */

  /* smb.asm: WBootCheck — warm boot only if every top score digit is a
   * valid digit and the validation byte holds $A5 */
  uint8_t offset = SMBC_COLD_BOOT_OFFSET;
  bool warm = true;
  for (int x = 0x05; x >= 0; x--) {
    if (state->ram[SMBC_TOP_SCORE_DISPLAY + x] >= 10u) {
      warm = false;
      break;
    }
  }
  if (warm && state->ram[SMBC_WARM_BOOT_VALIDATION] == 0xa5u) {
    offset = SMBC_WARM_BOOT_OFFSET;
  }

  /* smb.asm: ColdBoot */
  initialize_memory(state, offset);
  apu_write(&smbc->apu, 0x4011u, 0x00u); /* sta SND_DELTA_REG+1 (A is $00) */
  state->ram[SMBC_OPER_MODE] = 0x00u;
  state->ram[SMBC_WARM_BOOT_VALIDATION] = 0xa5u;
  state->ram[SMBC_PSEUDO_RANDOM_BIT_REG] = 0xa5u; /* seed */
  apu_write(&smbc->apu, 0x4015u, 0x0fu); /* all sound channels except DMC */
  ppu_write(&smbc->ppu, 1, 0x06u);       /* no OAM/background clipping */
  move_all_sprites_offscreen(state);
  initialize_name_tables(smbc);
  state->ram[SMBC_DISABLE_SCREEN_FLAG]++;
  write_ppu_reg1(smbc,
                 (uint8_t)(state->ram[SMBC_MIRROR_PPU_CTRL1] | 0x80u));

  /* smb.asm: EndlessLoop — the host frame loop owns time from here */
}

/* core/nmi.c — see core/nmi.h for the contract. */
#include "nmi.h"

#include "../hw/render.h"
#include "frame.h"
#include "joyread.h"
#include "mode.h"
#include "pause.h"
#include "screen.h"
#include "sound.h"
#include "sprites.h"
#include "state.h"

/* smb.asm: DecTimers — frame timers always; interval timers too when the
 * interval control expires (and it then re-arms at $14). */
static void dec_timers(CoreState *state) {
  int end = 0x14;
  state->ram[SMBC_INTERVAL_TIMER_CONTROL]--;
  if ((int8_t)state->ram[SMBC_INTERVAL_TIMER_CONTROL] < 0) {
    state->ram[SMBC_INTERVAL_TIMER_CONTROL] = 0x14u;
    end = 0x23;
  }
  for (int x = end; x >= 0; x--) {
    if (state->ram[SMBC_TIMERS + x] != 0) {
      state->ram[SMBC_TIMERS + x]--;
    }
  }
}

/* smb.asm: the rotate at PauseSkip — 7-byte LFSR from PseudoRandomBitReg,
 * fed by d1 of byte 0 XOR d1 of byte 1. */
static void rotate_pseudo_random(CoreState *state) {
  uint8_t carry =
      (uint8_t)(((state->ram[SMBC_PSEUDO_RANDOM_BIT_REG] ^
                  state->ram[SMBC_PSEUDO_RANDOM_BIT_REG + 1]) &
                 0x02u) != 0);
  for (int i = 0; i < 7; i++) {
    uint8_t v = state->ram[SMBC_PSEUDO_RANDOM_BIT_REG + i];
    uint8_t next_carry = v & 0x01u;
    state->ram[SMBC_PSEUDO_RANDOM_BIT_REG + i] =
        (uint8_t)((v >> 1) | (carry << 7));
    carry = next_carry;
  }
}

void nmi_run(Smbc *smbc) {
  /* smb.asm: NonMaskableInterrupt */
  CoreState *state = &smbc->state;
  Ppu *ppu = &smbc->ppu;

  /* the straight-line prefix to the UpdateScreen call (90), InitScroll's
   * body (14), the sprite DMA stall (513, measured in every ROM NMI), and
   * the taken bne ScreenOff (+1) */
  smbc_charge_cycles(smbc, 90u + 14u + 513u +
                               (state->ram[SMBC_DISABLE_SCREEN_FLAG] != 0 ? 1u : 0u));

  uint8_t mirror1 = (uint8_t)(state->ram[SMBC_MIRROR_PPU_CTRL1] & 0x7fu);
  state->ram[SMBC_MIRROR_PPU_CTRL1] = mirror1;   /* NMIs off in the mirror */
  ppu_write(ppu, 0, (uint8_t)(mirror1 & 0x7eu)); /* name table $2000 */

  uint8_t mirror2 = (uint8_t)(state->ram[SMBC_MIRROR_PPU_CTRL2] & 0xe6u);
  if (state->ram[SMBC_DISABLE_SCREEN_FLAG] == 0) {
    mirror2 = (uint8_t)(state->ram[SMBC_MIRROR_PPU_CTRL2] | 0x1eu);
  }
  state->ram[SMBC_MIRROR_PPU_CTRL2] = mirror2;   /* smb.asm: ScreenOff */
  ppu_write(ppu, 1, (uint8_t)(mirror2 & 0xe7u));

  (void)ppu_read(ppu, 2);       /* ldx PPU_STATUS: reset the flip-flop */
  init_scroll(smbc, 0x00u);
  ppu_write(ppu, 3, 0x00u);     /* sta PPU_SPR_ADDR */
  /* lda #$02; sta SPR_DMA — copy the $0200 sprite page into OAM */
  ppu_oam_dma(ppu, &state->ram[SMBC_SPRITE_DATA]);

  load_vram_addr_table(state);  /* smb.asm: set the $00/$01 indirect */
  update_screen(smbc);

  /* smb.asm: InitBuffer — clear the active VRAM buffer's header via the
   * VRAM_Buffer_Offset table ($00, or $40 when AddrCtrl is $06) */
  uint8_t buf_off = state->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] == 0x06u ? 0x40u : 0x00u;
  state->ram[SMBC_VRAM_BUFFER1_OFFSET + buf_off] = 0x00u;
  state->ram[SMBC_VRAM_BUFFER1 + buf_off] = 0x00u;
  state->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] = 0x00u;
  ppu_write(ppu, 1, state->ram[SMBC_MIRROR_PPU_CTRL2]);

  sound_engine(smbc);           /* smb.asm: SoundEngine */
  read_joypads(smbc);
  pause_routine(smbc);
  update_top_score(state);

  if ((state->ram[SMBC_GAME_PAUSE_STATUS] & 0x01u) == 0) {
    uint8_t control = state->ram[SMBC_TIMER_CONTROL];
    if (control == 0 || --state->ram[SMBC_TIMER_CONTROL] == 0) {
      dec_timers(state);
    }
    state->ram[SMBC_FRAME_COUNTER]++; /* smb.asm: NoDecTimers */
  }

  rotate_pseudo_random(state);  /* smb.asm: PauseSkip */

  if (state->ram[SMBC_SPRITE0_HIT_FLAG] != 0) {
    ppu_await_sprite0_clear(ppu); /* smb.asm: Sprite0Clr */
    if ((state->ram[SMBC_GAME_PAUSE_STATUS] & 0x01u) == 0) {
      move_sprites_offscreen(state);
      sprite_shuffler(state);
    }
    /* smb.asm: Sprite0Hit — the render layer evaluates the overlap, the
     * timing layer advances the raster clock to it */
    ppu_await_sprite0_set(ppu, ppu_sprite0_hit_scanline(ppu));
    /* smb.asm: HBlankDelay — sub-scanline hardware timing, absorbed into
     * the hit scanline (see the timing contract in hw/ppu.h) */
  }

  /* smb.asm: SkipSprite0 — set scroll from the variables, write mirror */
  ppu_write(ppu, 5, state->ram[SMBC_HORIZONTAL_SCROLL]);
  ppu_write(ppu, 5, state->ram[SMBC_VERTICAL_SCROLL]);
  uint8_t saved_mirror1 = state->ram[SMBC_MIRROR_PPU_CTRL1]; /* pha */
  ppu_write(ppu, 0, saved_mirror1);

  if ((state->ram[SMBC_GAME_PAUSE_STATUS] & 0x01u) == 0) {
    oper_mode_execution_tree(smbc);
  }

  /* smb.asm: SkipMainOper — reset flip-flop, reactivate NMIs; note the
   * mirror itself keeps d7 clear here, matching the original */
  (void)ppu_read(ppu, 2);
  ppu_write(ppu, 0, (uint8_t)(saved_mirror1 | 0x80u));
}

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
 * interval control expires (and it then re-arms at $14). Returns the cycles
 * of the path taken; no branch here crosses a page. */
static uint32_t dec_timers(CoreState *state) {
  uint32_t cycles = 2u + 6u; /* ldx #$14; dec IntervalTimerControl */
  int end = 0x14;
  state->ram[SMBC_INTERVAL_TIMER_CONTROL]--;
  if ((int8_t)state->ram[SMBC_INTERVAL_TIMER_CONTROL] < 0) {
    cycles += 2u + 2u + 4u + 2u; /* bpl, not taken; lda #$14; sta; ldx #$23 */
    state->ram[SMBC_INTERVAL_TIMER_CONTROL] = 0x14u;
    end = 0x23;
  } else {
    cycles += 3u; /* bpl DecTimersLoop, taken */
  }
  for (int x = end; x >= 0; x--) {
    cycles += 4u; /* lda Timers,x */
    if (state->ram[SMBC_TIMERS + x] == 0) {
      cycles += 3u; /* beq SkipExpTimer, taken */
    } else {
      state->ram[SMBC_TIMERS + x]--;
      cycles += 2u + 7u; /* beq, not taken; dec Timers,x */
    }
    cycles += 2u + (x > 0 ? 3u : 2u); /* dex; bpl */
  }
  return cycles;
}

/* smb.asm: the rotate at PauseSkip — 7-byte LFSR from PseudoRandomBitReg,
 * fed by d1 of byte 0 XOR d1 of byte 1. */
static uint32_t rotate_pseudo_random(CoreState *state) {
  /* ldx; ldy; lda abs; and; sta $00; lda abs; and; eor $00; clc */
  uint32_t cycles = 2u + 2u + 4u + 2u + 3u + 4u + 2u + 3u + 2u;
  uint8_t carry =
      (uint8_t)(((state->ram[SMBC_PSEUDO_RANDOM_BIT_REG] ^
                  state->ram[SMBC_PSEUDO_RANDOM_BIT_REG + 1]) &
                 0x02u) != 0);
  cycles += carry ? 2u + 2u /* beq not taken; sec */ : 3u /* beq taken */;
  for (int i = 0; i < 7; i++) {
    uint8_t v = state->ram[SMBC_PSEUDO_RANDOM_BIT_REG + i];
    uint8_t next_carry = v & 0x01u;
    state->ram[SMBC_PSEUDO_RANDOM_BIT_REG + i] =
        (uint8_t)((v >> 1) | (carry << 7));
    carry = next_carry;
  }
  cycles += 7u * (7u + 2u + 2u) + 6u * 3u + 2u; /* ror abs,x; inx; dey; bne */
  return cycles;
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
  smbc_charge_cycles(smbc, 6u + read_joypads(smbc)); /* jsr ReadJoypads */
  smbc_charge_cycles(smbc, 6u); /* jsr PauseRoutine; its path is charged inside */
  pause_routine(smbc);
  smbc_charge_cycles(smbc, 6u + update_top_score(state)); /* jsr UpdateTopScore */

  if ((state->ram[SMBC_GAME_PAUSE_STATUS] & 0x01u) != 0) {
    smbc_charge_cycles(smbc, 4u + 2u + 4u); /* lda GamePauseStatus; lsr; bcs PauseSkip, taken across a page */
  } else {
    uint32_t cycles = 4u + 2u + 2u + 4u; /* lda GamePauseStatus; lsr; bcs; lda TimerControl */
    if (state->ram[SMBC_TIMER_CONTROL] == 0) {
      cycles += 4u + dec_timers(state); /* beq DecTimers, taken across a page */
    } else {
      cycles += 2u + 6u; /* beq, not taken; dec TimerControl */
      if (--state->ram[SMBC_TIMER_CONTROL] == 0) {
        cycles += 2u + dec_timers(state); /* bne, not taken */
      } else {
        cycles += 3u; /* bne NoDecTimers, taken */
      }
    }
    state->ram[SMBC_FRAME_COUNTER]++; /* smb.asm: NoDecTimers */
    smbc_charge_cycles(smbc, cycles + 5u); /* inc FrameCounter */
  }

  /* smb.asm: PauseSkip, then the sprite-0 flag test (lda abs; beq taken when zero) */
  smbc_charge_cycles(smbc, rotate_pseudo_random(state) + 4u +
                               (state->ram[SMBC_SPRITE0_HIT_FLAG] != 0 ? 2u : 3u));

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

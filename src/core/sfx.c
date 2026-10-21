/* core/sfx.c — the three per-channel sound effect handlers.
 *
 * Owns: smb.asm: Square1SfxHandler, Square2SfxHandler and NoiseSfxHandler,
 * with their Play and Continue routines. Called once per frame from
 * core/sound (RunSoundSubroutines). Register writes go to the hw/apu
 * register capture through the helpers in core/sound; the tables come from
 * data/music.
 */
#include "frame.h"
#include "internal.h"
#include "state.h"
#include "../data/music.h"
#include "../hw/apu.h"

/* smb.asm: SND_MASTERCTRL_REG/SND_SQUARE1_REG/SND_SQUARE2_REG/SND_NOISE_REG */
#define APU_MASTERCTRL 0x4015u
#define APU_SQUARE1    0x4000u
#define APU_SQUARE2    0x4004u
#define APU_NOISE      0x400cu

/* ---- square 1 ------------------------------------------------------------ */

/* smb.asm: the frequency-register load. Dump_Freq_Regs costs 30 cycles when it
 * writes the registers and 15 when NoTone skips them (both include the rts). */
static uint32_t freq_cycles(bool wrote) { return wrote ? 30u : 15u; }

/* smb.asm: DecrementSfx1Length, including the rts; the stop path adds StopSquare1Sfx */
static uint32_t decrement_sfx1_length(Smbc *smbc) {
  CoreState *st = &smbc->state;
  if (--st->ram[SMBC_SQU1_SFX_LEN_COUNTER] != 0) {
    return 15u;
  }
  sound_stop_square1_sfx(smbc);
  return 31u;
}

/* smb.asm: jsr PlaySqu1Sfx (Dump_Squ1_Regs, then SetFreq_Squ1) */
static uint32_t play_squ1(Smbc *smbc, uint8_t a, uint8_t x, uint8_t y) {
  return 6u + 22u + freq_cycles(sound_play_squ1_sfx(smbc, a, x, y));
}

/* smb.asm: ContinueSndJump — the three-part jump envelope, then DecrementSfx1Length */
static uint32_t continue_snd_jump(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t a = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
  uint32_t cycles = 21u;
  if (a == 0x25u) {
    sound_dump_squ1_regs(smbc, 0x5fu, 0xf6u);
    cycles = 45u;
  } else if (a == 0x20u) {
    sound_dump_squ1_regs(smbc, 0x48u, 0xbcu); /* smb.asm: FPS2nd */
    cycles = 47u;
  }
  return cycles + decrement_sfx1_length(smbc);
}

/* smb.asm: ContinueBumpThrow */
static uint32_t continue_bump_throw(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint32_t cycles = 15u;
  if (st->ram[SMBC_SQU1_SFX_LEN_COUNTER] == 0x06u) {
    apu_write(&smbc->apu, APU_SQUARE1 + 1u, 0xbbu);
    cycles = 20u;
  }
  return cycles + decrement_sfx1_length(smbc);
}

/* smb.asm: ContinueSwimStomp */
static uint32_t continue_swim_stomp(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t y = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
  apu_write(&smbc->apu, APU_SQUARE1, SMBC_SWIM_STOMP_ENVELOPE_DATA[y - 1u]);
  uint32_t cycles = 20u;
  if (y == 0x06u) {
    apu_write(&smbc->apu, APU_SQUARE1 + 2u, 0x9eu);
    cycles = 25u;
  }
  return cycles + decrement_sfx1_length(smbc);
}

/* smb.asm: ContinueSmackEnemy */
static uint32_t continue_smack_enemy(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t y = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
  if (y == 0x08u) {
    apu_write(&smbc->apu, APU_SQUARE1 + 2u, 0xa0u);
    apu_write(&smbc->apu, APU_SQUARE1, 0x9fu);
    return 23u + decrement_sfx1_length(smbc);
  }
  apu_write(&smbc->apu, APU_SQUARE1, 0x90u);
  return 15u + decrement_sfx1_length(smbc);
}

/* smb.asm: ContinuePipeDownInj — the gate shifts the length counter twice; the
 * carry rejects d0 and d1, then d3 is tested through the shifted value. */
static uint32_t continue_pipe_down_inj(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t a = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
  uint32_t cycles;
  if ((a & 0x01u) != 0) {
    cycles = 9u;
  } else if ((a & 0x02u) != 0) {
    cycles = 13u;
  } else if ((a & 0x08u) == 0) {
    cycles = 17u;
  } else {
    cycles = 16u + 6u + play_squ1(smbc, 0x44u, 0x9au, 0x91u);
  }
  cycles += 3u; /* jmp DecrementSfx1Length */
  return cycles + decrement_sfx1_length(smbc);
}

/* smb.asm: Square1SfxHandler — the queue dispatch, then CheckSfx1Buffer */
uint32_t sfx_square1_handler(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t y = st->ram[SMBC_SQUARE1_SOUND_QUEUE];
  if (y != 0) {
    st->ram[SMBC_SQUARE1_SOUND_BUFFER] = y;
    uint32_t cycles;
    if ((y & 0x80u) != 0) { /* smb.asm: PlaySmallJump */
      cycles = 12u + 15u + play_squ1(smbc, 0x26u, 0x82u, 0xa7u);
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x28u;
      return cycles + continue_snd_jump(smbc);
    }
    if ((y & SMBC_SFX_BIG_JUMP) != 0) { /* smb.asm: PlayBigJump */
      cycles = 19u + 12u + play_squ1(smbc, 0x18u, 0x82u, 0xa7u);
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x28u;
      return cycles + continue_snd_jump(smbc);
    }
    if ((y & SMBC_SFX_BUMP) != 0) { /* smb.asm: PlayBump */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x0au;
      cycles = 26u + 12u + play_squ1(smbc, 0x0cu, 0x9eu, 0x93u);
      return cycles + continue_bump_throw(smbc);
    }
    if ((y & SMBC_SFX_ENEMY_STOMP) != 0) { /* smb.asm: PlaySwimStomp */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x0eu;
      cycles = 32u + 12u + play_squ1(smbc, 0x26u, 0x9eu, 0x9cu);
      return cycles + continue_swim_stomp(smbc);
    }
    if ((y & SMBC_SFX_ENEMY_SMACK) != 0) { /* smb.asm: PlaySmackEnemy */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x0eu;
      cycles = 39u + 15u + play_squ1(smbc, 0x28u, 0x9fu, 0xcbu);
      return cycles + decrement_sfx1_length(smbc);
    }
    if ((y & SMBC_SFX_PIPE_DOWN_INJURY) != 0) { /* smb.asm: PlayPipeDownInj */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x2fu;
      cycles = 46u + 6u;
      return cycles + continue_pipe_down_inj(smbc);
    }
    if ((y & SMBC_SFX_FIREBALL) != 0) { /* smb.asm: PlayFireballThrow */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x05u;
      cycles = 54u + 16u + play_squ1(smbc, 0x0cu, 0x9eu, 0x99u);
      return cycles + continue_bump_throw(smbc);
    }
    /* smb.asm: PlayFlagpoleSlide */
    st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x40u;
    cycles = 61u + 2u + 4u + 2u + 6u + 2u /* ldx #$00 in SetFreq_Squ1 */ +
             freq_cycles(sound_set_freq_squ1(smbc, 0x62u));
    cycles += 2u + 3u; /* ldx #$99; bne FPS2nd */
    sound_dump_squ1_regs(smbc, 0x99u, 0xbcu);
    cycles += 2u + 6u + 14u + 4u + 3u + 3u; /* ldy; jsr dump; bne DecJpFPS, taken across a page; bne BranchToDecLength1; bne DecrementSfx1Length */
    return cycles + decrement_sfx1_length(smbc);
  }

  /* smb.asm: CheckSfx1Buffer */
  uint8_t a = st->ram[SMBC_SQUARE1_SOUND_BUFFER];
  if (a == 0) {
    return 18u; /* smb.asm: ExS1H */
  }
  if ((a & 0x80u) != 0) {
    return 15u + continue_snd_jump(smbc);
  }
  if ((a & SMBC_SFX_BIG_JUMP) != 0) {
    return 19u + continue_snd_jump(smbc);
  }
  if ((a & SMBC_SFX_BUMP) != 0) {
    return 22u + continue_bump_throw(smbc);
  }
  if ((a & SMBC_SFX_ENEMY_STOMP) != 0) {
    return 26u + continue_swim_stomp(smbc);
  }
  if ((a & SMBC_SFX_ENEMY_SMACK) != 0) {
    return 30u + continue_smack_enemy(smbc);
  }
  if ((a & SMBC_SFX_PIPE_DOWN_INJURY) != 0) {
    return 34u + continue_pipe_down_inj(smbc);
  }
  if ((a & SMBC_SFX_FIREBALL) != 0) {
    return 38u + continue_bump_throw(smbc);
  }
  return 42u + decrement_sfx1_length(smbc);
}

/* ---- square 2 ------------------------------------------------------------ */

/* smb.asm: DecrementSfx2Length */
static void decrement_sfx2_length(Smbc *smbc) {
  CoreState *st = &smbc->state;
  if (--st->ram[SMBC_SQU2_SFX_LEN_COUNTER] == 0) {
    /* smb.asm: EmptySfx2Buffer */
    st->ram[SMBC_SQUARE2_SOUND_BUFFER] = 0x00u;
    sound_stop_square2_sfx(smbc);
  }
}

/* smb.asm: ContinueCGrabTTick */
static void continue_cgrab_ttick(Smbc *smbc) {
  CoreState *st = &smbc->state;
  if (st->ram[SMBC_SQU2_SFX_LEN_COUNTER] == 0x30u) {
    apu_write(&smbc->apu, APU_SQUARE2 + 2u, 0x54u);
  }
  decrement_sfx2_length(smbc);
}

/* smb.asm: ContinueBlast / ContinueBowserFall — shared reg-load tail is
 * PBFRegs; only the trigger lengths and contents differ. */
static void continue_blast(Smbc *smbc) {
  CoreState *st = &smbc->state;
  if (st->ram[SMBC_SQU2_SFX_LEN_COUNTER] == 0x18u) {
    sound_play_squ2_sfx(smbc, 0x18u, 0x9fu, 0x93u);
  }
  decrement_sfx2_length(smbc);
}

static void continue_bowser_fall(Smbc *smbc) {
  CoreState *st = &smbc->state;
  if (st->ram[SMBC_SQU2_SFX_LEN_COUNTER] == 0x08u) {
    sound_play_squ2_sfx(smbc, 0x5au, 0x9fu, 0xa4u);
  }
  decrement_sfx2_length(smbc);
}

/* smb.asm: ContinuePowerUpGrab */
static void continue_power_up_grab(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t a = st->ram[SMBC_SQU2_SFX_LEN_COUNTER];
  if ((a & 0x01u) == 0) { /* alter frequency every other frame */
    sound_play_squ2_sfx(smbc, SMBC_POWER_UP_GRAB_FREQ_DATA[(a >> 1) - 1u],
                        0x5du, 0x7fu);
  }
  decrement_sfx2_length(smbc);
}

/* smb.asm: ContinueExtraLife — new tone only every eight frames */
static void continue_extra_life(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t a = st->ram[SMBC_SQU2_SFX_LEN_COUNTER];
  if ((a & 0x07u) == 0) {
    sound_play_squ2_sfx(smbc, SMBC_EXTRA_LIFE_FREQ_DATA[(a >> 3) - 1u],
                        0x82u, 0x7fu);
  }
  decrement_sfx2_length(smbc);
}

/* smb.asm: ContinueGrowItems — secondary counter drives this one; the usual
 * length counter only marks the end. */
static void continue_grow_items(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t y = (uint8_t)(++st->ram[SMBC_SFX_SECONDARY_COUNTER] >> 1);
  if (y == st->ram[SMBC_SQU2_SFX_LEN_COUNTER]) {
    /* smb.asm: StopGrowItems */
    st->ram[SMBC_SQUARE2_SOUND_BUFFER] = 0x00u;
    sound_stop_square2_sfx(smbc);
    return;
  }
  apu_write(&smbc->apu, APU_SQUARE2, 0x9du);
  sound_set_freq_squ2(smbc, SMBC_PUP_VGROW_FREQ_DATA[y]);
}

/* smb.asm: Square2SfxHandler */
void sfx_square2_handler(Smbc *smbc) {
  CoreState *st = &smbc->state;

  /* the 1-up sound is never interrupted by other square 2 sfx */
  if ((st->ram[SMBC_SQUARE2_SOUND_BUFFER] & SMBC_SFX_EXTRA_LIFE) != 0) {
    continue_extra_life(smbc);
    return;
  }

  uint8_t y = st->ram[SMBC_SQUARE2_SOUND_QUEUE];
  if (y != 0) {
    st->ram[SMBC_SQUARE2_SOUND_BUFFER] = y;
    if ((y & 0x80u) != 0) { /* smb.asm: PlayBowserFall */
      st->ram[SMBC_SQU2_SFX_LEN_COUNTER] = 0x38u;
      sound_play_squ2_sfx(smbc, 0x18u, 0x9fu, 0xc4u);
      decrement_sfx2_length(smbc);
      return;
    }
    if ((y & SMBC_SFX_COIN_GRAB) != 0) { /* smb.asm: PlayCoinGrab */
      st->ram[SMBC_SQU2_SFX_LEN_COUNTER] = 0x35u;
      sound_play_squ2_sfx(smbc, 0x42u, 0x8du, 0x7fu);
      continue_cgrab_ttick(smbc);
      return;
    }
    if ((y & SMBC_SFX_GROW_POWER_UP) != 0) { /* smb.asm: PlayGrowPowerUp */
      st->ram[SMBC_SQU2_SFX_LEN_COUNTER] = 0x10u;
      apu_write(&smbc->apu, APU_SQUARE2 + 1u, 0x7fu);
      st->ram[SMBC_SFX_SECONDARY_COUNTER] = 0x00u;
      continue_grow_items(smbc);
      return;
    }
    if ((y & SMBC_SFX_GROW_VINE) != 0) { /* smb.asm: PlayGrowVine */
      st->ram[SMBC_SQU2_SFX_LEN_COUNTER] = 0x20u;
      apu_write(&smbc->apu, APU_SQUARE2 + 1u, 0x7fu);
      st->ram[SMBC_SFX_SECONDARY_COUNTER] = 0x00u;
      continue_grow_items(smbc);
      return;
    }
    if ((y & SMBC_SFX_BLAST) != 0) { /* smb.asm: PlayBlast */
      st->ram[SMBC_SQU2_SFX_LEN_COUNTER] = 0x20u;
      sound_play_squ2_sfx(smbc, 0x5eu, 0x9fu, 0x94u);
      decrement_sfx2_length(smbc);
      return;
    }
    if ((y & SMBC_SFX_TIMER_TICK) != 0) { /* smb.asm: PlayTimerTick */
      st->ram[SMBC_SQU2_SFX_LEN_COUNTER] = 0x06u;
      sound_play_squ2_sfx(smbc, 0x42u, 0x98u, 0x7fu);
      continue_cgrab_ttick(smbc);
      return;
    }
    if ((y & SMBC_SFX_POWER_UP_GRAB) != 0) { /* smb.asm: PlayPowerUpGrab */
      st->ram[SMBC_SQU2_SFX_LEN_COUNTER] = 0x36u;
      continue_power_up_grab(smbc);
      return;
    }
    /* smb.asm: PlayExtraLife */
    st->ram[SMBC_SQU2_SFX_LEN_COUNTER] = 0x30u;
    continue_extra_life(smbc);
    return;
  }

  /* smb.asm: CheckSfx2Buffer */
  uint8_t a = st->ram[SMBC_SQUARE2_SOUND_BUFFER];
  if (a == 0) {
    return; /* smb.asm: ExS2H */
  }
  if ((a & 0x80u) != 0) {
    continue_bowser_fall(smbc);
  } else if ((a & (SMBC_SFX_COIN_GRAB | SMBC_SFX_TIMER_TICK)) != 0) {
    continue_cgrab_ttick(smbc);
  } else if ((a & (SMBC_SFX_GROW_POWER_UP | SMBC_SFX_GROW_VINE)) != 0) {
    continue_grow_items(smbc);
  } else if ((a & SMBC_SFX_BLAST) != 0) {
    continue_blast(smbc);
  } else if ((a & SMBC_SFX_POWER_UP_GRAB) != 0) {
    continue_power_up_grab(smbc);
  } else { /* extra life */
    continue_extra_life(smbc);
  }
}

/* ---- noise --------------------------------------------------------------- */

/* smb.asm: PlayNoiseSfx */
static void play_noise_sfx(Smbc *smbc, uint8_t a, uint8_t x) {
  apu_write(&smbc->apu, APU_NOISE, a);
  apu_write(&smbc->apu, APU_NOISE + 2u, x);
  apu_write(&smbc->apu, APU_NOISE + 3u, 0x18u);
}

/* smb.asm: DecrementSfx3Length */
static void decrement_sfx3_length(Smbc *smbc) {
  CoreState *st = &smbc->state;
  if (--st->ram[SMBC_NOISE_SFX_LEN_COUNTER] == 0) {
    apu_write(&smbc->apu, APU_NOISE, 0xf0u);
    st->ram[SMBC_NOISE_SOUND_BUFFER] = 0x00u;
  }
}

/* smb.asm: ContinueBrickShatter */
static void continue_brick_shatter(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t a = st->ram[SMBC_NOISE_SFX_LEN_COUNTER];
  if ((a & 0x01u) != 0) {
    uint8_t y = a >> 1;
    play_noise_sfx(smbc, SMBC_BRICK_SHATTER_ENV_DATA[y],
                   SMBC_BRICK_SHATTER_FREQ_DATA[y]);
  }
  decrement_sfx3_length(smbc);
}

/* smb.asm: ContinueBowserFlame */
static void continue_bowser_flame(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t y = st->ram[SMBC_NOISE_SFX_LEN_COUNTER] >> 1;
  play_noise_sfx(smbc, SMBC_BOWSER_FLAME_ENV_DATA[y - 1u], 0x0fu);
  decrement_sfx3_length(smbc);
}

/* smb.asm: NoiseSfxHandler */
void sfx_noise_handler(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t y = st->ram[SMBC_NOISE_SOUND_QUEUE];
  if (y != 0) {
    st->ram[SMBC_NOISE_SOUND_BUFFER] = y;
    if ((y & SMBC_SFX_BRICK_SHATTER) != 0) { /* smb.asm: PlayBrickShatter */
      st->ram[SMBC_NOISE_SFX_LEN_COUNTER] = 0x20u;
      continue_brick_shatter(smbc);
      return;
    }
    if ((y & SMBC_SFX_BOWSER_FLAME) != 0) { /* smb.asm: PlayBowserFlame */
      st->ram[SMBC_NOISE_SFX_LEN_COUNTER] = 0x40u;
      continue_bowser_flame(smbc);
      return;
    }
  }

  /* smb.asm: CheckNoiseBuffer */
  uint8_t a = st->ram[SMBC_NOISE_SOUND_BUFFER];
  if ((a & SMBC_SFX_BRICK_SHATTER) != 0) {
    continue_brick_shatter(smbc);
  } else if ((a & SMBC_SFX_BOWSER_FLAME) != 0) {
    continue_bowser_flame(smbc);
  }
}

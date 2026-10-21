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

/* smb.asm: DecrementSfx1Length */
static void decrement_sfx1_length(Smbc *smbc) {
  CoreState *st = &smbc->state;
  if (--st->ram[SMBC_SQU1_SFX_LEN_COUNTER] == 0) {
    sound_stop_square1_sfx(smbc);
  }
}

/* smb.asm: ContinueSndJump */
static void continue_snd_jump(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t a = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
  if (a == 0x25u) {
    sound_dump_squ1_regs(smbc, 0x5fu, 0xf6u);
  } else if (a == 0x20u) {
    sound_dump_squ1_regs(smbc, 0x48u, 0xbcu); /* smb.asm: FPS2nd */
  }
  decrement_sfx1_length(smbc);
}

/* smb.asm: ContinueBumpThrow */
static void continue_bump_throw(Smbc *smbc) {
  CoreState *st = &smbc->state;
  if (st->ram[SMBC_SQU1_SFX_LEN_COUNTER] == 0x06u) {
    apu_write(&smbc->apu, APU_SQUARE1 + 1u, 0xbbu);
  }
  decrement_sfx1_length(smbc);
}

/* smb.asm: ContinueSwimStomp */
static void continue_swim_stomp(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t y = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
  apu_write(&smbc->apu, APU_SQUARE1, SMBC_SWIM_STOMP_ENVELOPE_DATA[y - 1u]);
  if (y == 0x06u) {
    apu_write(&smbc->apu, APU_SQUARE1 + 2u, 0x9eu);
  }
  decrement_sfx1_length(smbc);
}

/* smb.asm: ContinueSmackEnemy */
static void continue_smack_enemy(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t y = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
  uint8_t a = 0x90u;
  if (y == 0x08u) {
    apu_write(&smbc->apu, APU_SQUARE1 + 2u, 0xa0u);
    a = 0x9fu;
  }
  apu_write(&smbc->apu, APU_SQUARE1, a);
  decrement_sfx1_length(smbc);
}

/* smb.asm: ContinuePipeDownInj — regs written only when the length counter
 * has d0 and d1 clear and d3 set (the second lsr leaves d3 in bit 1 for the
 * and #%00000010 test). */
static void continue_pipe_down_inj(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t a = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
  if ((a & 0x0bu) == 0x08u) {
    sound_play_squ1_sfx(smbc, 0x44u, 0x9au, 0x91u);
  }
  decrement_sfx1_length(smbc);
}

/* smb.asm: Square1SfxHandler */
void sfx_square1_handler(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint8_t y = st->ram[SMBC_SQUARE1_SOUND_QUEUE];
  if (y != 0) {
    st->ram[SMBC_SQUARE1_SOUND_BUFFER] = y;
    if ((y & 0x80u) != 0) { /* smb.asm: PlaySmallJump */
      sound_play_squ1_sfx(smbc, 0x26u, 0x82u, 0xa7u);
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x28u;
      continue_snd_jump(smbc);
      return;
    }
    if ((y & SMBC_SFX_BIG_JUMP) != 0) { /* smb.asm: PlayBigJump */
      sound_play_squ1_sfx(smbc, 0x18u, 0x82u, 0xa7u);
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x28u;
      continue_snd_jump(smbc);
      return;
    }
    if ((y & SMBC_SFX_BUMP) != 0) { /* smb.asm: PlayBump */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x0au;
      sound_play_squ1_sfx(smbc, 0x0cu, 0x9eu, 0x93u);
      continue_bump_throw(smbc);
      return;
    }
    if ((y & SMBC_SFX_ENEMY_STOMP) != 0) { /* smb.asm: PlaySwimStomp */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x0eu;
      sound_play_squ1_sfx(smbc, 0x26u, 0x9eu, 0x9cu);
      continue_swim_stomp(smbc);
      return;
    }
    if ((y & SMBC_SFX_ENEMY_SMACK) != 0) { /* smb.asm: PlaySmackEnemy */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x0eu;
      sound_play_squ1_sfx(smbc, 0x28u, 0x9fu, 0xcbu);
      decrement_sfx1_length(smbc);
      return;
    }
    if ((y & SMBC_SFX_PIPE_DOWN_INJURY) != 0) { /* smb.asm: PlayPipeDownInj */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x2fu;
      continue_pipe_down_inj(smbc);
      return;
    }
    if ((y & SMBC_SFX_FIREBALL) != 0) { /* smb.asm: PlayFireballThrow */
      st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x05u;
      sound_play_squ1_sfx(smbc, 0x0cu, 0x9eu, 0x99u);
      continue_bump_throw(smbc);
      return;
    }
    /* smb.asm: PlayFlagpoleSlide */
    st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x40u;
    sound_set_freq_squ1(smbc, 0x62u);
    sound_dump_squ1_regs(smbc, 0x99u, 0xbcu);
    decrement_sfx1_length(smbc);
    return;
  }

  /* smb.asm: CheckSfx1Buffer */
  uint8_t a = st->ram[SMBC_SQUARE1_SOUND_BUFFER];
  if (a == 0) {
    return; /* smb.asm: ExS1H */
  }
  if ((a & 0x81u) != 0) { /* small or big jump */
    continue_snd_jump(smbc);
  } else if ((a & (SMBC_SFX_BUMP | SMBC_SFX_FIREBALL)) != 0) {
    continue_bump_throw(smbc);
  } else if ((a & SMBC_SFX_ENEMY_STOMP) != 0) {
    continue_swim_stomp(smbc);
  } else if ((a & SMBC_SFX_ENEMY_SMACK) != 0) {
    continue_smack_enemy(smbc);
  } else if ((a & SMBC_SFX_PIPE_DOWN_INJURY) != 0) {
    continue_pipe_down_inj(smbc);
  } else { /* flagpole slide */
    decrement_sfx1_length(smbc);
  }
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

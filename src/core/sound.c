/* core/sound.c — see core/sound.h for the contract.
 *
 * Owns the sound engine's frame entry point and its register/frequency
 * helpers, plus the pause sound effect. The per-channel sound effect and
 * music logic lives in core/sfx and core/music; this atom calls into them
 * through core/internal.h.
 */
#include "sound.h"

#include "../data/music.h"
#include "../hw/apu.h"
#include "frame.h"
#include "internal.h"
#include "state.h"

/* smb.asm: FreqRegLookupTbl is followed in ROM by MusicLengthLookupTbl, the
 * envelope tables, and the interrupt vectors at $FFFA, so the indexed reads
 * in Dump_Freq_Regs run on through all of them. */
static const uint8_t VECTOR_TBL[6] = {0x82u, 0x80u, 0x00u, 0x80u, 0xf0u, 0xffu};

uint8_t sound_freq_lookup(uint8_t index) {
  uint8_t i = index;
  if (i < SMBC_FREQ_REG_LOOKUP_TBL_SIZE) {
    return SMBC_FREQ_REG_LOOKUP_TBL[i];
  }
  i -= SMBC_FREQ_REG_LOOKUP_TBL_SIZE;
  if (i < SMBC_MUSIC_LENGTH_LOOKUP_TBL_SIZE) {
    return SMBC_MUSIC_LENGTH_LOOKUP_TBL[i];
  }
  i -= SMBC_MUSIC_LENGTH_LOOKUP_TBL_SIZE;
  if (i < SMBC_END_OF_CASTLE_MUSIC_ENV_DATA_SIZE) {
    return SMBC_END_OF_CASTLE_MUSIC_ENV_DATA[i];
  }
  i -= SMBC_END_OF_CASTLE_MUSIC_ENV_DATA_SIZE;
  if (i < SMBC_AREA_MUSIC_ENV_DATA_SIZE) {
    return SMBC_AREA_MUSIC_ENV_DATA[i];
  }
  i -= SMBC_AREA_MUSIC_ENV_DATA_SIZE;
  if (i < SMBC_WATER_EVENT_MUS_ENV_DATA_SIZE) {
    return SMBC_WATER_EVENT_MUS_ENV_DATA[i];
  }
  i -= SMBC_WATER_EVENT_MUS_ENV_DATA_SIZE;
  if (i < SMBC_BOWSER_FLAME_ENV_DATA_SIZE) {
    return SMBC_BOWSER_FLAME_ENV_DATA[i];
  }
  i -= SMBC_BOWSER_FLAME_ENV_DATA_SIZE;
  if (i < SMBC_BRICK_SHATTER_ENV_DATA_SIZE) {
    return SMBC_BRICK_SHATTER_ENV_DATA[i];
  }
  return VECTOR_TBL[i - SMBC_BRICK_SHATTER_ENV_DATA_SIZE];
}

/* smb.asm: Dump_Freq_Regs */
bool sound_dump_freq_regs(Smbc *smbc, uint8_t x, uint8_t a) {
  /* $FF+1 wraps the 16-bit indexed address to $0000, which is RAM */
  uint8_t lo = a == 0xffu ? smbc->state.ram[0] : sound_freq_lookup((uint8_t)(a + 1u));
  if (lo == 0) {
    return false;
  }
  uint8_t hi = sound_freq_lookup(a);
  apu_write(&smbc->apu, (uint16_t)(0x4002u + x), lo);
  apu_write(&smbc->apu, (uint16_t)(0x4003u + x), (uint8_t)(hi | 0x08u));
  return true;
}

uint32_t sound_freq_dump_cycles(uint8_t a, bool wrote) {
  return (wrote ? 30u : 15u) + (a == 0xffu ? 1u : 0u);
}

/* smb.asm: Dump_Squ1_Regs */
void sound_dump_squ1_regs(Smbc *smbc, uint8_t x, uint8_t y) {
  apu_write(&smbc->apu, 0x4001u, y);
  apu_write(&smbc->apu, 0x4000u, x);
}

/* smb.asm: Dump_Sq2_Regs */
void sound_dump_sq2_regs(Smbc *smbc, uint8_t x, uint8_t y) {
  apu_write(&smbc->apu, 0x4004u, x);
  apu_write(&smbc->apu, 0x4005u, y);
}

/* smb.asm: PlaySqu1Sfx (Dump_Squ1_Regs then SetFreq_Squ1, which forces x=0) */
bool sound_play_squ1_sfx(Smbc *smbc, uint8_t a, uint8_t x, uint8_t y) {
  sound_dump_squ1_regs(smbc, x, y);
  return sound_dump_freq_regs(smbc, 0x00u, a);
}

/* smb.asm: PlaySqu2Sfx (Dump_Sq2_Regs then SetFreq_Squ2, which forces x=4) */
bool sound_play_squ2_sfx(Smbc *smbc, uint8_t a, uint8_t x, uint8_t y) {
  sound_dump_sq2_regs(smbc, x, y);
  return sound_dump_freq_regs(smbc, 0x04u, a);
}

/* smb.asm: SetFreq_Squ1 */
bool sound_set_freq_squ1(Smbc *smbc, uint8_t a) {
  return sound_dump_freq_regs(smbc, 0x00u, a);
}

/* smb.asm: SetFreq_Squ2 */
bool sound_set_freq_squ2(Smbc *smbc, uint8_t a) {
  return sound_dump_freq_regs(smbc, 0x04u, a);
}

/* smb.asm: SetFreq_Tri */
bool sound_set_freq_tri(Smbc *smbc, uint8_t a) {
  return sound_dump_freq_regs(smbc, 0x08u, a);
}

/* smb.asm: StopSquare1Sfx */
void sound_stop_square1_sfx(Smbc *smbc) {
  smbc->state.ram[SMBC_SQUARE1_SOUND_BUFFER] = 0x00u;
  apu_write(&smbc->apu, 0x4015u, 0x0eu);
  apu_write(&smbc->apu, 0x4015u, 0x0fu);
}

/* smb.asm: StopSquare2Sfx */
void sound_stop_square2_sfx(Smbc *smbc) {
  apu_write(&smbc->apu, 0x4015u, 0x0du);
  apu_write(&smbc->apu, 0x4015u, 0x0fu);
}

/* smb.asm: PTRegC — ldx, ldy, then the jsr PlaySqu1Sfx with its cost */
static uint32_t play_pause_tone(Smbc *smbc, uint8_t a) {
  return 2u + 2u + sfx_play_squ1(smbc, a, 0x84u, 0x7fu);
}

/* smb.asm: InPause through DecPauC, without the SkipSoundSubroutines tail the
 * callers reach. Returns cycles for the pause path taken. */
static uint32_t pause_sound(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint32_t cycles = 4u; /* lda PauseSoundBuffer */

  if (st->ram[SMBC_PAUSE_SOUND_BUFFER] != 0) {
    cycles += 4u; /* bne ContPau, taken across a page */
    cycles += 4u; /* lda Squ1_SfxLenCounter */
    uint8_t a = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
    if (a == 0x24u) {
      cycles += 2u + 3u + 2u; /* cmp; beq taken; lda #$64 */
      cycles += play_pause_tone(smbc, 0x64u);
    } else if (a == 0x1eu) {
      cycles += 2u + 2u + 2u + 3u + 2u + 3u; /* cmp; beq; cmp; beq taken; lda #$44; bne PTRegC */
      cycles += play_pause_tone(smbc, 0x44u);
    } else if (a == 0x18u) {
      cycles += 2u + 2u + 2u + 2u + 2u + 2u + 2u; /* cmp; beq; cmp; beq; cmp; bne; lda #$64 */
      cycles += play_pause_tone(smbc, 0x64u);
    } else {
      cycles += 2u + 2u + 2u + 2u + 2u + 3u; /* cmp; beq; cmp; beq; cmp; bne DecPauC taken */
    }
  } else {
    cycles += 2u; /* bne ContPau, not taken */
    cycles += 3u; /* lda PauseSoundQueue */
    uint8_t queue = st->ram[SMBC_PAUSE_SOUND_QUEUE];
    if (queue == 0) {
      return cycles + 4u; /* beq SkipSoundSubroutines, taken across a page */
    }
    cycles += 2u; /* beq not taken */
    st->ram[SMBC_PAUSE_SOUND_BUFFER] = queue;
    st->ram[SMBC_PAUSE_MODE_FLAG] = queue;
    apu_write(&smbc->apu, 0x4015u, 0x00u);
    st->ram[SMBC_SQUARE1_SOUND_BUFFER] = 0x00u;
    st->ram[SMBC_SQUARE2_SOUND_BUFFER] = 0x00u;
    st->ram[SMBC_NOISE_SOUND_BUFFER] = 0x00u;
    apu_write(&smbc->apu, 0x4015u, 0x0fu);
    st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x2au;
    cycles += 4u + 4u + 2u + 4u + 3u + 3u + 3u + 2u + 4u + 2u + 4u; /* stores as in InPause */
    cycles += 2u + 3u; /* PTone1F: lda #$44; bne PTRegC */
    cycles += play_pause_tone(smbc, 0x44u);
  }

  cycles += 6u; /* dec Squ1_SfxLenCounter */
  if (--st->ram[SMBC_SQU1_SFX_LEN_COUNTER] != 0) {
    return cycles + 3u; /* bne SkipSoundSubroutines, taken */
  }
  apu_write(&smbc->apu, 0x4015u, 0x00u);
  cycles += 2u + 2u + 4u + 4u + 2u; /* bne SkipSoundSubroutines not taken; lda #0; sta SND_MASTERCTRL; lda PauseSoundBuffer; cmp #$02 */
  if (st->ram[SMBC_PAUSE_SOUND_BUFFER] == 0x02u) {
    st->ram[SMBC_PAUSE_MODE_FLAG] = 0x00u;
    cycles += 2u + 2u + 4u; /* bne SkipPIn not taken; lda #$00; sta PauseModeFlag */
  } else {
    cycles += 3u; /* bne SkipPIn, taken */
  }
  st->ram[SMBC_PAUSE_SOUND_BUFFER] = 0x00u;
  return cycles + 2u + 4u + 3u; /* SkipPIn: lda #$00; sta PauseSoundBuffer; beq SkipSoundSubroutines */
}

/* smb.asm: RunSoundSubroutines through the music queue clears */
static uint32_t run_sound_subroutines(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint32_t cycles = 6u * 4u; /* jsr to each handler, including MusicHandler */
  cycles += sfx_square1_handler(smbc);
  cycles += sfx_square2_handler(smbc);
  cycles += sfx_noise_handler(smbc);
  cycles += sound_music_handler(smbc);
  st->ram[SMBC_AREA_MUSIC_QUEUE] = 0x00u;
  st->ram[SMBC_EVENT_MUSIC_QUEUE] = 0x00u;
  return cycles + 2u + 3u + 3u;
}

/* smb.asm: SkipSoundSubroutines, including the rts. */
static uint32_t skip_sound_subroutines(Smbc *smbc) {
  CoreState *st = &smbc->state;
  st->ram[SMBC_SQUARE1_SOUND_QUEUE] = 0x00u;
  st->ram[SMBC_SQUARE2_SOUND_QUEUE] = 0x00u;
  st->ram[SMBC_NOISE_SOUND_QUEUE] = 0x00u;
  st->ram[SMBC_PAUSE_SOUND_QUEUE] = 0x00u;

  uint8_t y = st->ram[SMBC_DAC_COUNTER];
  uint32_t cycles = 2u + 3u * 4u + 4u + 3u + 2u; /* lda #0; 4 sta zp; ldy abs; lda zp; and */
  if ((st->ram[SMBC_AREA_MUSIC_BUFFER] & 0x03u) != 0) {
    cycles += 2u + 6u + 2u; /* beq not taken; inc abs; cpy */
    st->ram[SMBC_DAC_COUNTER]++;
    if (y < 0x30u) {
      cycles += 3u; /* bcc StrWave, taken */
    } else {
      cycles += 2u + 2u + 2u + 6u; /* bcc not taken; tya; beq not taken; dec abs */
      st->ram[SMBC_DAC_COUNTER]--;
    }
  } else if (y == 0) {
    cycles += 3u + 2u + 3u; /* beq taken; tya; beq taken */
  } else {
    cycles += 3u + 2u + 2u + 6u; /* beq taken; tya; beq not taken; dec abs */
    st->ram[SMBC_DAC_COUNTER]--;
  }
  apu_write(&smbc->apu, 0x4011u, y);
  return cycles + 4u + 6u; /* sty abs; rts */
}

/* smb.asm: SoundEngine. Returns the cycles of the path taken, excluding the
 * MusicHandler body, which is not yet charged. */
uint32_t sound_engine(Smbc *smbc) {
  CoreState *st = &smbc->state;

  uint8_t mode = st->ram[SMBC_OPER_MODE];
  if (mode == 0) { /* title screen: disable sound and leave */
    apu_write(&smbc->apu, 0x4015u, mode);
    return 16u;
  }
  apu_write(&smbc->apu, 0x4017u, 0xffu);
  apu_write(&smbc->apu, 0x4015u, 0x0fu);

  uint32_t cycles = 4u + 3u + 2u + 4u + 2u + 4u; /* lda OperMode; bne SndOn; ... sta */
  cycles += 4u; /* lda PauseModeFlag */
  if (st->ram[SMBC_PAUSE_MODE_FLAG] != 0) {
    cycles += 3u; /* bne InPause, taken */
    cycles += pause_sound(smbc);
  } else {
    cycles += 2u + 3u + 2u; /* bne not taken; lda PauseSoundQueue; cmp #$01 */
    if (st->ram[SMBC_PAUSE_SOUND_QUEUE] != 0x01u) {
      cycles += 4u; /* bne RunSoundSubroutines, taken across a page */
      cycles += run_sound_subroutines(smbc);
    } else {
      cycles += 2u; /* bne not taken, into InPause */
      cycles += pause_sound(smbc);
    }
  }

  return cycles + skip_sound_subroutines(smbc);
}

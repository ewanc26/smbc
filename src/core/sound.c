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

/* smb.asm: FreqRegLookupTbl immediately followed in ROM by
 * MusicLengthLookupTbl, so an index of 102..149 reads the length table. */
uint8_t sound_freq_lookup(uint8_t index) {
  if (index < SMBC_FREQ_REG_LOOKUP_TBL_SIZE) {
    return SMBC_FREQ_REG_LOOKUP_TBL[index];
  }
  if (index < SMBC_FREQ_REG_LOOKUP_TBL_SIZE + SMBC_MUSIC_LENGTH_LOOKUP_TBL_SIZE) {
    return SMBC_MUSIC_LENGTH_LOOKUP_TBL[index - SMBC_FREQ_REG_LOOKUP_TBL_SIZE];
  }
  return 0;
}

/* smb.asm: Dump_Freq_Regs */
bool sound_dump_freq_regs(Smbc *smbc, uint8_t x, uint8_t a) {
  uint8_t lo = sound_freq_lookup((uint8_t)(a + 1u));
  if (lo == 0) {
    return false;
  }
  uint8_t hi = sound_freq_lookup(a);
  apu_write(&smbc->apu, (uint16_t)(0x4002u + x), lo);
  apu_write(&smbc->apu, (uint16_t)(0x4003u + x), (uint8_t)(hi | 0x08u));
  return true;
}

/* smb.asm: Dump_Squ1_Regs */
void sound_dump_squ1_regs(Smbc *smbc, uint8_t x, uint8_t y) {
  apu_write(&smbc->apu, 0x4000u, x);
  apu_write(&smbc->apu, 0x4001u, y);
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

/* smb.asm: InPause through DecPauC — the pause sfx takes over square 1 and
 * the engine always lands on SkipSoundSubroutines afterwards. */
static void pause_sound(Smbc *smbc) {
  CoreState *st = &smbc->state;
  bool play = false;
  uint8_t a = 0x00u;

  if (st->ram[SMBC_PAUSE_SOUND_BUFFER] != 0) {
    a = st->ram[SMBC_SQU1_SFX_LEN_COUNTER];
    if (a == 0x24u || a == 0x18u) {
      a = 0x64u;
      play = true;
    } else if (a == 0x1eu) {
      a = 0x44u;
      play = true;
    }
  } else {
    if (st->ram[SMBC_PAUSE_SOUND_QUEUE] == 0) {
      return;
    }
    a = st->ram[SMBC_PAUSE_SOUND_QUEUE];
    st->ram[SMBC_PAUSE_SOUND_BUFFER] = a;
    st->ram[SMBC_PAUSE_MODE_FLAG] = a;
    apu_write(&smbc->apu, 0x4015u, 0x00u);
    st->ram[SMBC_SQUARE1_SOUND_BUFFER] = 0x00u;
    st->ram[SMBC_SQUARE2_SOUND_BUFFER] = 0x00u;
    st->ram[SMBC_NOISE_SOUND_BUFFER] = 0x00u;
    apu_write(&smbc->apu, 0x4015u, 0x0fu);
    st->ram[SMBC_SQU1_SFX_LEN_COUNTER] = 0x2au;
    a = 0x44u; /* PTone1F */
    play = true;
  }

  if (play) {
    sound_play_squ1_sfx(smbc, a, 0x84u, 0x7fu);
  }

  if (--st->ram[SMBC_SQU1_SFX_LEN_COUNTER] == 0) {
    apu_write(&smbc->apu, 0x4015u, 0x00u);
    if (st->ram[SMBC_PAUSE_SOUND_BUFFER] == 0x02u) {
      st->ram[SMBC_PAUSE_MODE_FLAG] = 0x00u;
    }
    st->ram[SMBC_PAUSE_SOUND_BUFFER] = 0x00u;
  }
}

/* smb.asm: SkipSoundSubroutines — clear the sfx queues and emit the DAC
 * sample level; the register always receives the previous counter value. */
static void skip_sound_subroutines(Smbc *smbc) {
  CoreState *st = &smbc->state;
  st->ram[SMBC_SQUARE1_SOUND_QUEUE] = 0x00u;
  st->ram[SMBC_SQUARE2_SOUND_QUEUE] = 0x00u;
  st->ram[SMBC_NOISE_SOUND_QUEUE] = 0x00u;
  st->ram[SMBC_PAUSE_SOUND_QUEUE] = 0x00u;

  uint8_t y = st->ram[SMBC_DAC_COUNTER];
  if ((st->ram[SMBC_AREA_MUSIC_BUFFER] & 0x03u) != 0) {
    st->ram[SMBC_DAC_COUNTER]++;
    if (y >= 0x30u && y != 0) {
      st->ram[SMBC_DAC_COUNTER]--;
    }
  } else if (y != 0) {
    st->ram[SMBC_DAC_COUNTER]--;
  }
  apu_write(&smbc->apu, 0x4011u, y);
}

/* smb.asm: SoundEngine. Returns the cycles of the square 1 path only; the
 * other channels, music, pause and skip paths are not yet charged. */
uint32_t sound_engine(Smbc *smbc) {
  CoreState *st = &smbc->state;

  uint8_t mode = st->ram[SMBC_OPER_MODE];
  if (mode == 0) { /* title screen: disable sound and leave */
    apu_write(&smbc->apu, 0x4015u, mode);
    return 0;
  }
  apu_write(&smbc->apu, 0x4017u, 0xffu);
  apu_write(&smbc->apu, 0x4015u, 0x0fu);

  uint32_t cycles = 0;
  if (st->ram[SMBC_PAUSE_MODE_FLAG] != 0 ||
      st->ram[SMBC_PAUSE_SOUND_QUEUE] == 0x01u) {
    pause_sound(smbc);
  } else {
    cycles = sfx_square1_handler(smbc);
    sfx_square2_handler(smbc);
    sfx_noise_handler(smbc);
    sound_music_handler(smbc);
    st->ram[SMBC_AREA_MUSIC_QUEUE] = 0x00u;
    st->ram[SMBC_EVENT_MUSIC_QUEUE] = 0x00u;
  }

  skip_sound_subroutines(smbc);
  return cycles;
}

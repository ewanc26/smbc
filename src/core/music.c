/* core/music.c — the music sequencer.
 *
 * Owns: smb.asm: MusicHandler and everything it calls that the sfx atoms do
 * not — header loading, the square 2 / square 1 / triangle / noise music
 * routines, length and envelope processing, and end-of-data handling.
 * Called once per frame from core/sound (RunSoundSubroutines). Each routine
 * accumulates the 6502 cycles of the path it takes, including its own rts;
 * the caller charges the jsr that reaches it.
 *
 * Song data lives in data/music as one ROM-order blob assembled at $f9b8;
 * the two-byte MusicData pointer in RAM indexes it directly. The header
 * blocks keep the disassembly's addressing: offsets are relative to the
 * start of the offset table, which the headers follow immediately.
 */
#include "frame.h"
#include "internal.h"
#include "state.h"
#include "../data/music.h"
#include "../hw/apu.h"

/* smb.asm: SND_MASTERCTRL_REG/SND_SQUARE1_REG/SND_SQUARE2_REG/
 * SND_TRIANGLE_REG/SND_NOISE_REG */
#define APU_MASTERCTRL 0x4015u
#define APU_SQUARE1    0x4000u
#define APU_SQUARE2    0x4004u
#define APU_TRIANGLE   0x4008u
#define APU_NOISE      0x400cu

/* smb.asm: ROM addresses of the tables read with indexed addressing, used for
 * the page-crossing cycle penalty */
#define ADDR_MUSIC_HEADER_OFFSET_DATA 0xf90cu
#define ADDR_MUSIC_HEADER_DATA        0xf90du
#define ADDR_MUSIC_LENGTH_LOOKUP_TBL  0xff66u
#define ADDR_END_OF_CASTLE_ENV        0xff96u
#define ADDR_AREA_ENV                 0xff9au
#define ADDR_WATER_EVENT_ENV          0xffa2u

#define JSR_CYCLES          6u
#define STOP_SQUARE1_CYCLES 23u /* smb.asm: StopSquare1Sfx, rts included */
#define STOP_SQUARE2_CYCLES 18u /* smb.asm: StopSquare2Sfx, rts included */

/* an indexed read costs one more cycle when the index carries past the base's low byte */
static uint32_t page_cross(uint16_t base, uint8_t index) {
  return ((base & 0xffu) + index) > 0xffu ? 1u : 0u;
}

/* a conditional branch: 2 cycles, 3 when taken (no page crossing) */
static uint32_t branch(bool taken) { return taken ? 3u : 2u; }

/* branches into HandleTriangleMusic ($f81a) from $f7be, $f7e4 and $f7f9 cross a page */
static uint32_t branch_to_triangle(bool taken) { return taken ? 4u : 2u; }

/* smb.asm: lda (MusicData),y — 5 cycles, one more across a page. The pointer in
 * $f5/$f6 addresses the song blob at its assembled base. */
static uint8_t music_data_read(const Smbc *smbc, uint8_t y, uint32_t *cyc) {
  const CoreState *st = &smbc->state;
  uint16_t ptr = (uint16_t)(st->ram[SMBC_MUSIC_DATA_LOW] |
                            ((uint16_t)st->ram[SMBC_MUSIC_DATA_HIGH] << 8));
  *cyc += 5u + page_cross(ptr, y);
  return SMBC_MUSIC_ROM[ptr - SMBC_MUSIC_ROM_BASE + y];
}

/* smb.asm: MusicHeaderData — the offset table and the headers form one
 * addressing region; offsets index from the region's start. */
static uint8_t mhd_read(uint8_t index) {
  if (index < SMBC_MUSIC_HEADER_OFFSET_DATA_SIZE) {
    return SMBC_MUSIC_HEADER_OFFSET_DATA[index];
  }
  return SMBC_MUSIC_HEADER_DATA[index - SMBC_MUSIC_HEADER_OFFSET_DATA_SIZE];
}

/* smb.asm: the envelope tables sit contiguously in ROM (EndOfCastle, Area,
 * WaterEvent, BowserFlame, BrickShatter) and reads spill across their
 * boundaries, so envelope lookup is over the combined region. */
static uint8_t env_read(uint8_t base, uint8_t y) {
  uint8_t i = (uint8_t)(base + y);
  if (i < SMBC_END_OF_CASTLE_MUSIC_ENV_DATA_SIZE) {
    return SMBC_END_OF_CASTLE_MUSIC_ENV_DATA[i];
  }
  i = (uint8_t)(i - SMBC_END_OF_CASTLE_MUSIC_ENV_DATA_SIZE);
  if (i < SMBC_AREA_MUSIC_ENV_DATA_SIZE) {
    return SMBC_AREA_MUSIC_ENV_DATA[i];
  }
  i = (uint8_t)(i - SMBC_AREA_MUSIC_ENV_DATA_SIZE);
  if (i < SMBC_WATER_EVENT_MUS_ENV_DATA_SIZE) {
    return SMBC_WATER_EVENT_MUS_ENV_DATA[i];
  }
  i = (uint8_t)(i - SMBC_WATER_EVENT_MUS_ENV_DATA_SIZE);
  if (i < SMBC_BOWSER_FLAME_ENV_DATA_SIZE) {
    return SMBC_BOWSER_FLAME_ENV_DATA[i];
  }
  i = (uint8_t)(i - SMBC_BOWSER_FLAME_ENV_DATA_SIZE);
  return SMBC_BRICK_SHATTER_ENV_DATA[i];
}

#define ENV_END_OF_CASTLE_BASE 0x00u
#define ENV_AREA_BASE          SMBC_END_OF_CASTLE_MUSIC_ENV_DATA_SIZE
#define ENV_WATER_EVENT_BASE   (ENV_AREA_BASE + SMBC_AREA_MUSIC_ENV_DATA_SIZE)

/* smb.asm: ProcessLengthData, rts included — the three low bits plus the
 * header's length offset (and the time-running-out adder) index
 * MusicLengthLookupTbl. The length-table index is handed back through idx. */
static uint8_t process_length_data(Smbc *smbc, uint8_t a, uint8_t *idx, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  uint8_t y = (uint8_t)((a & 0x07u) + st->ram[SMBC_NOTE_LEN_LOOKUP_TBL_OFS] +
                        st->ram[SMBC_NOTE_LENGTH_TBL_ADDER]);
  *cyc += 23u + page_cross(ADDR_MUSIC_LENGTH_LOOKUP_TBL, y);
  *idx = y;
  return SMBC_MUSIC_LENGTH_LOOKUP_TBL[y];
}

/* smb.asm: AlternateLengthHandler, which falls through into ProcessLengthData —
 * length bits arrive as d0,d7,d6; rotate them into d2,d1,d0. The original byte
 * and the length-table index are handed back through orig and idx (the asm
 * leaves them in X and Y — the silent beat writes both). */
static uint8_t alternate_length_handler(Smbc *smbc, uint8_t a, uint8_t *orig,
                                        uint8_t *idx, uint32_t *cyc) {
  *orig = a;
  *cyc += 12u;
  uint8_t length_bits = (uint8_t)(((a & 0x01u) << 2) | ((a & 0x80u) >> 6) |
                                  ((a & 0x40u) >> 6));
  return process_length_data(smbc, length_bits, idx, cyc);
}

/* smb.asm: LoadControlRegs, rts included — returns the envelope start in a and
 * the control reg contents in x/y (0x82/0x7f). */
static uint8_t load_control_regs(Smbc *smbc, uint8_t *x, uint8_t *y, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  uint8_t a;
  *cyc += 4u + 2u; /* lda EventMusicBuffer; and #EndOfCastleMusic */
  if ((st->ram[SMBC_EVENT_MUSIC_BUFFER] & SMBC_EVENT_MUSIC_END_OF_CASTLE) != 0) {
    *cyc += 2u + 2u + 3u; /* beq not taken; lda #$04; bne AllMus */
    a = 0x04u;
  } else {
    *cyc += 3u + 3u + 2u; /* beq NotECstlM taken; lda AreaMusicBuffer; and #$7d */
    if ((st->ram[SMBC_AREA_MUSIC_BUFFER] & 0x7du) != 0) {
      *cyc += 2u + 2u + 3u; /* beq not taken; lda #$08; bne AllMus */
      a = 0x08u;
    } else {
      *cyc += 3u + 2u; /* beq WaterMus taken; lda #$28 */
      a = 0x28u;
    }
  }
  *cyc += 2u + 2u + 6u; /* AllMus: ldx #$82; ldy #$7f; rts */
  *x = 0x82u;
  *y = 0x7fu;
  return a;
}

/* smb.asm: LoadEnvelopeData, rts included */
static uint8_t load_envelope_data(Smbc *smbc, uint8_t y, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  *cyc += 4u + 2u; /* lda EventMusicBuffer; and #EndOfCastleMusic */
  if ((st->ram[SMBC_EVENT_MUSIC_BUFFER] & SMBC_EVENT_MUSIC_END_OF_CASTLE) != 0) {
    *cyc += 2u + 4u + page_cross(ADDR_END_OF_CASTLE_ENV, y) + 6u;
    return env_read(ENV_END_OF_CASTLE_BASE, y);
  }
  *cyc += 3u; /* beq LoadUsualEnvData, taken */
  *cyc += 3u + 2u; /* lda AreaMusicBuffer; and #$7d */
  if ((st->ram[SMBC_AREA_MUSIC_BUFFER] & 0x7du) != 0) {
    *cyc += 2u + 4u + page_cross(ADDR_AREA_ENV, y) + 6u;
    return env_read(ENV_AREA_BASE, y);
  }
  *cyc += 3u + 4u + page_cross(ADDR_WATER_EVENT_ENV, y) + 6u; /* beq taken, then lda */
  return env_read(ENV_WATER_EVENT_BASE, y);
}

/* smb.asm: FindAreaMusicHeader/FindEventMusicHeader — count down the music
 * constant's set bit into a header index, starting from the given base. Each
 * pass is iny, lsr and bcc (6 cycles, the bcc adds 3 when it loops). */
static uint8_t find_music_header(uint8_t y, uint8_t a, uint32_t *cyc) {
  uint32_t passes = 0;
  uint8_t carry;
  do {
    y++;
    passes++;
    carry = (uint8_t)(a & 0x01u);
    a >>= 1;
  } while (carry == 0);
  *cyc += 7u * passes - 1u;
  return y;
}

/* smb.asm: LoadHeader, falling into HandleSquare2Music — Y selects the entry
 * through the offset table. */
static void load_header(Smbc *smbc, uint8_t y, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  *cyc += 4u + page_cross(ADDR_MUSIC_HEADER_OFFSET_DATA, y) + 2u; /* lda; tay */
  uint8_t ofs = mhd_read((uint8_t)(y - 1u)); /* MusicHeaderOffsetData = MHD-1 */

  *cyc += 4u + page_cross(ADDR_MUSIC_HEADER_DATA, ofs) + 3u;
  st->ram[SMBC_NOTE_LEN_LOOKUP_TBL_OFS] = mhd_read(ofs);
  *cyc += 4u + page_cross(ADDR_MUSIC_HEADER_DATA + 1u, ofs) + 3u;
  st->ram[SMBC_MUSIC_DATA_LOW] = mhd_read((uint8_t)(ofs + 1u));
  *cyc += 4u + page_cross(ADDR_MUSIC_HEADER_DATA + 2u, ofs) + 3u;
  st->ram[SMBC_MUSIC_DATA_HIGH] = mhd_read((uint8_t)(ofs + 2u));
  *cyc += 4u + page_cross(ADDR_MUSIC_HEADER_DATA + 3u, ofs) + 3u;
  st->ram[SMBC_MUSIC_OFFSET_TRIANGLE] = mhd_read((uint8_t)(ofs + 3u));
  *cyc += 4u + page_cross(ADDR_MUSIC_HEADER_DATA + 4u, ofs) + 3u;
  st->ram[SMBC_MUSIC_OFFSET_SQUARE1] = mhd_read((uint8_t)(ofs + 4u));
  *cyc += 4u + page_cross(ADDR_MUSIC_HEADER_DATA + 5u, ofs) + 4u + 4u;
  st->ram[SMBC_MUSIC_OFFSET_NOISE] = mhd_read((uint8_t)(ofs + 5u));
  st->ram[SMBC_NOISE_DATA_LOOPBACK_OFS] = st->ram[SMBC_MUSIC_OFFSET_NOISE];

  *cyc += 2u + 4u * 4u; /* lda #$01; four sta to the note counters */
  st->ram[SMBC_SQU2_NOTE_LEN_COUNTER] = 0x01u;
  st->ram[SMBC_SQU1_NOTE_LEN_COUNTER] = 0x01u;
  st->ram[SMBC_TRI_NOTE_LEN_COUNTER] = 0x01u;
  st->ram[SMBC_NOISE_BEAT_LEN_COUNTER] = 0x01u;

  *cyc += 2u + 3u + 4u; /* lda #$00; sta MusicOffset_Square2; sta AltRegContentFlag */
  st->ram[SMBC_MUSIC_OFFSET_SQUARE2] = 0x00u;
  st->ram[SMBC_ALT_REG_CONTENT_FLAG] = 0x00u;

  *cyc += 2u + 4u + 2u + 4u; /* lda #$0b; sta $4015; lda #$0f; sta $4015 */
  apu_write(&smbc->apu, APU_MASTERCTRL, 0x0bu);
  apu_write(&smbc->apu, APU_MASTERCTRL, 0x0fu);
}

/* smb.asm: HandleAreaMusicLoopB — (re)start area music; also the loopback path
 * when a song's data runs out. */
static void handle_area_music_loop_b(Smbc *smbc, uint8_t a, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  *cyc += 2u + 4u + 3u + 2u; /* ldy #$00; sty EventMusicBuffer; sta AreaMusicBuffer; cmp #$01 */
  st->ram[SMBC_EVENT_MUSIC_BUFFER] = 0x00u;
  st->ram[SMBC_AREA_MUSIC_BUFFER] = a;
  if (a != SMBC_MUSIC_GROUND) {
    *cyc += 3u + 2u + 3u; /* bne FindAreaMusicHeader, taken; ldy #$08; sty MusicOffset_Square2 */
    uint8_t y = find_music_header(0x08u, a, cyc);
    load_header(smbc, y, cyc);
    return;
  }
  /* ground level music walks its layout through GroundMusicHeaderOfs; when
   * the walk reaches $32 the offset is re-armed at $11 and the walk resumes
   * at $12, so the lead-in entry only plays once (smb.asm: GMLoopB) */
  *cyc += 2u; /* bne not taken */
  *cyc += 6u + 4u + 2u; /* inc GroundMusicHeaderOfs; ldy; cpy #$32 */
  uint8_t y = (uint8_t)(st->ram[SMBC_GROUND_MUSIC_HEADER_OFS] + 1u);
  if (y == 0x32u) {
    *cyc += 2u + 2u + 3u + 4u; /* bne LoadHeader, not taken; ldy #$11; bne GMLoopB; sty */
    st->ram[SMBC_GROUND_MUSIC_HEADER_OFS] = 0x11u;
    handle_area_music_loop_b(smbc, a, cyc);
    return;
  }
  *cyc += 3u; /* bne LoadHeader, taken */
  st->ram[SMBC_GROUND_MUSIC_HEADER_OFS] = y;
  load_header(smbc, y, cyc);
}

/* smb.asm: LoadAreaMusic */
static void load_area_music(Smbc *smbc, uint8_t a, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  *cyc += 2u; /* cmp #$04 */
  if (a == SMBC_MUSIC_UNDERGROUND) {
    *cyc += 2u + JSR_CYCLES + STOP_SQUARE1_CYCLES;
    sound_stop_square1_sfx(smbc);
  } else {
    *cyc += 3u;
  }
  st->ram[SMBC_GROUND_MUSIC_HEADER_OFS] = 0x10u;
  *cyc += 2u + 4u; /* ldy #$10; sty GroundMusicHeaderOfs */
  handle_area_music_loop_b(smbc, a, cyc);
}

/* smb.asm: LoadEventMusic */
static void load_event_music(Smbc *smbc, uint8_t a, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  st->ram[SMBC_EVENT_MUSIC_BUFFER] = a;
  *cyc += 4u + 2u; /* sta EventMusicBuffer; cmp #DeathMusic */
  if (a == SMBC_EVENT_MUSIC_DEATH) {
    *cyc += 2u + JSR_CYCLES + STOP_SQUARE1_CYCLES + JSR_CYCLES + STOP_SQUARE2_CYCLES;
    sound_stop_square1_sfx(smbc);
    sound_stop_square2_sfx(smbc);
  } else {
    *cyc += 3u; /* bne NoStopSfx, taken */
  }
  st->ram[SMBC_AREA_MUSIC_BUFFER_ALT] = st->ram[SMBC_AREA_MUSIC_BUFFER];
  st->ram[SMBC_NOTE_LENGTH_TBL_ADDER] = 0x00u;
  st->ram[SMBC_AREA_MUSIC_BUFFER] = 0x00u;
  *cyc += 3u + 4u + 2u + 4u + 3u; /* ldx; stx Alt; ldy #0; sty adder; sty AreaMusicBuffer */

  *cyc += 2u; /* cmp #TimeRunningOutMusic */
  if (a == SMBC_EVENT_MUSIC_TIME_RUNNING_OUT) {
    st->ram[SMBC_NOTE_LENGTH_TBL_ADDER] = 0x08u;
    *cyc += 2u + 2u + 4u + 3u; /* bne not taken; ldx #$08; stx adder; bne FindEventMusicHeader */
  } else {
    *cyc += 3u; /* bne FindEventMusicHeader, taken */
  }
  uint8_t y = find_music_header(0x00u, a, cyc);
  load_header(smbc, y, cyc);
}

/* smb.asm: EndOfMusicData — returns whether a new song was loaded (the
 * loopback paths fall from LoadHeader into HandleSquare2Music) or the engine
 * was silenced, in which case its rts is charged here. */
static bool end_of_music_data(Smbc *smbc, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  *cyc += 4u + 2u; /* lda EventMusicBuffer; cmp #TimeRunningOutMusic */
  uint8_t a = st->ram[SMBC_EVENT_MUSIC_BUFFER];
  if (a == SMBC_EVENT_MUSIC_TIME_RUNNING_OUT) {
    *cyc += 2u + 4u; /* bne NotTRO, not taken; lda AreaMusicBuffer_Alt */
    a = st->ram[SMBC_AREA_MUSIC_BUFFER_ALT];
    if (a != 0) {
      *cyc += 3u + 3u; /* bne MusicLoopBack, taken; jmp HandleAreaMusicLoopB */
      handle_area_music_loop_b(smbc, a, cyc);
      return true;
    }
    *cyc += 2u; /* bne MusicLoopBack, not taken; the alt was empty */
    a = 0x00u;
  } else {
    *cyc += 3u; /* bne NotTRO, taken */
  }

  /* smb.asm: NotTRO */
  *cyc += 2u; /* and #VictoryMusic */
  a = (uint8_t)(a & SMBC_EVENT_MUSIC_VICTORY);
  if (a != 0) { /* smb.asm: VictoryMLoopBack */
    *cyc += 3u + 3u; /* bne taken; jmp LoadEventMusic */
    load_event_music(smbc, a, cyc);
    return true;
  }
  *cyc += 2u; /* bne not taken */

  *cyc += 3u + 2u; /* lda AreaMusicBuffer; and #%01011111 */
  a = (uint8_t)(st->ram[SMBC_AREA_MUSIC_BUFFER] & 0x5fu);
  if (a != 0) { /* smb.asm: MusicLoopBack — any area music but pipe intro */
    *cyc += 3u + 3u; /* bne taken; jmp HandleAreaMusicLoopB */
    handle_area_music_loop_b(smbc, a, cyc);
    return true;
  }
  *cyc += 2u; /* bne not taken */

  st->ram[SMBC_AREA_MUSIC_BUFFER] = 0x00u;
  st->ram[SMBC_EVENT_MUSIC_BUFFER] = 0x00u;
  apu_write(&smbc->apu, APU_TRIANGLE, 0x00u);
  apu_write(&smbc->apu, APU_SQUARE1, 0x90u);
  apu_write(&smbc->apu, APU_SQUARE2, 0x90u);
  *cyc += 2u + 3u + 4u + 4u + 2u + 4u + 4u + 6u; /* silencing stores; rts */
  return false;
}

/* smb.asm: ExitMusicHandler reached through HandleNoiseMusic — the noise beat,
 * including its rts */
static void handle_noise_music(Smbc *smbc, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  *cyc += 3u + 2u; /* lda AreaMusicBuffer; and #%11110011 */
  if ((st->ram[SMBC_AREA_MUSIC_BUFFER] & 0xf3u) == 0) {
    *cyc += 3u + 6u; /* beq ExitMusicHandler, taken; rts */
    return;
  }
  *cyc += 2u;
  *cyc += 6u; /* dec Noise_BeatLenCounter */
  if (--st->ram[SMBC_NOISE_BEAT_LEN_COUNTER] != 0) {
    *cyc += 3u + 6u; /* bne ExitMusicHandler, taken; rts */
    return;
  }
  *cyc += 2u;

  uint8_t a;
  for (;;) { /* smb.asm: FetchNoiseBeatData */
    *cyc += 4u + 6u; /* ldy MusicOffset_Noise; inc MusicOffset_Noise */
    uint8_t y = st->ram[SMBC_MUSIC_OFFSET_NOISE]++;
    a = music_data_read(smbc, y, cyc);
    if (a != 0) {
      *cyc += 3u; /* bne NoiseBeatHandler, taken */
      break;
    }
    *cyc += 2u + 4u + 4u + 3u; /* bne not taken; lda loopback; sta; bne FetchNoiseBeatData */
    st->ram[SMBC_MUSIC_OFFSET_NOISE] = st->ram[SMBC_NOISE_DATA_LOOPBACK_OFS];
  }

  /* smb.asm: NoiseBeatHandler */
  uint8_t beat, idx;
  *cyc += JSR_CYCLES;
  st->ram[SMBC_NOISE_BEAT_LEN_COUNTER] = alternate_length_handler(smbc, a, &beat, &idx, cyc);
  *cyc += 4u + 2u + 2u; /* sta Noise_BeatLenCounter; txa; and #%00111110 */
  a = (uint8_t)(beat & 0x3eu);
  uint8_t x, y;
  if (a == 0) { /* smb.asm: SilentBeat — X/Y keep the beat byte and length index */
    *cyc += 3u + 2u;
    a = 0x10u;
    x = beat;
    y = idx;
  } else if (a == 0x30u) { /* long beat */
    *cyc += 2u + 2u + 3u + 2u + 2u + 2u + 3u;
    a = 0x1cu; x = 0x03u; y = 0x58u;
  } else if (a == 0x20u) { /* strong beat */
    *cyc += 2u + 2u + 2u + 2u + 3u + 2u + 2u + 2u + 3u;
    a = 0x1cu; x = 0x0cu; y = 0x18u;
  } else if ((a & 0x10u) != 0) { /* short beat */
    *cyc += 2u * 10u + 3u;
    a = 0x1cu; x = 0x03u; y = 0x18u;
  } else { /* smb.asm: SilentBeat */
    *cyc += 2u * 6u + 3u + 2u;
    a = 0x10u; x = beat; y = idx;
  }
  /* smb.asm: PlayBeat, then ExitMusicHandler */
  apu_write(&smbc->apu, APU_NOISE, a);
  apu_write(&smbc->apu, APU_NOISE + 2u, x);
  apu_write(&smbc->apu, APU_NOISE + 3u, y);
  *cyc += 4u + 4u + 4u + 6u;
}

/* smb.asm: TriNoteHandler — a note byte already fetched; LoadTriCtrlReg writes
 * the control reg, then HandleNoiseMusic runs */
static void triangle_note(Smbc *smbc, uint8_t a, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  bool wrote = sound_set_freq_tri(smbc, a);
  *cyc += JSR_CYCLES + 2u + 3u + sound_freq_dump_cycles(a, wrote); /* jsr SetFreq_Tri: ldx; bne Dump_Freq_Regs */
  uint8_t x = st->ram[SMBC_TRI_NOTE_LEN_BUFFER];
  st->ram[SMBC_TRI_NOTE_LEN_COUNTER] = x;
  *cyc += 4u + 4u + 4u + 2u; /* ldx; stx Tri_NoteLenCounter; lda EventMusicBuffer; and #%01101110 */

  bool secondary = (st->ram[SMBC_EVENT_MUSIC_BUFFER] & 0x6eu) != 0;
  *cyc += branch(secondary);
  if (!secondary) {
    *cyc += 3u + 2u; /* lda AreaMusicBuffer; and #%00001010 */
    if ((st->ram[SMBC_AREA_MUSIC_BUFFER] & 0x0au) == 0) {
      *cyc += 3u; /* beq HandleNoiseMusic, taken */
      handle_noise_music(smbc, cyc);
      return;
    }
    *cyc += 2u; /* beq not taken */
  }

  /* smb.asm: NotDOrD4 */
  *cyc += 2u + 2u; /* txa; cmp #$12 */
  uint8_t value;
  if (x >= 0x12u) {
    *cyc += 3u + 2u; /* bcs LongN, taken; lda #$ff */
    value = 0xffu;
  } else {
    *cyc += 2u; /* bcs not taken */
    *cyc += 4u + 2u; /* lda EventMusicBuffer; and #EndOfCastleMusic */
    if ((st->ram[SMBC_EVENT_MUSIC_BUFFER] & SMBC_EVENT_MUSIC_END_OF_CASTLE) != 0) {
      *cyc += 2u + 2u + 3u; /* beq MediN not taken; lda #$0f; bne LoadTriCtrlReg */
      value = 0x0fu;
    } else {
      *cyc += 3u + 2u + 3u; /* beq MediN taken; lda #$1f; bne LoadTriCtrlReg */
      value = 0x1fu;
    }
  }
  apu_write(&smbc->apu, APU_TRIANGLE, value);
  *cyc += 4u; /* sta SND_TRIANGLE_REG */
  handle_noise_music(smbc, cyc);
}

/* smb.asm: HandleTriangleMusic */
static void handle_triangle_music(Smbc *smbc, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  *cyc += 3u + 6u; /* lda MusicOffset_Triangle; dec Tri_NoteLenCounter */
  if (--st->ram[SMBC_TRI_NOTE_LEN_COUNTER] != 0) {
    *cyc += 3u; /* bne HandleNoiseMusic, taken */
    handle_noise_music(smbc, cyc);
    return;
  }
  *cyc += 2u;
  uint8_t y = st->ram[SMBC_MUSIC_OFFSET_TRIANGLE]++;
  *cyc += 3u + 5u; /* ldy; inc MusicOffset_Triangle */
  uint8_t a = music_data_read(smbc, y, cyc);
  if (a == 0) {
    *cyc += 3u + 4u; /* beq LoadTriCtrlReg, taken; sta */
    apu_write(&smbc->apu, APU_TRIANGLE, 0x00u);
    handle_noise_music(smbc, cyc);
    return;
  }
  *cyc += 2u; /* beq not taken */
  if ((a & 0x80u) == 0) {
    *cyc += 3u; /* bpl TriNoteHandler, taken */
    triangle_note(smbc, a, cyc);
    return;
  }
  *cyc += 2u; /* bpl not taken: length data */
  uint8_t idx;
  *cyc += JSR_CYCLES;
  st->ram[SMBC_TRI_NOTE_LEN_BUFFER] = process_length_data(smbc, a, &idx, cyc);
  *cyc += 4u + 2u + 4u; /* sta Tri_NoteLenBuffer; lda #$1f; sta SND_TRIANGLE_REG */
  apu_write(&smbc->apu, APU_TRIANGLE, 0x1fu);
  y = st->ram[SMBC_MUSIC_OFFSET_TRIANGLE]++;
  *cyc += 3u + 5u; /* ldy; inc */
  a = music_data_read(smbc, y, cyc);
  if (a == 0) {
    *cyc += 3u + 4u; /* beq LoadTriCtrlReg, taken; sta */
    apu_write(&smbc->apu, APU_TRIANGLE, 0x00u);
    handle_noise_music(smbc, cyc);
    return;
  }
  *cyc += 2u; /* beq not taken */
  triangle_note(smbc, a, cyc);
}

/* smb.asm: MiscSqu1MusicTasks — the envelope and alternate control reg, then
 * HandleTriangleMusic */
static void misc_squ1_music_tasks(Smbc *smbc, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  *cyc += 3u; /* lda Square1SoundBuffer */
  if (st->ram[SMBC_SQUARE1_SOUND_BUFFER] != 0) {
    *cyc += branch_to_triangle(true);
    handle_triangle_music(smbc, cyc);
    return;
  }
  *cyc += branch_to_triangle(false);

  *cyc += 4u + 2u; /* lda EventMusicBuffer; and #%10010001 */
  if ((st->ram[SMBC_EVENT_MUSIC_BUFFER] & 0x91u) != 0) {
    *cyc += 3u; /* bne DeathMAltReg, taken */
  } else {
    *cyc += 2u; /* bne not taken */
    uint8_t y = st->ram[SMBC_SQU1_ENVELOPE_DATA_CTRL];
    *cyc += 4u; /* ldy Squ1_EnvelopeDataCtrl */
    if (y == 0) {
      *cyc += 3u; /* beq NoDecEnv2, taken */
    } else {
      *cyc += 2u + 6u; /* beq not taken; dec */
      st->ram[SMBC_SQU1_ENVELOPE_DATA_CTRL]--;
    }
    *cyc += JSR_CYCLES;
    apu_write(&smbc->apu, APU_SQUARE1, load_envelope_data(smbc, y, cyc));
    *cyc += 4u; /* sta SND_SQUARE1_REG */
  }

  /* smb.asm: DeathMAltReg */
  *cyc += 4u; /* lda AltRegContentFlag */
  uint8_t a = st->ram[SMBC_ALT_REG_CONTENT_FLAG];
  if (a != 0) {
    *cyc += 3u; /* bne DoAltLoad, taken */
  } else {
    *cyc += 2u + 2u; /* bne not taken; lda #$7f */
    a = 0x7fu;
  }
  *cyc += 4u; /* sta SND_SQUARE1_REG+1 */
  apu_write(&smbc->apu, APU_SQUARE1 + 1u, a);
  handle_triangle_music(smbc, cyc);
}

/* smb.asm: HandleSquare1Music */
static void handle_square1_music(Smbc *smbc, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  *cyc += 3u; /* ldy MusicOffset_Square1 */
  if (st->ram[SMBC_MUSIC_OFFSET_SQUARE1] == 0) {
    *cyc += branch_to_triangle(true);
    handle_triangle_music(smbc, cyc);
    return;
  }
  *cyc += branch_to_triangle(false);

  *cyc += 6u; /* dec Squ1_NoteLenCounter */
  if (--st->ram[SMBC_SQU1_NOTE_LEN_COUNTER] != 0) {
    *cyc += 3u; /* bne MiscSqu1MusicTasks, taken */
    misc_squ1_music_tasks(smbc, cyc);
    return;
  }
  *cyc += 2u;

  uint8_t a;
  for (;;) { /* smb.asm: FetchSqu1MusicData */
    *cyc += 3u + 5u; /* ldy MusicOffset_Square1; inc */
    uint8_t y = st->ram[SMBC_MUSIC_OFFSET_SQUARE1]++;
    a = music_data_read(smbc, y, cyc);
    if (a != 0) {
      *cyc += 3u; /* bne Squ1NoteHandler, taken */
      break;
    }
    *cyc += 2u + 2u + 4u + 2u + 4u + 4u + 3u; /* bne not taken; lda; sta; lda; sta; sta; bne */
    apu_write(&smbc->apu, APU_SQUARE1, 0x83u);
    apu_write(&smbc->apu, APU_SQUARE1 + 1u, 0x94u);
    st->ram[SMBC_ALT_REG_CONTENT_FLAG] = 0x94u;
  }

  /* smb.asm: Squ1NoteHandler */
  uint8_t note, idx;
  *cyc += JSR_CYCLES;
  st->ram[SMBC_SQU1_NOTE_LEN_COUNTER] = alternate_length_handler(smbc, a, &note, &idx, cyc);
  *cyc += 4u + 3u; /* sta Squ1_NoteLenCounter; ldy Square1SoundBuffer */
  if (st->ram[SMBC_SQUARE1_SOUND_BUFFER] != 0) {
    *cyc += branch_to_triangle(true);
    handle_triangle_music(smbc, cyc);
    return;
  }
  *cyc += branch_to_triangle(false);

  a = (uint8_t)(note & 0x3eu);
  *cyc += 2u + 2u; /* txa; and #%00111110 */
  uint8_t x = 0x00u, yregs = 0x00u;
  bool wrote = sound_set_freq_squ1(smbc, a);
  *cyc += JSR_CYCLES + 2u + sound_freq_dump_cycles(a, wrote); /* jsr SetFreq_Squ1: ldx #0; Dump_Freq_Regs */
  if (wrote) {
    *cyc += 2u + JSR_CYCLES; /* beq SkipCtrlL, not taken; jsr LoadControlRegs */
    a = load_control_regs(smbc, &x, &yregs, cyc);
  } else {
    *cyc += 3u; /* beq SkipCtrlL, taken */
    a = 0x00u;
    x = 0x00u;
    yregs = (uint8_t)(note & 0x3eu); /* SetFreq leaves y=note */
  }
  st->ram[SMBC_SQU1_ENVELOPE_DATA_CTRL] = a;
  *cyc += 4u + JSR_CYCLES + 14u; /* sta Squ1_EnvelopeDataCtrl; jsr Dump_Squ1_Regs */
  sound_dump_squ1_regs(smbc, x, yregs);

  misc_squ1_music_tasks(smbc, cyc);
}

/* smb.asm: HandleSquare2Music — the square 2 note walk, then MiscSqu2MusicTasks
 * and into HandleSquare1Music. The song-end path can loop once a new song
 * loads, so the note counter test runs again. */
static void handle_square2_music(Smbc *smbc, uint32_t *cyc) {
  CoreState *st = &smbc->state;
  for (;;) {
    *cyc += 6u; /* dec Squ2_NoteLenCounter */
    if (--st->ram[SMBC_SQU2_NOTE_LEN_COUNTER] != 0) {
      *cyc += 3u; /* bne MiscSqu2MusicTasks, taken */
      break;
    }
    *cyc += 2u;
    *cyc += 3u + 5u; /* ldy MusicOffset_Square2; inc */
    uint8_t y = st->ram[SMBC_MUSIC_OFFSET_SQUARE2]++;
    uint8_t a = music_data_read(smbc, y, cyc);
    if (a == 0) {
      *cyc += 3u; /* beq EndOfMusicData, taken */
      if (!end_of_music_data(smbc, cyc)) {
        return;
      }
      continue; /* a new song was loaded; its header run starts here */
    }
    *cyc += 2u; /* beq not taken */
    if ((a & 0x80u) != 0) { /* smb.asm: Squ2LengthHandler */
      *cyc += 2u + 3u; /* bpl not taken; bne Squ2LengthHandler, taken */
      uint8_t idx;
      *cyc += JSR_CYCLES;
      st->ram[SMBC_SQU2_NOTE_LEN_BUFFER] = process_length_data(smbc, a, &idx, cyc);
      *cyc += 4u + 3u + 5u; /* sta Squ2_NoteLenBuffer; ldy; inc */
      y = st->ram[SMBC_MUSIC_OFFSET_SQUARE2]++;
      a = music_data_read(smbc, y, cyc);
    } else {
      *cyc += 3u; /* bpl Squ2NoteHandler, taken */
    }

    /* smb.asm: Squ2NoteHandler */
    *cyc += 3u; /* ldx Square2SoundBuffer */
    if (st->ram[SMBC_SQUARE2_SOUND_BUFFER] == 0) {
      *cyc += 2u; /* bne SkipFqL1, not taken */
      uint8_t x = 0x04u, yregs = a; /* rest: SetFreq leaves x=4, y=note */
      bool wrote = sound_set_freq_squ2(smbc, a);
      *cyc += JSR_CYCLES + 2u + 3u + sound_freq_dump_cycles(a, wrote); /* jsr SetFreq_Squ2 */
      if (wrote) {
        *cyc += 2u + JSR_CYCLES; /* beq Rest, not taken; jsr LoadControlRegs */
        a = load_control_regs(smbc, &x, &yregs, cyc);
      } else {
        *cyc += 3u; /* beq Rest, taken */
        a = 0x00u;
      }
      st->ram[SMBC_SQU2_ENVELOPE_DATA_CTRL] = a;
      *cyc += 4u + JSR_CYCLES + 14u; /* sta Squ2_EnvelopeDataCtrl; jsr Dump_Sq2_Regs */
      sound_dump_sq2_regs(smbc, x, yregs);
    } else {
      *cyc += 3u; /* bne SkipFqL1, taken */
    }
    st->ram[SMBC_SQU2_NOTE_LEN_COUNTER] = st->ram[SMBC_SQU2_NOTE_LEN_BUFFER];
    *cyc += 4u + 4u; /* SkipFqL1: lda Squ2_NoteLenBuffer; sta Squ2_NoteLenCounter */
    break;
  }

  /* smb.asm: MiscSqu2MusicTasks */
  *cyc += 3u; /* lda Square2SoundBuffer */
  if (st->ram[SMBC_SQUARE2_SOUND_BUFFER] != 0) {
    *cyc += 3u;
    handle_square1_music(smbc, cyc);
    return;
  }
  *cyc += 2u;
  *cyc += 4u + 2u; /* lda EventMusicBuffer; and #%10010001 */
  if ((st->ram[SMBC_EVENT_MUSIC_BUFFER] & 0x91u) != 0) {
    *cyc += 3u;
    handle_square1_music(smbc, cyc);
    return;
  }
  *cyc += 2u;

  uint8_t y = st->ram[SMBC_SQU2_ENVELOPE_DATA_CTRL];
  *cyc += 4u; /* ldy Squ2_EnvelopeDataCtrl */
  if (y == 0) {
    *cyc += 3u; /* beq NoDecEnv1, taken */
  } else {
    *cyc += 2u + 6u; /* beq not taken; dec */
    st->ram[SMBC_SQU2_ENVELOPE_DATA_CTRL]--;
  }
  *cyc += JSR_CYCLES;
  apu_write(&smbc->apu, APU_SQUARE2, load_envelope_data(smbc, y, cyc));
  *cyc += 4u + 2u + 4u; /* sta SND_SQUARE2_REG; ldx #$7f; stx SND_SQUARE2_REG+1 */
  apu_write(&smbc->apu, APU_SQUARE2 + 1u, 0x7fu);
  handle_square1_music(smbc, cyc);
}

/* smb.asm: MusicHandler — returns the cycles of the path taken, including its
 * rts. The caller charges the jsr that reaches it. */
uint32_t sound_music_handler(Smbc *smbc) {
  CoreState *st = &smbc->state;
  uint32_t cycles = 3u; /* lda EventMusicQueue */
  uint8_t a = st->ram[SMBC_EVENT_MUSIC_QUEUE];
  if (a != 0) {
    cycles += 3u; /* bne LoadEventMusic, taken */
    load_event_music(smbc, a, &cycles);
  } else {
    cycles += 2u + 3u; /* bne not taken; lda AreaMusicQueue */
    a = st->ram[SMBC_AREA_MUSIC_QUEUE];
    if (a != 0) {
      cycles += 3u; /* bne LoadAreaMusic, taken */
      load_area_music(smbc, a, &cycles);
    } else {
      cycles += 2u + 4u + 3u; /* bne not taken; lda EventMusicBuffer; ora AreaMusicBuffer */
      if ((st->ram[SMBC_EVENT_MUSIC_BUFFER] | st->ram[SMBC_AREA_MUSIC_BUFFER]) == 0) {
        return cycles + 2u + 6u; /* bne ContinueMusic, not taken; rts: no music, then leave */
      }
      cycles += 3u + 3u; /* bne ContinueMusic, taken; jmp HandleSquare2Music */
    }
  }
  handle_square2_music(smbc, &cycles);
  return cycles;
}

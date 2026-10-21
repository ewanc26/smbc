/* core/screen.c — see core/screen.h for the contract. */
#include "screen.h"
#include "playergfx.h"
#include "enemies.h"

#include "parser.h"
#include "../hw/ppu.h"
#include "../data/gametext.h"
#include "../data/messages.h"
#include "../data/palettes.h"
#include "../data/screen.h"
#include "../data/statusbar.h"
#include "frame.h"
#include "memory.h"
#include "sprites.h"
#include "title.h"

#include <stddef.h>

void write_ppu_reg1(Smbc *smbc, uint8_t value) {
  /* smb.asm: WritePPUReg1 */
  ppu_write(&smbc->ppu, 0, value);
  smbc->state.ram[SMBC_MIRROR_PPU_CTRL1] = value;
}

/* smb.asm: OutputNumbers — append one status bar set to VRAM buffer 1.
 * The selector runs 1-6 after incrementing; six or more prints nothing.
 * Faithful to the original: no bounds check on the buffer. */
static void output_numbers(CoreState *state, uint8_t selector) {
  selector = (uint8_t)((selector + 1u) & 0x0fu);
  if (selector >= 6u) return; /* smb.asm: ExitOutputN */
  uint8_t y = (uint8_t)(selector * 2u);
  uint8_t x = state->ram[SMBC_VRAM_BUFFER1_OFFSET];
  uint16_t base = (uint16_t)(SMBC_VRAM_BUFFER1 + x);
  /* smb.asm: top score display goes to $22f0, everything else to $20xx */
  state->ram[base] = (y == 0u) ? 0x22u : 0x20u;
  state->ram[base + 1] = SMBC_STATUS_BAR_DATA[y];
  state->ram[base + 2] = SMBC_STATUS_BAR_DATA[y + 1];
  uint8_t len = state->ram[base + 2];
  uint8_t off = (uint8_t)(SMBC_STATUS_BAR_OFFSET[selector] -
                          SMBC_STATUS_BAR_DATA[y + 1]);
  for (uint8_t i = 0; i < len; i++) { /* smb.asm: DigitPLoop */
    state->ram[base + 3 + i] = state->ram[SMBC_DISPLAY_DIGITS + off + i];
  }
  state->ram[base + 3 + len] = 0x00u; /* smb.asm: zero terminator */
  state->ram[SMBC_VRAM_BUFFER1_OFFSET] = (uint8_t)(x + 3u + len);
}

void print_status_bar_numbers(CoreState *state, uint8_t nybbles) {
  /* smb.asm: PrintStatusBarNumbers */
  state->ram[SMBC_TEMP_VRAM_PTR] = nybbles; /* smb.asm: sta $00 */
  output_numbers(state, nybbles);           /* smb.asm: coin display */
  output_numbers(state, (uint8_t)(nybbles >> 4)); /* smb.asm: score */
}

void digits_math_routine(CoreState *state, uint8_t y) {
  /* smb.asm: DigitsMathRoutine */
  if (state->ram[SMBC_OPER_MODE] != SMBC_MODE_TITLE) {
    for (int x = 5; x >= 0; x--) {           /* smb.asm: AddModLoop */
      uint8_t v = (uint8_t)(state->ram[SMBC_DIGIT_MODIFIER + x] +
                            state->ram[SMBC_DISPLAY_DIGITS + y]);
      if (v & 0x80u) { /* smb.asm: BorrowOne — negative means borrow */
        state->ram[SMBC_DIGIT_MODIFIER + x - 1]--;
        v = 0x09u;
      } else if (v >= 10u) { /* smb.asm: CarryOne */
        v = (uint8_t)(v - 10u);
        state->ram[SMBC_DIGIT_MODIFIER + x - 1]++;
      }
      state->ram[SMBC_DISPLAY_DIGITS + y] = v; /* smb.asm: StoreNewD */
      y--;
    }
  }
  for (size_t x = 0; x < 6u; x++) { /* smb.asm: EraseMLoop */
    state->ram[SMBC_DIGIT_MODIFIER + x] = 0x00u;
  }
}

void init_scroll(Smbc *smbc, uint8_t value) {
  /* smb.asm: InitScroll */
  ppu_write(&smbc->ppu, 5, value);
  ppu_write(&smbc->ppu, 5, value);
}

/* smb.asm: VRAM_AddrTable_Low / VRAM_AddrTable_High — the pointers the NMI
 * loads into the $00/$01 indirect. RAM entries are the two VRAM buffers.
 * The ROM entries point at palette and message data (data/palettes,
 * data/messages); the host cannot express those as 6502 address halves, so
 * the low/high bytes stay zero here and the actual data is reached through
 * VRAM_ADDR_ROM_DATA below. */
#define VRAM_ADDR_TABLE_SIZE 19u
static const uint8_t VRAM_ADDR_TABLE_LOW[VRAM_ADDR_TABLE_SIZE] = {
    0x01u, /*  0: VRAM_Buffer1 */
    0x00u, /*  1: WaterPaletteData (ROM) */
    0x00u, /*  2: GroundPaletteData (ROM) */
    0x00u, /*  3: UndergroundPaletteData (ROM) */
    0x00u, /*  4: CastlePaletteData (ROM) */
    0x00u, /*  5: VRAM_Buffer1_Offset */
    0x41u, /*  6: VRAM_Buffer2 */
    0x41u, /*  7: VRAM_Buffer2 */
    0x00u, /*  8: BowserPaletteData (ROM) */
    0x00u, /*  9: DaySnowPaletteData (ROM) */
    0x00u, /* 10: NightSnowPaletteData (ROM) */
    0x00u, /* 11: MushroomPaletteData (ROM) */
    0x00u, /* 12: MarioThanksMessage (ROM) */
    0x00u, /* 13: LuigiThanksMessage (ROM) */
    0x00u, /* 14: MushroomRetainerSaved (ROM) */
    0x00u, /* 15: PrincessSaved1 (ROM) */
    0x00u, /* 16: PrincessSaved2 (ROM) */
    0x00u, /* 17: WorldSelectMessage1 (ROM) */
    0x00u, /* 18: WorldSelectMessage2 (ROM) */
};
static const uint8_t VRAM_ADDR_TABLE_HIGH[VRAM_ADDR_TABLE_SIZE] = {
    0x03u, 0x00u, 0x00u, 0x00u, 0x00u, 0x03u, 0x03u, 0x03u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
};

/* the host-side counterparts of the ROM addr table entries; NULL marks the
 * RAM entries, which read through $00/$01 as the original does */
static const uint8_t *const VRAM_ADDR_ROM_DATA[VRAM_ADDR_TABLE_SIZE] = {
    NULL,                             /*  0: VRAM_Buffer1 */
    SMBC_WATER_PALETTE_DATA,          /*  1 */
    SMBC_GROUND_PALETTE_DATA,         /*  2 */
    SMBC_UNDERGROUND_PALETTE_DATA,    /*  3 */
    SMBC_CASTLE_PALETTE_DATA,         /*  4 */
    NULL,                             /*  5: VRAM_Buffer1_Offset */
    NULL,                             /*  6: VRAM_Buffer2 */
    NULL,                             /*  7: VRAM_Buffer2 */
    SMBC_BOWSER_PALETTE_DATA,         /*  8 */
    SMBC_DAY_SNOW_PALETTE_DATA,       /*  9 */
    SMBC_NIGHT_SNOW_PALETTE_DATA,     /* 10 */
    SMBC_MUSHROOM_PALETTE_DATA,       /* 11 */
    SMBC_MARIO_THANKS_MESSAGE,        /* 12 */
    SMBC_LUIGI_THANKS_MESSAGE,        /* 13 */
    SMBC_MUSHROOM_RETAINER_SAVED,     /* 14 */
    SMBC_PRINCESS_SAVED1,             /* 15 */
    SMBC_PRINCESS_SAVED2,             /* 16 */
    SMBC_WORLD_SELECT_MESSAGE1,       /* 17 */
    SMBC_WORLD_SELECT_MESSAGE2,       /* 18 */
};

void load_vram_addr_table(CoreState *state) {
  /* smb.asm: ldx VRAM_Buffer_AddrCtrl; lda VRAM_AddrTable_Low,x ... */
  uint8_t ctrl = state->ram[SMBC_VRAM_BUFFER_ADDR_CTRL];
  if (ctrl >= VRAM_ADDR_TABLE_SIZE) {
    ctrl = 0; /* the original trusts the index; clamp rather than read OOB */
  }
  state->ram[SMBC_TEMP_VRAM_PTR] = VRAM_ADDR_TABLE_LOW[ctrl];
  state->ram[SMBC_TEMP_VRAM_PTR + 1] = VRAM_ADDR_TABLE_HIGH[ctrl];
}

/* smb.asm: the update-set reader. The original walks a 6502 ($00/$01)
 * indirect pointer through the update sets, whether they live in RAM (the two
 * VRAM buffers) or ROM (palette and message data). RAM entries keep that
 * pointer in RAM exactly as the original does; ROM entries cannot be
 * expressed as two RAM bytes, so the host walks an index into the ROM table
 * instead. `pos` is the offset of the next byte within the active source. */
static uint8_t source_byte(const CoreState *state, const uint8_t *rom,
                           uint16_t pos, uint8_t y) {
  if (rom != NULL) {
    return rom[(uint16_t)(pos + y)];
  }
  /* RAM: pos IS the $00/$01 pointer, and y is the offset within the set */
  return state->ram[(uint16_t)(pos + y)];
}

/* smb.asm: OutputToVRAM — X counts down from the length, so length 0 runs
 * 256 iterations. Repeat mode is 16 cycles per byte, otherwise 17; each
 * taken bne back to the loop top adds one cycle except the last. */
static uint32_t vram_loop_cycles(uint8_t length, bool repeat) {
  uint32_t n = length == 0 ? 256u : length;
  return n * (repeat ? 16u : 17u) + (n - 1u);
}

void update_screen(Smbc *smbc) {
  /* smb.asm: UpdateScreen */
  CoreState *state = &smbc->state;
  Ppu *ppu = &smbc->ppu;
  (void)ppu_read(ppu, 2); /* ldx PPU_STATUS: reset the flip-flop */

  uint8_t ctrl = state->ram[SMBC_VRAM_BUFFER_ADDR_CTRL];
  const uint8_t *rom =
      ctrl < VRAM_ADDR_TABLE_SIZE ? VRAM_ADDR_ROM_DATA[ctrl] : NULL;

  uint16_t pos;
  if (rom != NULL) {
    pos = 0u; /* ROM: index into the table */
  } else {
    pos = (uint16_t)(state->ram[SMBC_TEMP_VRAM_PTR] |
                    ((uint16_t)state->ram[SMBC_TEMP_VRAM_PTR + 1] << 8));
  }

  uint8_t first = source_byte(state, rom, pos, 0);
  uint32_t cycles = 13u + (first != 0 ? 1u : 0u); /* UpdateScreen entry check */
  while (first != 0) {
    /* smb.asm: WriteBufferToScreen — one update set:
     *   byte 1-2: VRAM address, high then low
     *   byte 3:   d7 increment-by-32, d6 repeat, d5-d0 length
     *   then the data bytes */
    uint8_t y = 0;
    ppu_write(ppu, 6, first);
    ppu_write(ppu, 6, source_byte(state, rom, pos, (uint8_t)(y + 1u)));
    y = (uint8_t)(y + 2u);
    uint8_t third = source_byte(state, rom, pos, y);

    uint8_t mirror = (uint8_t)(state->ram[SMBC_MIRROR_PPU_CTRL1] | 0x04u);
    if ((third & 0x80u) == 0) {
      mirror &= 0xfbu; /* smb.asm: bcs SetupWrites / and #%11111011 */
    }
    write_ppu_reg1(smbc, mirror);

    bool repeat = (third & 0x40u) != 0; /* d6 of the third byte */
    uint8_t length = (uint8_t)(third & 0x3fu);
    cycles += 103u + ((third & 0x80u) != 0 ? 3u : 4u) + (repeat ? 6u : 3u) +
              vram_loop_cycles(length, repeat);
    if (repeat) {
      y++; /* smb.asm: ora #%00000010; iny */
    }
    unsigned count = length == 0 ? 256u : length;
    for (unsigned i = 0; i < count; i++) {
      /* smb.asm: OutputToVRAM — repeat mode holds Y on the data byte */
      if (!repeat) {
        y++;
      }
      ppu_write(ppu, 7, source_byte(state, rom, pos, y));
    }

    /* smb.asm: sec; tya; adc $00; sta $00; lda #$00; adc $01; sta $01 —
     * advance past this set onto the next header. RAM entries keep the
     * pointer in $00/$01 as the original does. */
    pos = (uint16_t)(pos + (uint16_t)y + 1u);
    if (rom == NULL) {
      state->ram[SMBC_TEMP_VRAM_PTR] = (uint8_t)pos;
      state->ram[SMBC_TEMP_VRAM_PTR + 1] = (uint8_t)(pos >> 8);
    }
    ppu_write(ppu, 6, 0x3fu);   /* smb.asm: the $3F00/$00/$00 sequence */
    ppu_write(ppu, 6, 0x00u);
    ppu_write(ppu, 6, 0x00u);
    ppu_write(ppu, 6, 0x00u);
    first = source_byte(state, rom, pos, 0);
    cycles += 13u + (first != 0 ? 1u : 0u);
  }
  cycles += 14u; /* smb.asm: InitScroll body, reached by falling through */
  smbc_charge_cycles(smbc, cycles);
  init_scroll(smbc, first); /* smb.asm: falls through to InitScroll, A=0 */
}

/* smb.asm: TopScoreCheck — six-digit subtraction, lowest digit first; the
 * score is copied only if no borrow survives the final digit. */
static void top_score_check(CoreState *state, int x) {
  int y = 5;
  int no_borrow = 1; /* sec on entry */
  for (int i = 0; i < 6; i++) {
    int diff = state->ram[SMBC_PLAYER_SCORE_DISPLAY + x] -
               state->ram[SMBC_TOP_SCORE_DISPLAY + y] - (1 - no_borrow);
    no_borrow = diff >= 0;
    x--;
    y--;
  }
  if (!no_borrow) {
    return; /* smb.asm: NoTopSc */
  }
  x++;
  y++;
  for (int i = 0; i < 6; i++) {
    state->ram[SMBC_TOP_SCORE_DISPLAY + y] =
        state->ram[SMBC_PLAYER_SCORE_DISPLAY + x];
    x++;
    y++;
  }
}

void update_top_score(CoreState *state) {
  /* smb.asm: UpdateTopScore — mario's score, then luigi's */
  top_score_check(state, 0x05);
  top_score_check(state, 0x0b);
}

void get_player_colors(Smbc *smbc) { get_player_colors_core(&smbc->state); }

void get_player_colors_core(CoreState *st) {
  /* smb.asm: GetPlayerColors */
  uint8_t x = st->ram[SMBC_VRAM_BUFFER1_OFFSET];
  uint8_t y = 0x00;
  if (st->ram[SMBC_CURRENT_PLAYER] != 0x00) y = 0x04u; /* luigi's offset */
  if (st->ram[SMBC_PLAYER_STATUS] == 0x02u) y = 0x08u; /* fiery offset */
  for (int i = 3; i >= 0; i--) { /* ClrGetLoop */
    st->ram[SMBC_VRAM_BUFFER1 + 3 + x] = SMBC_PLAYER_COLORS[y];
    y++;
    x++;
  }
  x = st->ram[SMBC_VRAM_BUFFER1_OFFSET];
  /* the background colour overwrites the first fetched player colour */
  y = st->ram[SMBC_BACKGROUND_COLOR_CTRL];
  if (y == 0x00) y = st->ram[SMBC_AREA_TYPE]; /* SetBGColor */
  st->ram[SMBC_VRAM_BUFFER1 + 3 + x] = SMBC_BACKGROUND_COLORS[y];
  st->ram[SMBC_VRAM_BUFFER1 + x] = 0x3fu;     /* sprite palette address */
  st->ram[SMBC_VRAM_BUFFER1 + 1 + x] = 0x10u;
  st->ram[SMBC_VRAM_BUFFER1 + 2 + x] = 0x04u; /* length byte */
  st->ram[SMBC_VRAM_BUFFER1 + 7 + x] = 0x00u; /* null terminator */
  st->ram[SMBC_VRAM_BUFFER1_OFFSET] = (uint8_t)(x + 7u); /* SetVRAMOffset */
}

void update_number(CoreState *state, uint8_t nybbles) {
  /* smb.asm: UpdateNumber */
  print_status_bar_numbers(state, nybbles);
  uint8_t y = state->ram[SMBC_VRAM_BUFFER1_OFFSET];
  /* NoZSup — zero-suppress the score's highest digit */
  if (state->ram[SMBC_VRAM_BUFFER1 - 6 + y] == 0x00) {
    state->ram[SMBC_VRAM_BUFFER1 - 6 + y] = 0x24u; /* space tile */
  }
}

void get_sb_nybbles(CoreState *state) {
  /* smb.asm: GetSBNybbles — falls through into UpdateNumber */
  update_number(state, SMBC_STATUS_BAR_NYBBLES[state->ram[SMBC_CURRENT_PLAYER]]);
}

void write_game_text(CoreState *state, uint8_t text) {
  /* smb.asm: WriteGameText */
  uint8_t y = (uint8_t)(text * 2u);
  if (y >= 0x08u) {
    y = 0x08u; /* the warp zone */
  } else if (y >= 0x04u && state->ram[SMBC_NUMBER_OF_PLAYERS] == 0x00) {
    y++; /* one player: skip the name row */
  }
  uint8_t x = SMBC_GAME_TEXT_OFFSETS[y];
  y = 0x00;
  for (;;) { /* GameTextLoop */
    uint8_t a = SMBC_GAME_TEXT[x];
    if (a == 0xffu) break; /* EndGameText */
    state->ram[SMBC_VRAM_BUFFER1 + y] = a;
    x++;
    y++;
  }
  state->ram[SMBC_VRAM_BUFFER1 + y] = 0x00; /* null terminator */
  if (text >= 0x04u) {
    /* smb.asm: PrintWarpZoneNumbers — a number in every fourth space */
    x = (uint8_t)((text - 0x04u) * 4u);
    for (y = 0x00; y < 0x0cu; y = (uint8_t)(y + 4u)) {
      state->ram[SMBC_VRAM_BUFFER1 + 27 + y] = SMBC_WARP_ZONE_NUMBERS[x];
      x++;
    }
    state->ram[SMBC_VRAM_BUFFER1_OFFSET] = 0x2cu; /* SetVRAMOffset */
    return;
  }
  if (text == 0x01u) {
    /* the world/lives display: patch in lives, world and level */
    uint8_t lives = (uint8_t)(state->ram[SMBC_NUMBEROF_LIVES] + 1u);
    if (lives >= 10u) { /* PutLives — more than 9: a crown tile */
      lives = (uint8_t)(lives - 10u);
      state->ram[SMBC_VRAM_BUFFER1 + 7] = 0x9fu;
    }
    state->ram[SMBC_VRAM_BUFFER1 + 8] = lives;
    state->ram[SMBC_VRAM_BUFFER1 + 19] =
        (uint8_t)(state->ram[SMBC_WORLD_NUMBER] + 1u);
    state->ram[SMBC_VRAM_BUFFER1 + 21] =
        (uint8_t)(state->ram[SMBC_LEVEL_NUMBER] + 1u);
    return;
  }
  /* smb.asm: CheckPlayerName */
  if (state->ram[SMBC_NUMBER_OF_PLAYERS] == 0x00u) return;
  uint8_t player = state->ram[SMBC_CURRENT_PLAYER];
  if (text == 0x02u && state->ram[SMBC_OPER_MODE] != SMBC_MODE_GAME_OVER) {
    player ^= 0x01u; /* time up names the other player */
  }
  if ((player & 0x01u) == 0x00u) return; /* mario: keep the name */
  for (int i = 4; i >= 0; i--) { /* NameLoop */
    state->ram[SMBC_VRAM_BUFFER1 + 3 + i] = SMBC_LUIGI_NAME[i];
  }
}

/* smb.asm: ResetScreenTimer — the tail of ResetSpritesAndScreenTimer. */
static void reset_screen_timer(CoreState *st) {
  st->ram[SMBC_SCREEN_TIMER] = 0x07u;
  st->ram[SMBC_SCREEN_ROUTINE_TASK]++;
}

/* smb.asm: OutputInter — the shared tail of DisplayTimeUp and
 * DisplayIntermediate's PlayerInter path. `text` selects the message. */
static void output_inter(Smbc *smbc, uint8_t text) {
  CoreState *st = &smbc->state;
  write_game_text(st, text);
  reset_screen_timer(st);
  st->ram[SMBC_DISABLE_SCREEN_FLAG] = 0x00; /* reenable screen output */
}

void screen_routines(Smbc *smbc) {
  CoreState *st = &smbc->state;
  switch (st->ram[SMBC_SCREEN_ROUTINE_TASK]) {
    case 0: /* smb.asm: InitScreen */
      move_all_sprites_offscreen(st);
      initialize_name_tables(smbc);
      if (st->ram[SMBC_OPER_MODE] != SMBC_MODE_TITLE) {
        st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] = 0x03u; /* SetVRAMAddr_A */
      }
      st->ram[SMBC_SCREEN_ROUTINE_TASK]++; /* NextSubtask */
      break;
    case 1: { /* smb.asm: SetupIntermediate */
      uint8_t bg = st->ram[SMBC_BACKGROUND_COLOR_CTRL];
      uint8_t status = st->ram[SMBC_PLAYER_STATUS];
      st->ram[SMBC_PLAYER_STATUS] = 0x00;      /* not fiery */
      st->ram[SMBC_BACKGROUND_COLOR_CTRL] = 0x02u; /* the one time it is < 4 */
      get_player_colors(smbc);
      st->ram[SMBC_PLAYER_STATUS] = status;
      st->ram[SMBC_BACKGROUND_COLOR_CTRL] = bg;
      st->ram[SMBC_SCREEN_ROUTINE_TASK]++;
      break;
    }
    case 2: /* smb.asm: WriteTopStatusLine */
      write_game_text(st, 0x00u); /* the main status bar */
      st->ram[SMBC_SCREEN_ROUTINE_TASK]++;
      break;
    case 3: { /* smb.asm: WriteBottomStatusLine */
      get_sb_nybbles(st);
      uint8_t x = st->ram[SMBC_VRAM_BUFFER1_OFFSET];
      st->ram[SMBC_VRAM_BUFFER1 + x] = 0x20u;     /* world-area number */
      st->ram[SMBC_VRAM_BUFFER1 + 1 + x] = 0x73u;
      st->ram[SMBC_VRAM_BUFFER1 + 2 + x] = 0x03u; /* length */
      st->ram[SMBC_VRAM_BUFFER1 + 3 + x] =
          (uint8_t)(st->ram[SMBC_WORLD_NUMBER] + 1u);
      st->ram[SMBC_VRAM_BUFFER1 + 4 + x] = 0x28u; /* the dash */
      st->ram[SMBC_VRAM_BUFFER1 + 5 + x] =
          (uint8_t)(st->ram[SMBC_LEVEL_NUMBER] + 1u);
      st->ram[SMBC_VRAM_BUFFER1 + 6 + x] = 0x00u; /* null terminator */
      st->ram[SMBC_VRAM_BUFFER1_OFFSET] = (uint8_t)(x + 6u);
      st->ram[SMBC_SCREEN_ROUTINE_TASK]++;
      break;
    }
    case 4: /* smb.asm: DisplayTimeUp */
      if (st->ram[SMBC_GAME_TIMER_EXPIRED_FLAG] == 0x00) {
        st->ram[SMBC_SCREEN_ROUTINE_TASK] += 2u; /* NoTimeUp */
        break;
      }
      st->ram[SMBC_GAME_TIMER_EXPIRED_FLAG] = 0x00;
      output_inter(smbc, 0x02u); /* the time-up screen */
      break;
    case 5:
    case 7: /* smb.asm: ResetSpritesAndScreenTimer */
      if (st->ram[SMBC_SCREEN_TIMER] != 0x00) break; /* NoReset */
      move_all_sprites_offscreen(st);
      reset_screen_timer(st);
      break;
    case 6: { /* smb.asm: DisplayIntermediate */
      uint8_t mode = st->ram[SMBC_OPER_MODE];
      if (mode == SMBC_MODE_GAME_OVER) {
        st->ram[SMBC_SCREEN_TIMER] = 0x12u;
        write_game_text(st, 0x03u); /* the game over screen */
        st->ram[SMBC_OPER_MODE_TASK]++; /* IncModeTask_B */
        break;
      }
      if (mode != SMBC_MODE_TITLE &&
          st->ram[SMBC_ALT_ENTRANCE_CONTROL] == 0x00 &&
          (st->ram[SMBC_AREA_TYPE] == 0x03u || /* castle: possibly residual */
           st->ram[SMBC_DISABLE_INTERMEDIATE] == 0x00)) {
        /* smb.asm: DrawPlayer_Intermediate */
        draw_player_intermediate(st);
        output_inter(smbc, 0x01u); /* the lives display */
        break;
      }
      st->ram[SMBC_SCREEN_ROUTINE_TASK] = 0x08u; /* NoInter */
      break;
    }
    case 8: /* smb.asm: AreaParserTaskControl */
      st->ram[SMBC_DISABLE_SCREEN_FLAG]++; /* turn off screen */
      do {
        area_parser_task_handler(st);
      } while (st->ram[SMBC_AREA_PARSER_TASK_NUM] != 0x00); /* TaskLoop */
      st->ram[SMBC_COLUMN_SETS]--;
      if ((st->ram[SMBC_COLUMN_SETS] & 0x80u) != 0x00) {
        st->ram[SMBC_SCREEN_ROUTINE_TASK]++; /* no more sets: move on */
      }
      st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] = 0x06u; /* OutputCol */
      break;
    case 9: /* smb.asm: GetAreaPalette */
      st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] =
          SMBC_AREA_PALETTE[st->ram[SMBC_AREA_TYPE]]; /* SetVRAMAddr_A */
      st->ram[SMBC_SCREEN_ROUTINE_TASK]++;
      break;
    case 10: { /* smb.asm: GetBackgroundColor — falls into GetPlayerColors */
      uint8_t ctrl = st->ram[SMBC_BACKGROUND_COLOR_CTRL];
      /* ctrl is 0 or 4-7 by design; 1-3 read stray bytes in the original */
      if (ctrl >= 0x04u) {
        st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] = SMBC_BG_COLOR_CTRL_ADDR[ctrl - 4u];
      }
      st->ram[SMBC_SCREEN_ROUTINE_TASK]++; /* NoBGColor */
      get_player_colors(smbc);
      break;
    }
    case 11: /* smb.asm: GetAlternatePalette1 */
      if (st->ram[SMBC_AREA_STYLE] == 0x01u) {
        st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] = 0x0bu; /* SetVRAMAddr_B */
      }
      st->ram[SMBC_SCREEN_ROUTINE_TASK]++;
      break;
    case 12: /* smb.asm: DrawTitleScreen */
      if (st->ram[SMBC_OPER_MODE] != SMBC_MODE_TITLE) {
        st->ram[SMBC_OPER_MODE_TASK]++; /* IncModeTask_B */
        break;
      }
      /* smb.asm: read the title screen data ($1ec0 in the CHR) through
       * $2007 into the $0300 buffer page — 314 bytes, one priming read */
      ppu_write(&smbc->ppu, 6, 0x1eu); /* smb.asm: PPU_ADDRESS hi/lo */
      ppu_write(&smbc->ppu, 6, 0xc0u);
      (void)ppu_read(&smbc->ppu, 7); /* the garbage read */
      for (uint16_t i = 0; i < 0x13au; i++) { /* OutputTScr */
        st->ram[SMBC_VRAM_BUFFER1 - 1 + i] = ppu_read(&smbc->ppu, 7);
      }
      st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] = 0x05u; /* SetVRAMAddr_B */
      st->ram[SMBC_SCREEN_ROUTINE_TASK]++;
      break;
    case 13: /* smb.asm: ClearBuffersDrawIcon */
      if (st->ram[SMBC_OPER_MODE] != SMBC_MODE_TITLE) {
        st->ram[SMBC_OPER_MODE_TASK]++; /* IncModeTask_B */
        break;
      }
      /* clear the buffer pages $0300-$04ff (a holds OperMode, i.e. zero) */
      for (int i = 0; i < 0x100; i++) { /* TScrClear */
        st->ram[SMBC_VRAM_BUFFER1 - 1 + i] = 0x00;
        st->ram[SMBC_VRAM_BUFFER1 - 1 + 0x100 + i] = 0x00;
      }
      draw_mushroom_icon(smbc);
      st->ram[SMBC_SCREEN_ROUTINE_TASK]++;
      break;
    case 14: /* smb.asm: WriteTopScore */
      update_number(st, 0xfau);
      st->ram[SMBC_OPER_MODE_TASK]++; /* IncModeTask_B */
      break;
    default:
      break;
  }
}

/* smb.asm: EndAreaPoints — award the digit modifier to the
 * current player's score, then print score and timer */
void end_area_points(CoreState *st) {
  uint8_t y = st->ram[SMBC_CURRENT_PLAYER] == 0x00u ? 0x0bu : 0x11u;
  digits_math_routine(st, y);
  uint8_t nybbles = (uint8_t)(st->ram[SMBC_CURRENT_PLAYER] << 2);
  update_number(st, (uint8_t)(nybbles | 0x04u));
}

/* smb.asm: AddToScore — apply the digit modifiers to the current
 * player's score, then refresh the status bar through
 * GetSBNybbles/UpdateNumber */
void add_to_score(CoreState *st) {
  digits_math_routine(st, SMBC_SCORE_OFFSETS[st->ram[SMBC_CURRENT_PLAYER]]);
  get_sb_nybbles(st);
}

/* smb.asm: FloateyNumTileData — the two tile halves of each
 * point value, 1-UP last */
static const uint8_t SMBC_FLOATEY_NUM_TILE_DATA[24] = {
    0xffu, 0xffu, 0xf6u, 0xfbu, 0xf7u, 0xfbu, 0xf8u, 0xfbu,
    0xf9u, 0xfbu, 0xfau, 0xfbu, 0xf6u, 0x50u, 0xf7u, 0x50u,
    0xf8u, 0x50u, 0xf9u, 0x50u, 0xfau, 0x50u, 0xfdu, 0xfeu};

/* smb.asm: ScoreUpdateData — high nybble the score digit, low
 * nybble the amount added to it */
static const uint8_t SMBC_SCORE_UPDATE_DATA[12] = {
    0xffu, 0x41u, 0x42u, 0x44u, 0x45u, 0x48u,
    0x31u, 0x32u, 0x34u, 0x35u, 0x38u, 0x00u};

/* smb.asm: FloateyNumbersRoutine — the rising point number over
 * a defeated enemy: counts down its timer, banks the points at
 * $2b (a 1-up for control $0b), rises a pixel a frame, and draws
 * its two sprite halves */
void floatey_numbers_routine(CoreState *st, uint8_t x) {
  uint8_t ctrl = st->ram[SMBC_FLOATEY_NUM_CONTROL + x];
  if (ctrl == 0x00u) return;
  if (ctrl >= 0x0bu) {
    ctrl = 0x0bu;
    st->ram[SMBC_FLOATEY_NUM_CONTROL + x] = 0x0bu;
  }
  uint8_t timer = st->ram[SMBC_FLOATEY_NUM_TIMER + x];
  if (timer == 0x00u) {
    st->ram[SMBC_FLOATEY_NUM_CONTROL + x] = 0x00u;
    return;
  }
  st->ram[SMBC_FLOATEY_NUM_TIMER + x]--;
  if (timer == 0x2bu) {
    if (ctrl == 0x0bu) {
      st->ram[SMBC_NUMBEROF_LIVES]++;
      st->ram[SMBC_SQUARE2_SOUND_QUEUE] = SMBC_SFX_EXTRA_LIFE;
    }
    uint8_t update = SMBC_SCORE_UPDATE_DATA[ctrl];
    st->ram[SMBC_DIGIT_MODIFIER + (update >> 4u)] =
        (uint8_t)(update & 0x0fu);
    add_to_score(st);
  }
  /* smb.asm: ChkTallEnemy — tall enemies not on the exemption
   * list, the hammer bro, and active short enemies use the
   * alternate sprite data offset */
  uint8_t y = st->ram[SMBC_ENEMY_SPR_DATA_OFFSET + x];
  uint8_t id = st->ram[SMBC_ENEMY_ID + x];
  if (id != SMBC_SPINY && id != SMBC_PIRANHA_PLANT && id != 0x0au &&
      id != 0x0bu &&
      (id == SMBC_HAMMER_BRO || id >= 0x09u ||
       st->ram[SMBC_ENEMY_STATE + x] < 0x02u)) {
    y = st->ram[SMBC_ALT_SPR_DATA_OFFSET +
                st->ram[SMBC_SPDATA_OFFSET_CTRL]];
  }
  if (st->ram[SMBC_FLOATEY_NUM_Y_POS + x] >= 0x18u) {
    st->ram[SMBC_FLOATEY_NUM_Y_POS + x]--;
  }
  /* smb.asm: SetupNumSpr + DumpTwoSpr */
  uint8_t a = (uint8_t)(st->ram[SMBC_FLOATEY_NUM_Y_POS + x] - 0x08u);
  st->ram[SMBC_SPRITE_Y_POSITION + y] = a;
  st->ram[SMBC_SPRITE_Y_POSITION + 4u + y] = a;
  uint8_t xpos = st->ram[SMBC_FLOATEY_NUM_X_POS + x];
  st->ram[SMBC_SPRITE_X_POSITION + y] = xpos;
  st->ram[SMBC_SPRITE_X_POSITION + 4u + y] = (uint8_t)(xpos + 0x08u);
  st->ram[SMBC_SPRITE_ATTRIBUTES + y] = 0x02u;
  st->ram[SMBC_SPRITE_ATTRIBUTES + 4u + y] = 0x02u;
  st->ram[SMBC_SPRITE_TILENUMBER + y] =
      SMBC_FLOATEY_NUM_TILE_DATA[ctrl * 2u];
  st->ram[SMBC_SPRITE_TILENUMBER + 4u + y] =
      SMBC_FLOATEY_NUM_TILE_DATA[ctrl * 2u + 1u];
}

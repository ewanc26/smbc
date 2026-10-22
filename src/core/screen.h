/* core/screen.h — screen update helpers.
 *
 * Owns: the small PPU-writing subroutines the reset and NMI paths share,
 * the top-score bookkeeping, WriteGameText, and the ScreenRoutines task
 * tree with its palette and colour subroutines. DrawPlayer_Intermediate
 * and the area parser are pending their own atoms — the tasks that need
 * them run their control flow and skip the write.
 */
#ifndef SMBC_CORE_SCREEN_H
#define SMBC_CORE_SCREEN_H

#include <stdint.h>

#include "state.h"

struct Smbc; /* frame.h */

/* smb.asm: WritePPUReg1 — write to $2000 and its mirror together. */
void write_ppu_reg1(struct Smbc *smbc, uint8_t value);

/* smb.asm: InitScroll — write the same value to $2005 twice. */
void init_scroll(struct Smbc *smbc, uint8_t value);

/* smb.asm: the NMI's pointer load — sets the $00/$01 indirect from
 * VRAM_AddrTable_Low/High indexed by VRAM_Buffer_AddrCtrl. ROM-backed
 * entries (palettes, messages) are pending their data tables and read as
 * empty update sets until then. */
void load_vram_addr_table(CoreState *state);

/* smb.asm: UpdateScreen / WriteBufferToScreen — walk the update sets at
 * the $00/$01 indirect, writing each to VRAM through $2006/$2007, until a
 * zero high byte. Falls through to InitScroll either way. */
void update_screen(struct Smbc *smbc);

/* smb.asm: UpdateTopScore — copy either player's score into the top score
 * display when it exceeds it. */
uint32_t update_top_score(CoreState *state);

/* smb.asm: PrintStatusBarNumbers — print the status bar values selected by
 * the two nybbles of `nybbles` into VRAM buffer 1: the low nybble selects
 * one display (score, coin tally, game timer — see data/statusbar), the
 * high nybble selects another. Selectors of six or more print nothing.
 * Callers handle zero suppression. */
void print_status_bar_numbers(CoreState *state, uint8_t nybbles);

/* smb.asm: DigitsMathRoutine — apply the six digit modifiers to the six
 * display digits ending at index `y`, borrowing and carrying between
 * digits (digits stay 0-9), then erase the modifiers. On the title screen
 * only the erase happens. */
void digits_math_routine(CoreState *state, uint8_t y);

/* smb.asm: AddToScore / GetSBNybbles / UpdateNumber — apply the digit
 * modifiers to the current player's score and refresh the status bar */
void add_to_score(CoreState *state);

/* smb.asm: FloateyNumbersRoutine — the rising point number over a
 * defeated enemy */
void floatey_numbers_routine(CoreState *state, uint8_t x);

/* smb.asm: GetPlayerColors — append the player's palette set to VRAM
 * buffer 1 at its current offset: $3f $10, length 4, the background
 * colour (from BackgroundColorCtrl when set, else AreaType) followed by
 * the three player colours, then a terminator. Called from the screen
 * task tree and the player atom's palette changes. */
void get_player_colors(struct Smbc *smbc);
/* the same on bare state, for callers without a Smbc */
void get_player_colors_core(CoreState *state);

/* smb.asm: UpdateNumber — print the status bar values selected by
 * `nybbles`, then zero-suppress the score's highest digit. */
void update_number(CoreState *state, uint8_t nybbles);

/* smb.asm: EndAreaPoints — award the digit modifier (game timer
 * intervals or fireworks 500s) to the current player's score and
 * print the updated score and timer */
void end_area_points(CoreState *state);

/* smb.asm: GetSBNybbles — UpdateNumber with the current player's
 * StatusBarNybbles entry. */
void get_sb_nybbles(CoreState *state);

/* smb.asm: WriteGameText — copy message number `text` (0 top status bar,
 * 1 world/lives display, 2 time up, 3 game over, 4 warp zone welcome)
 * into VRAM buffer 1 from its start. The world/lives display gets the
 * lives count (a crown tile past nine), world and level numbers patched
 * in; two-player messages get "LUIGI" over "MARIO" for the right player;
 * the warp zone gets its world numbers and sets the buffer offset to
 * $2c. */
void write_game_text(CoreState *state, uint8_t text);

/* smb.asm: ScreenRoutines — the screen task tree, jump table order:
 *   0 InitScreen, 1 SetupIntermediate, 2 WriteTopStatusLine,
 *   3 WriteBottomStatusLine, 4 DisplayTimeUp, 5/7 ResetSpritesAndScreenTimer,
 *   6 DisplayIntermediate, 8 AreaParserTaskControl, 9 GetAreaPalette,
 *   10 GetBackgroundColor, 11 GetAlternatePalette1, 12 DrawTitleScreen,
 *   13 ClearBuffersDrawIcon, 14 WriteTopScore */
void screen_routines(struct Smbc *smbc);

#endif /* SMBC_CORE_SCREEN_H */

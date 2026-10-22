/* core/gamemode.c — see core/gamemode.h for the contract. */
#include "gamemode.h"
#include "fball.h"
#include "flagpole.h"

#include "blocks.h"
#include "cannon.h"
#include "colour.h"
#include "enemies.h"
#include "frame.h"
#include "miscobj.h"
#include "initialize.h"
#include "offscreen.h"
#include "parser.h"
#include "playergfx.h"
#include "routines.h"
#include "screen.h"
#include "setup.h"
#include "state.h"
#include "timer.h"
#include "whirlpool.h"

void upd_scroll_var(Smbc *smbc) {
  CoreState *st = &smbc->state;
  /* smb.asm: UpdScrollVar — the vram address controller at 6 (one of two
   * $0341s) means the buffer is mid-write; leave */
  if (st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] == 0x06) return; /* ExitEng */
  if (st->ram[SMBC_AREA_PARSER_TASK_NUM] == 0x00) {
    if (st->ram[SMBC_SCROLL_THIRTY_TWO] < 0x20) return; /* bmi ExitEng */
    /* wrap the 0-31 scroll range and reset the level graphics buffer */
    st->ram[SMBC_SCROLL_THIRTY_TWO] -= 0x20u;
    st->ram[SMBC_VRAM_BUFFER2_OFFSET] = 0x00;
  }
  /* smb.asm: RunParser — AreaParserTaskHandler */
  smbc_charge_cycles(smbc, area_parser_task_handler(st));
}

/* smb.asm: GameRoutines — jump table order:
 *   0 Entrance_GameTimerSetup, 1 Vine_AutoClimb, 2 SideExitPipeEntry,
 *   3 VerticalPipeEntry, 4 FlagpoleSlide, 5 PlayerEndLevel,
 *   6 PlayerLoseLife, 7 PlayerEntrance, 8 PlayerCtrlRoutine,
 *   9 PlayerChangeSize, 10 PlayerInjuryBlink, 11 PlayerDeath,
 *   12 PlayerFireFlower */
static void game_routines(Smbc *smbc) {
  switch (smbc->state.ram[SMBC_GAME_ENGINE_SUBROUTINE]) {
    case 0x00u: entrance_game_timer_setup(smbc); break;
    case 0x01u: vine_auto_climb(smbc); break;
    case 0x02u: side_exit_pipe_entry(smbc); break;
    case 0x03u: vertical_pipe_entry(smbc); break;
    case 0x04u: flagpole_slide(smbc); break;
    case 0x05u: player_end_level(smbc); break;
    case 0x06u: player_lose_life(smbc); break;
    case 0x07u: player_entrance(smbc); break;
    case 0x08u: player_ctrl_routine(smbc); break;
    case 0x09u: player_change_size(smbc); break;
    case 0x0au: player_injury_blink(smbc); break;
    case 0x0bu: player_death(smbc); break;
    case 0x0cu: player_fire_flower(smbc); break;
    default: break;
  }
}

/* smb.asm: GameEngine — the per-frame object and music processing. The
 * fireball/bubble, enemy, translation, offscreen, block, cannon,
 * whirlpool, flagpole and timer calls run, as do the music check,
 * palette cycling and the SaveAB tail. */
static void game_engine(Smbc *smbc) {
  CoreState *st = &smbc->state;
  /* smb.asm: ProcFireball_Bubble */
  proc_fireball_bubble(st);
  /* smb.asm: ProcELoop — the enemy and floatey-number passes over all
   * six slots */
  for (uint8_t x = 0x00u; x < 0x06u; x++) {
    st->ram[SMBC_OBJECT_OFFSET] = x;
    enemies_and_loops_core(st, x);
    floatey_numbers_routine(st, x);
  }
  /* smb.asm: GetPlayerOffscreenBits, then RelativePlayerPosition — the
   * player's offscreen bits and screen-relative coordinates, ahead of
   * PlayerGfxHandler */
  get_player_offscreen_bits(st);
  relative_player_position(st);
  /* smb.asm: PlayerGfxHandler */
  player_gfx_handler(st);
  /* smb.asm: BlockObjMT_Updater, then BlockObjectsCore for the second
   * block object and the first */
  block_obj_mt_updater(st);
  st->ram[SMBC_OBJECT_OFFSET] = 0x01u;
  block_objects_core(st, 0x01u);
  st->ram[SMBC_OBJECT_OFFSET] = 0x00u;
  block_objects_core(st, 0x00u);
  /* smb.asm: MiscObjectsCore */
  misc_objects_core(st);
  /* smb.asm: ProcessCannons */
  process_cannons(st);
  /* smb.asm: ProcessWhirlpools */
  process_whirlpools(st);
  /* smb.asm: FlagpoleRoutine */
  flagpole_routine(st);
  /* smb.asm: RunGameTimer */
  run_game_timer(st);
  /* smb.asm: ColorRotation */
  color_rotation(st);
  bool cycle_palette = true;
  /* cmp #$02 / bpl — the sign of (Player_Y_HighPos - 2) decides whether
   * the player is far enough below the screen to skip the music check */
  if (((st->ram[SMBC_PLAYER_Y_HIGH_POS] - 0x02u) & 0x80u) != 0) {
    uint8_t star = st->ram[SMBC_STAR_INVINCIBLE_TIMER];
    if (star == 0x00) {
      reset_pal_star(st); /* ClrPlrPal */
      cycle_palette = false;
    } else if (star == 0x04u && st->ram[SMBC_INTERVAL_TIMER_CONTROL] == 0x00u) {
      get_area_music(st); /* re-attain appropriate level music */
    }
  }
  if (cycle_palette) {
    /* NoChgMus — the star timer picks the palette cycle rate; the
     * computed value is CyclePlayerPalette's input */
    uint8_t a = st->ram[SMBC_FRAME_COUNTER];
    if (st->ram[SMBC_STAR_INVINCIBLE_TIMER] < 0x08u) a >>= 2;
    a >>= 1;
    cycle_player_palette(st, a);
  }
  /* SaveAB */
  st->ram[SMBC_PREV_A_B_BUTTONS] = st->ram[SMBC_A_B_BUTTONS];
  st->ram[SMBC_LEFT_RIGHT_BUTTONS] = 0x00; /* nullify left/right buttons */
  upd_scroll_var(smbc);
}

void game_core_routine(Smbc *smbc) {
  CoreState *st = &smbc->state;
  /* smb.asm: GameCoreRoutine — the current player's controller bits
   * become the master controller bits */
  st->ram[SMBC_SAVED_JOYPAD1_BITS] =
      st->ram[SMBC_SAVED_JOYPAD1_BITS + st->ram[SMBC_CURRENT_PLAYER]];
  game_routines(smbc);
  if (st->ram[SMBC_OPER_MODE_TASK] >= 0x03) {
    game_engine(smbc); /* only if we are supposed to be here */
  }
}

void game_mode(Smbc *smbc) {
  /* smb.asm: GameMode — jump table order:
   *   0 InitializeArea, 1 ScreenRoutines, 2 SecondaryGameSetup,
   *   3 GameCoreRoutine */
  switch (smbc->state.ram[SMBC_OPER_MODE_TASK]) {
    case 0: initialize_area(smbc); break;
    case 1: screen_routines(smbc); break;
    case 2: secondary_game_setup(smbc); break;
    case 3: game_core_routine(smbc); break;
    default: break;
  }
}

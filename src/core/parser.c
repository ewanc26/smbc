/* core/parser.c — see core/parser.h for the contract. */
#include "parser.h"

#include "../data/metatiles.h"
#include "../data/objects.h"
#include "../data/scenery.h"
#include "screen.h"

#define SMBC_WORLD8 0x07u /* smb.asm: World8 */

/* smb.asm: enemy/object identifier constants written by the handlers */
#define SMBC_PIRANHA_PLANT 0x0du
#define SMBC_FLAGPOLE_FLAG_OBJECT 0x30u
#define SMBC_STAR_FLAG_OBJECT 0x31u
#define SMBC_JUMPSPRING_OBJECT 0x32u

/* Low byte of each palette's graphics table in the ROM (smb.asm:
 * MetatileGraphics_Low). Only the carry out of this byte matters, for the
 * page-cross cycle of the indirect reads in RenderAreaGraphics. */
static const uint8_t kPaletteTableLow[4] = {0x10u, 0xacu, 0x64u, 0x8cu};

/* smb.asm: FindEmptyEnemySlot — the first of five enemy slots with a
 * clear flag, or -1 (the asm's carry) if all are occupied */
static int find_empty_enemy_slot(const CoreState *st) {
  for (uint8_t x = 0; x < 0x05u; x++) {
    if (st->ram[SMBC_ENEMY_FLAG + x] == 0x00u) return x;
  }
  return -1;
}

/* smb.asm: InitPiranhaPlant (+ SetBBox2) — initialise the piranha
 * plant state around the vertical coordinate the caller stored */
static void init_piranha_plant(CoreState *st, uint8_t x) {
  st->ram[SMBC_PIRANHA_PLANT_Y_SPEED + x] = 0x01u;
  st->ram[SMBC_ENEMY_STATE + x] = 0x00u;
  st->ram[SMBC_PIRANHA_PLANT_MOVE_FLAG + x] = 0x00u;
  uint8_t ypos = st->ram[SMBC_ENEMY_Y_POSITION + x];
  st->ram[SMBC_PIRANHA_PLANT_DOWN_Y_POS + x] = ypos;
  st->ram[SMBC_PIRANHA_PLANT_UP_Y_POS + x] = (uint8_t)(ypos - 0x18u);
  st->ram[SMBC_ENEMY_BOUND_BOX_CTRL + x] = 0x09u;
}

/* smb.asm: KillEnemies — clear the enemy flag on every slot holding
 * the given identifier */
void kill_enemies(CoreState *st, uint8_t id) {
  for (int8_t x = 0x04; x >= 0; x--) {
    if (st->ram[SMBC_ENEMY_ID + x] == id)
      st->ram[SMBC_ENEMY_FLAG + x] = 0x00u;
  }
}

/* smb.asm: RenderUnderPart — render metatile A from row x downward
 * for (y + 1) rows, leaving anything worth keeping alone: ledge
 * middles ($17/$1a), palette-3 metatiles above $c0 (but not the coin
 * question block $c0 itself), and the cracked rock terrain ($54) when
 * a mushroom stem ($50) would overwrite it. Returns the row one past
 * the last row touched. */
static uint8_t render_under_part(CoreState *st, uint8_t x, uint8_t a,
                                 uint8_t y) {
  st->ram[SMBC_AREA_OBJECT_HEIGHT] = y;
  for (;;) {
    uint8_t cur = st->ram[SMBC_METATILE_BUFFER + x];
    if (cur != 0x17u && cur != 0x1au && cur <= 0xc0u &&
        !(cur == 0x54u && a == 0x50u)) {
      st->ram[SMBC_METATILE_BUFFER + x] = a;
    }
    x++;
    if (x >= 0x0du) break;
    if (st->ram[SMBC_AREA_OBJECT_HEIGHT] == 0x00u) break;
    st->ram[SMBC_AREA_OBJECT_HEIGHT]--;
  }
  return x;
}

/* smb.asm: ChkLrgObjFixedLength — if the object length counter is
 * unset (negative), store the given length. Returns true (the asm's
 * carry) when the counter was just set. */
static bool chk_lrg_obj_fixed_length(CoreState *st, uint8_t x, uint8_t y) {
  if ((int8_t)st->ram[SMBC_AREA_OBJECT_LENGTH + x] >= 0) return false;
  st->ram[SMBC_AREA_OBJECT_LENGTH + x] = y;
  return true;
}

/* smb.asm: GetLrgObjAttrib — the buffered level object's row (first
 * byte, low nybble) and lower nybble (second byte: length or height) */
static void get_lrg_obj_attrib(const CoreState *st, uint8_t x, uint8_t *row,
                               uint8_t *low) {
  uint8_t y = st->ram[SMBC_AREA_OBJ_OFFSET_BUFFER + x];
  if (row != NULL) *row = (uint8_t)(st->area_data[y] & 0x0fu);
  if (low != NULL) *low = (uint8_t)(st->area_data[y + 1u] & 0x0fu);
}

/* smb.asm: ChkLrgObjLength — GetLrgObjAttrib into ChkLrgObjFixedLength
 * with the lower nybble as the length */
static bool chk_lrg_obj_length(CoreState *st, uint8_t x, uint8_t *row,
                               uint8_t *low) {
  uint8_t tmp;
  if (low == NULL) low = &tmp;
  get_lrg_obj_attrib(st, x, row, low);
  return chk_lrg_obj_fixed_length(st, x, *low);
}

/* smb.asm: GetAreaObjXPosition — the current column as a pixel
 * coordinate (the high bit carries into the page location at the
 * call site) */
static uint8_t get_area_obj_x_position(const CoreState *st) {
  return (uint8_t)(st->ram[SMBC_CURRENT_COLUMN_POS] << 4);
}

/* smb.asm: GetAreaObjYPosition — a metatile row as a pixel coordinate,
 * plus 32 pixels for the status bar */
static uint8_t get_area_obj_y_position(uint8_t row) {
  return (uint8_t)((uint8_t)(row << 4) + 32u);
}

/* smb.asm: IncAreaObjOffset — move the area data pointer to the next
 * object and reset the page select */
static void inc_area_obj_offset(CoreState *st) {
  st->ram[SMBC_AREA_DATA_OFFSET] = (uint8_t)(st->ram[SMBC_AREA_DATA_OFFSET] + 2u);
  st->ram[SMBC_AREA_OBJECT_PAGE_SEL] = 0x00u;
}

/* --- the object handlers (smb.asm: lines 3544-4245) --- */

/* smb.asm: AlterAreaAttributes — the row-14 attribute object: d6 clear
 * rewrites terrain control and background scenery, d6 set rewrites
 * foreground scenery (and background colour control for values >= 4) */
static void alter_area_attributes(CoreState *st, uint8_t x) {
  uint8_t y = (uint8_t)(st->ram[SMBC_AREA_OBJ_OFFSET_BUFFER + x] + 1u);
  uint8_t b2 = st->area_data[y];
  if ((b2 & 0x40u) == 0x00u) {
    st->ram[SMBC_TERRAIN_CONTROL] = (uint8_t)(b2 & 0x0fu);
    st->ram[SMBC_BACKGROUND_SCENERY] = (uint8_t)((b2 & 0x30u) >> 4);
    return;
  }
  uint8_t low = (uint8_t)(b2 & 0x07u);
  if (low >= 0x04u) {
    st->ram[SMBC_BACKGROUND_COLOR_CTRL] = low;
    low = 0x00u;
  }
  st->ram[SMBC_FOREGROUND_SCENERY] = low;
}

/* smb.asm: ScrollLockObject_Warp (+ ScrollLockObject) — print the warp
 * zone text for the current world, kill the piranha plants, then
 * invert the scroll lock (the row-13 scroll lock objects share this
 * tail) */
static void scroll_lock_object_warp(CoreState *st) {
  uint8_t text = 0x04u;
  if (st->ram[SMBC_WORLD_NUMBER] != 0x00u) {
    text++;
    if (st->ram[SMBC_AREA_TYPE] == 0x01u) text++;
  }
  st->ram[SMBC_WARP_ZONE_CONTROL] = text;
  write_game_text(st, text);
  kill_enemies(st, SMBC_PIRANHA_PLANT);
  st->ram[SMBC_SCROLL_LOCK] = (uint8_t)(st->ram[SMBC_SCROLL_LOCK] ^ 0x01u);
}

/* smb.asm: ScrollLockObject — invert the scroll lock */
static void scroll_lock_object(CoreState *st) {
  st->ram[SMBC_SCROLL_LOCK] = (uint8_t)(st->ram[SMBC_SCROLL_LOCK] ^ 0x01u);
}

/* smb.asm: AreaFrenzy — queue the frenzy enemy unless one already
 * occupies a regular enemy slot, in which case nullify the queue */
static void area_frenzy(CoreState *st, uint8_t id) {
  uint8_t frenzy = SMBC_FRENZY_ID_DATA[id - 0x08u];
  uint8_t queue = frenzy;
  for (int8_t y = 0x04; y >= 0; y--) {
    if (st->ram[SMBC_ENEMY_ID + y] == frenzy) {
      queue = 0x00u;
      break;
    }
  }
  st->ram[SMBC_ENEMY_FRENZY_QUEUE] = queue;
}

/* smb.asm: TreeLedge — start/middle/end of the tree ledge, with the
 * middle drawn continuously from the start of the level */
static void tree_ledge(CoreState *st, uint8_t x) {
  uint8_t row, len;
  get_lrg_obj_attrib(st, x, &row, &len);
  int8_t length = (int8_t)st->ram[SMBC_AREA_OBJECT_LENGTH + x];
  if (length == 0) {
    render_under_part(st, row, 0x18u, 0x00u); /* EndTreeL */
    return;
  }
  if (length > 0) {
    st->ram[SMBC_METATILE_BUFFER + row] = 0x17u; /* MidTreeL */
    render_under_part(st, (uint8_t)(row + 1u), 0x4cu, 0x0fu);
    return;
  }
  st->ram[SMBC_AREA_OBJECT_LENGTH + x] = len;
  if ((uint8_t)(st->ram[SMBC_CURRENT_PAGE_LOC] |
                st->ram[SMBC_CURRENT_COLUMN_POS]) == 0x00u) {
    st->ram[SMBC_METATILE_BUFFER + row] = 0x17u; /* start of level */
    render_under_part(st, (uint8_t)(row + 1u), 0x4cu, 0x0fu);
    return;
  }
  render_under_part(st, row, 0x16u, 0x00u); /* start of ledge */
}

/* smb.asm: MushroomLedge — the wide mushroom platform, with the stem
 * rendered only at the centre column */
static void mushroom_ledge(CoreState *st, uint8_t x) {
  uint8_t row, len;
  bool started = chk_lrg_obj_length(st, x, &row, &len);
  if (started) {
    st->ram[SMBC_MUSHROOM_LEDGE_HALF_LEN + x] =
        (uint8_t)(st->ram[SMBC_AREA_OBJECT_LENGTH + x] >> 1);
    render_under_part(st, row, 0x19u, 0x00u); /* start of mushroom */
    return;
  }
  uint8_t length = st->ram[SMBC_AREA_OBJECT_LENGTH + x];
  if (length == 0x00u) {
    render_under_part(st, row, 0x1bu, 0x00u); /* end of mushroom */
    return;
  }
  st->ram[SMBC_METATILE_BUFFER + row] = 0x1au; /* middle */
  if (length == st->ram[SMBC_MUSHROOM_LEDGE_HALF_LEN + x]) {
    st->ram[SMBC_METATILE_BUFFER + row + 1u] = 0x4fu; /* stem top */
    render_under_part(st, (uint8_t)(row + 2u), 0x50u, 0x0fu);
  }
}

/* smb.asm: BulletBillCannon — the cannon body plus a cannon data
 * entry at the ring buffer offset shared with whirlpools */
static void bullet_bill_cannon(CoreState *st, uint8_t x) {
  uint8_t row, len;
  get_lrg_obj_attrib(st, x, &row, &len);
  uint8_t bx = row;
  st->ram[SMBC_METATILE_BUFFER + bx] = 0x64u;
  bx++;
  int8_t y = (int8_t)(len - 1);
  if (y >= 0) {
    st->ram[SMBC_METATILE_BUFFER + bx] = 0x65u;
    bx++;
    y--;
    if (y >= 0) render_under_part(st, bx, 0x66u, (uint8_t)y);
  }
  uint8_t c = st->ram[SMBC_CANNON_OFFSET];
  st->ram[SMBC_CANNON_Y_POSITION + c] = get_area_obj_y_position(row);
  st->ram[SMBC_CANNON_PAGE_LOC + c] = st->ram[SMBC_CURRENT_PAGE_LOC];
  st->ram[SMBC_CANNON_X_POSITION + c] = get_area_obj_x_position(st);
  c++;
  if (c >= 0x06u) c = 0x00u;
  st->ram[SMBC_CANNON_OFFSET] = c;
}

/* smb.asm: AreaStyleObject — dispatch on the level object style */
static void area_style_object(CoreState *st, uint8_t x) {
  switch (st->ram[SMBC_AREA_STYLE]) {
    case 0x00u: tree_ledge(st, x); break;
    case 0x01u: mushroom_ledge(st, x); break;
    default: bullet_bill_cannon(st, x); break;
  }
}

/* smb.asm: PulleyRopeObject — left pulley when starting, rope in the
 * middle, right pulley at the end; always at the top of the screen */
static void pulley_rope_object(CoreState *st, uint8_t x) {
  bool started = chk_lrg_obj_length(st, x, NULL, NULL);
  uint8_t mt;
  if (started) mt = SMBC_PULLEY_ROPE_METATILES[0];
  else if (st->ram[SMBC_AREA_OBJECT_LENGTH + x] != 0x00u)
    mt = SMBC_PULLEY_ROPE_METATILES[1];
  else mt = SMBC_PULLEY_ROPE_METATILES[2];
  st->ram[SMBC_METATILE_BUFFER] = mt;
}

/* smb.asm: CastleObject — render the castle band by band from the
 * length counter; on the closing columns put a brick at the floor or
 * spawn the star flag */
static void castle_object(CoreState *st, uint8_t x) {
  uint8_t len;
  get_lrg_obj_attrib(st, x, NULL, &len);
  uint8_t start_row = len; /* the castle's low nybble is the start row */
  chk_lrg_obj_fixed_length(st, x, 0x04u);
  uint8_t y = st->ram[SMBC_AREA_OBJECT_LENGTH + x]; /* length as offset */
  uint8_t bx = start_row;
  uint8_t limit = 0x0bu;
  for (;;) {
    st->ram[SMBC_METATILE_BUFFER + bx] = SMBC_CASTLE_METATILES[y];
    bx++;
    if (limit != 0x00u) {
      y = (uint8_t)(y + 5u);
      limit--;
    }
    if (bx == 0x0bu) break;
  }
  if (st->ram[SMBC_CURRENT_PAGE_LOC] == 0x00u) return;
  uint8_t length = st->ram[SMBC_AREA_OBJECT_LENGTH + x];
  if (length == 0x01u) {
    st->ram[SMBC_METATILE_BUFFER + 10u] = 0x52u; /* PlayerStop */
    return;
  }
  if (start_row == 0x00u && length == 0x03u) {
    st->ram[SMBC_METATILE_BUFFER + 10u] = 0x52u;
    return;
  }
  if (length != 0x02u) return;
  uint8_t xpos = get_area_obj_x_position(st);
  int slot = find_empty_enemy_slot(st);
  if (slot < 0) slot = 0x05; /* the asm's unchecked slot index */
  st->ram[SMBC_ENEMY_X_POSITION + slot] = xpos;
  st->ram[SMBC_ENEMY_PAGE_LOC + slot] = st->ram[SMBC_CURRENT_PAGE_LOC];
  st->ram[SMBC_ENEMY_Y_HIGH_POS + slot] = 0x01u;
  st->ram[SMBC_ENEMY_FLAG + slot] = 0x01u;
  st->ram[SMBC_ENEMY_Y_POSITION + slot] = 0x90u;
  st->ram[SMBC_ENEMY_ID + slot] = SMBC_STAR_FLAG_OBJECT;
}

/* smb.asm: WaterPipe — a 1-column water pipe, two metatiles tall */
static void water_pipe(CoreState *st, uint8_t x) {
  uint8_t row;
  get_lrg_obj_attrib(st, x, &row, NULL);
  st->ram[SMBC_METATILE_BUFFER + row] = 0x6bu;
  st->ram[SMBC_METATILE_BUFFER + row + 1u] = 0x6cu;
}

/* smb.asm: RenderSidewaysPipe — the sideways part of an intro/exit
 * pipe, with the vertical shaft drawn while the horizontal length
 * calls for it. Returns true (the asm's carry) when no shaft was
 * drawn; the leftover horizontal length goes to horiz_out. */
static bool render_sideways_pipe(CoreState *st, uint8_t x, uint8_t vert_len,
                                 uint8_t *horiz_out) {
  uint8_t vert = (uint8_t)(vert_len - 2u);
  uint8_t horiz = st->ram[SMBC_AREA_OBJECT_LENGTH + x];
  uint8_t bx = (uint8_t)(vert + 1u);
  uint8_t shaft = SMBC_SIDE_PIPE_SHAFT_DATA[horiz];
  bool carry = true;
  if (shaft != 0x00u) {
    bx = render_under_part(st, 0x00u, shaft, vert);
    carry = false;
  }
  st->ram[SMBC_METATILE_BUFFER + bx] = SMBC_SIDE_PIPE_TOP_PART[horiz];
  st->ram[SMBC_METATILE_BUFFER + bx + 1u] = SMBC_SIDE_PIPE_BOTTOM_PART[horiz];
  *horiz_out = horiz;
  return carry;
}

/* smb.asm: IntroPipe — the sideways pipe the player walks out of,
 * ending in the top of a vertical pipe */
static void intro_pipe(CoreState *st, uint8_t x) {
  chk_lrg_obj_fixed_length(st, x, 0x03u);
  uint8_t horiz;
  if (render_sideways_pipe(st, x, 0x0au, &horiz)) return; /* NoBlankP */
  for (uint8_t bx = 0x00u; bx <= 0x06u; bx++) {
    st->ram[SMBC_METATILE_BUFFER + bx] = 0x00u;
  }
  st->ram[SMBC_METATILE_BUFFER + 0x07u] = SMBC_VERTICAL_PIPE_DATA[horiz];
}

/* smb.asm: ExitPipe — the sideways pipe the player descends into */
static void exit_pipe(CoreState *st, uint8_t x) {
  chk_lrg_obj_fixed_length(st, x, 0x03u);
  uint8_t len;
  get_lrg_obj_attrib(st, x, NULL, &len);
  uint8_t horiz;
  (void)render_sideways_pipe(st, x, len, &horiz);
}

/* smb.asm: VerticalPipe (+ GetPipeHeight) — warp and decoration
 * pipes; spawn a piranha plant on the first column outside 1-1 */
static void vertical_pipe(CoreState *st, uint8_t x, uint8_t id) {
  chk_lrg_obj_fixed_length(st, x, 0x01u);
  uint8_t row, low;
  get_lrg_obj_attrib(st, x, &row, &low);
  uint8_t height = (uint8_t)(low & 0x07u);
  uint8_t y = st->ram[SMBC_AREA_OBJECT_LENGTH + x]; /* length left over */
  if (id != 0x00u) y = (uint8_t)(y + 4u); /* warp pipes keep the null id */
  if ((uint8_t)(st->ram[SMBC_AREA_NUMBER] | st->ram[SMBC_WORLD_NUMBER]) !=
          0x00u &&
      st->ram[SMBC_AREA_OBJECT_LENGTH + x] != 0x00u) {
    int slot = find_empty_enemy_slot(st);
    if (slot >= 0) {
      uint16_t xpos = (uint16_t)(get_area_obj_x_position(st) + 0x08u);
      st->ram[SMBC_ENEMY_X_POSITION + slot] = (uint8_t)xpos;
      st->ram[SMBC_ENEMY_PAGE_LOC + slot] =
          (uint8_t)(st->ram[SMBC_CURRENT_PAGE_LOC] + (xpos >> 8));
      st->ram[SMBC_ENEMY_Y_HIGH_POS + slot] = 0x01u;
      st->ram[SMBC_ENEMY_FLAG + slot] = 0x01u;
      st->ram[SMBC_ENEMY_Y_POSITION + slot] = get_area_obj_y_position(row);
      st->ram[SMBC_ENEMY_ID + slot] = SMBC_PIRANHA_PLANT;
      init_piranha_plant(st, (uint8_t)slot);
    }
  }
  st->ram[SMBC_METATILE_BUFFER + row] = SMBC_VERTICAL_PIPE_DATA[y];
  st->ram[SMBC_METATILE_BUFFER + row + 1u] = SMBC_VERTICAL_PIPE_DATA[y + 2u];
  render_under_part(st, (uint8_t)(row + 1u), SMBC_VERTICAL_PIPE_DATA[y + 2u],
                    (uint8_t)(height - 1u));
}

/* smb.asm: Hole_Water — the water in a pit: waves on row 10, water
 * beneath */
static void hole_water(CoreState *st, uint8_t x) {
  chk_lrg_obj_length(st, x, NULL, NULL);
  st->ram[SMBC_METATILE_BUFFER + 10u] = 0x86u;
  render_under_part(st, 0x0bu, 0x87u, 0x01u);
}

/* smb.asm: QuestionBlockRow_High/Low — a row of coin question blocks
 * on the fourth or eighth row */
static void question_block_row(CoreState *st, uint8_t x, uint8_t row) {
  chk_lrg_obj_length(st, x, NULL, NULL);
  st->ram[SMBC_METATILE_BUFFER + row] = 0xc0u;
}

/* smb.asm: Bridge_High/Middle/Low — railing on the given row, bridge
 * proper one row beneath */
static void bridge(CoreState *st, uint8_t x, uint8_t row) {
  chk_lrg_obj_length(st, x, NULL, NULL);
  st->ram[SMBC_METATILE_BUFFER + row] = 0x0bu;
  render_under_part(st, (uint8_t)(row + 1u), 0x63u, 0x00u);
}

/* smb.asm: FlagBalls_Residual — the flag balls left over when a flag
 * is rendered as part of a level (no flagpole object) */
static void flag_balls_residual(CoreState *st, uint8_t x) {
  uint8_t len;
  get_lrg_obj_attrib(st, x, NULL, &len);
  render_under_part(st, 0x02u, 0x6du, len);
}

/* smb.asm: FlagpoleObject — the flagpole plus the flag itself, which
 * is written directly into the sixth enemy slot */
static void flagpole_object(CoreState *st) {
  st->ram[SMBC_METATILE_BUFFER] = 0x24u;
  render_under_part(st, 0x01u, 0x25u, 0x08u);
  st->ram[SMBC_METATILE_BUFFER + 10u] = 0x61u;
  uint16_t xpos = (uint16_t)(get_area_obj_x_position(st) - 0x08u);
  st->ram[SMBC_ENEMY_X_POSITION + 5u] = (uint8_t)xpos;
  st->ram[SMBC_ENEMY_PAGE_LOC + 5u] =
      (uint8_t)(st->ram[SMBC_CURRENT_PAGE_LOC] - ((xpos >> 8) & 0x01u));
  st->ram[SMBC_ENEMY_Y_POSITION + 5u] = 0x30u;
  st->ram[SMBC_FLAGPOLE_FNUM_Y_POS] = 0xb0u;
  st->ram[SMBC_ENEMY_ID + 5u] = SMBC_FLAGPOLE_FLAG_OBJECT;
  st->ram[SMBC_ENEMY_FLAG + 5u]++;
}

/* smb.asm: EndlessRope — a rope spanning the full screen height */
static void endless_rope(CoreState *st) {
  render_under_part(st, 0x00u, 0x40u, 0x0fu);
}

/* smb.asm: BalancePlatRope — blank the rope column, then draw the
 * rope for the platform's vertical length */
static void balance_plat_rope(CoreState *st, uint8_t x) {
  render_under_part(st, 0x01u, 0x44u, 0x0fu);
  uint8_t len;
  get_lrg_obj_attrib(st, x, NULL, &len);
  render_under_part(st, 0x01u, 0x40u, len);
}

/* smb.asm: GetRow — a single-row object with the given metatile */
static void get_row(CoreState *st, uint8_t x, uint8_t mt) {
  uint8_t row;
  chk_lrg_obj_length(st, x, &row, NULL);
  render_under_part(st, row, mt, 0x00u);
}

/* smb.asm: GetRow2 — a single-column object with the given metatile */
static void get_row2(CoreState *st, uint8_t x, uint8_t mt) {
  uint8_t row, len;
  get_lrg_obj_attrib(st, x, &row, &len);
  render_under_part(st, row, mt, len);
}

/* smb.asm: RowOfCoins */
static void row_of_coins(CoreState *st, uint8_t x) {
  get_row(st, x, SMBC_COIN_METATILE_DATA[st->ram[SMBC_AREA_TYPE]]);
}

/* smb.asm: RowOfBricks — the cloud-type override substitutes the
 * fifth brick metatile */
static void row_of_bricks(CoreState *st, uint8_t x) {
  uint8_t type = st->ram[SMBC_AREA_TYPE];
  if (st->ram[SMBC_CLOUD_TYPE_OVERRIDE] != 0x00u) type = 0x04u;
  get_row(st, x, SMBC_BRICK_METATILES[type]);
}

/* smb.asm: RowOfSolidBlocks */
static void row_of_solid_blocks(CoreState *st, uint8_t x) {
  get_row(st, x, SMBC_SOLID_BLOCK_METATILES[st->ram[SMBC_AREA_TYPE]]);
}

/* smb.asm: ColumnOfBricks — no cloud override for columns */
static void column_of_bricks(CoreState *st, uint8_t x) {
  get_row2(st, x, SMBC_BRICK_METATILES[st->ram[SMBC_AREA_TYPE]]);
}

/* smb.asm: ColumnOfSolidBlocks */
static void column_of_solid_blocks(CoreState *st, uint8_t x) {
  get_row2(st, x, SMBC_SOLID_BLOCK_METATILES[st->ram[SMBC_AREA_TYPE]]);
}

/* smb.asm: ChainObj (+ CastleBridgeObj/AxeObj/EmptyBlock) — the "C
 * objects", a single metatile on a fixed row selected by id - 2 */
static void chain_obj(CoreState *st, uint8_t id) {
  render_under_part(st, SMBC_C_OBJECT_ROW[id - 0x02u],
                    SMBC_C_OBJECT_METATILE[id - 0x02u], 0x00u);
}

static void castle_bridge_obj(CoreState *st, uint8_t x, uint8_t id) {
  chk_lrg_obj_fixed_length(st, x, 0x0cu);
  chain_obj(st, id);
}

static void axe_obj(CoreState *st, uint8_t id) {
  st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] = 0x08u;
  chain_obj(st, id);
}

/* smb.asm: EmptyBlock */
static void empty_block(CoreState *st, uint8_t x) {
  uint8_t row;
  get_lrg_obj_attrib(st, x, &row, NULL);
  render_under_part(st, row, 0xc4u, 0x00u);
}

/* smb.asm: StaircaseObject — a step per column, walking down the
 * staircase control counter */
static void staircase_object(CoreState *st, uint8_t x) {
  if (chk_lrg_obj_length(st, x, NULL, NULL))
    st->ram[SMBC_STAIRCASE_CONTROL] = 0x09u;
  st->ram[SMBC_STAIRCASE_CONTROL]--;
  uint8_t sc = st->ram[SMBC_STAIRCASE_CONTROL];
  render_under_part(st, SMBC_STAIRCASE_ROW_DATA[sc], 0x61u,
                    SMBC_STAIRCASE_HEIGHT_DATA[sc]);
}

/* smb.asm: Jumpspring — write the jumpspring into an enemy slot and
 * draw its two metatiles */
static void jumpspring(CoreState *st, uint8_t x) {
  uint8_t row;
  get_lrg_obj_attrib(st, x, &row, NULL);
  int slot = find_empty_enemy_slot(st);
  if (slot < 0) slot = 0x05; /* the asm's unchecked slot index */
  st->ram[SMBC_ENEMY_X_POSITION + slot] = get_area_obj_x_position(st);
  st->ram[SMBC_ENEMY_PAGE_LOC + slot] = st->ram[SMBC_CURRENT_PAGE_LOC];
  uint8_t ypos = get_area_obj_y_position(row);
  st->ram[SMBC_ENEMY_Y_POSITION + slot] = ypos;
  st->ram[SMBC_JUMPSPRING_FIXED_Y_POS + slot] = ypos;
  st->ram[SMBC_ENEMY_ID + slot] = SMBC_JUMPSPRING_OBJECT;
  st->ram[SMBC_ENEMY_Y_HIGH_POS + slot] = 0x01u;
  st->ram[SMBC_ENEMY_FLAG + slot]++;
  st->ram[SMBC_METATILE_BUFFER + row] = 0x67u;
  st->ram[SMBC_METATILE_BUFFER + row + 1u] = 0x68u;
}

/* smb.asm: DrawQBlk — a question block or brick-with-item, metatile
 * selected through the shared table */
static void draw_qblk(CoreState *st, uint8_t x, uint8_t idx) {
  uint8_t mt = SMBC_BRICK_Q_BLOCK_METATILES[idx];
  uint8_t row;
  get_lrg_obj_attrib(st, x, &row, NULL);
  render_under_part(st, row, mt, 0x00u);
}

/* smb.asm: QuestionBlock */
static void question_block(CoreState *st, uint8_t x, uint8_t id) {
  draw_qblk(st, x, id);
}

/* smb.asm: BrickWithItem — bricks with items use an area-type adder:
 * ground types index the table from 0, everything else from 5 */
static void brick_with_item(CoreState *st, uint8_t x, uint8_t id) {
  uint8_t adder = (st->ram[SMBC_AREA_TYPE] == 0x01u) ? 0x00u : 0x05u;
  draw_qblk(st, x, (uint8_t)(adder + id));
}

/* smb.asm: BrickWithCoins — reset the multi-coin timer, then the
 * shared brick-with-item tail */
static void brick_with_coins(CoreState *st, uint8_t x, uint8_t id) {
  st->ram[SMBC_BRICK_COIN_TIMER_FLAG] = 0x00u;
  brick_with_item(st, x, id);
}

/* smb.asm: Hidden1UpBlock — only rendered once the hidden 1-up flag
 * has been set by a prior block */
static void hidden_1up_block(CoreState *st, uint8_t x, uint8_t id) {
  if (st->ram[SMBC_HIDDEN_1UP_FLAG] == 0x00u) return;
  st->ram[SMBC_HIDDEN_1UP_FLAG] = 0x00u;
  brick_with_item(st, x, id);
}

/* smb.asm: Hole_Empty — the pit; in water areas a whirlpool data
 * entry is written on the first column */
static void hole_empty(CoreState *st, uint8_t x) {
  uint8_t len;
  bool started = chk_lrg_obj_length(st, x, NULL, &len);
  if (started && st->ram[SMBC_AREA_TYPE] == 0x00u) {
    uint8_t w = st->ram[SMBC_WHIRLPOOL_OFFSET];
    uint16_t xpos = (uint16_t)(get_area_obj_x_position(st) - 0x10u);
    st->ram[SMBC_WHIRLPOOL_LEFT_EXTENT + w] = (uint8_t)xpos;
    st->ram[SMBC_WHIRLPOOL_PAGE_LOC + w] =
        (uint8_t)(st->ram[SMBC_CURRENT_PAGE_LOC] - ((xpos >> 8) & 0x01u));
    st->ram[SMBC_WHIRLPOOL_LENGTH + w] = (uint8_t)((len + 2u) << 4);
    w++;
    if (w >= 0x05u) w = 0x00u;
    st->ram[SMBC_WHIRLPOOL_OFFSET] = w;
  }
  render_under_part(st, 0x08u, SMBC_HOLE_METATILES[st->ram[SMBC_AREA_TYPE]],
                    0x0fu);
}

/* smb.asm: JumpEngine table for area objects — 47 entries in the
 * order the decoder adds its row offset: 8 large objects, 8 row-12,
 * 6 row-15, 12 small, 12 row-13, 1 row-14 */
static void run_object(CoreState *st, uint8_t x, uint8_t index, uint8_t id) {
  switch (index) {
    case 0x00u: vertical_pipe(st, x, id); break;
    case 0x01u: area_style_object(st, x); break;
    case 0x02u: row_of_bricks(st, x); break;
    case 0x03u: row_of_solid_blocks(st, x); break;
    case 0x04u: row_of_coins(st, x); break;
    case 0x05u: column_of_bricks(st, x); break;
    case 0x06u: column_of_solid_blocks(st, x); break;
    case 0x07u: vertical_pipe(st, x, id); break;
    case 0x08u: hole_empty(st, x); break;
    case 0x09u: pulley_rope_object(st, x); break;
    case 0x0au: bridge(st, x, 0x06u); break;
    case 0x0bu: bridge(st, x, 0x07u); break;
    case 0x0cu: bridge(st, x, 0x09u); break;
    case 0x0du: hole_water(st, x); break;
    case 0x0eu: question_block_row(st, x, 0x03u); break;
    case 0x0fu: question_block_row(st, x, 0x07u); break;
    case 0x10u: endless_rope(st); break;
    case 0x11u: balance_plat_rope(st, x); break;
    case 0x12u: castle_object(st, x); break;
    case 0x13u: staircase_object(st, x); break;
    case 0x14u: exit_pipe(st, x); break;
    case 0x15u: flag_balls_residual(st, x); break;
    case 0x16u: question_block(st, x, id); break;
    case 0x17u: question_block(st, x, id); break;
    case 0x18u: question_block(st, x, id); break;
    case 0x19u: hidden_1up_block(st, x, id); break;
    case 0x1au: brick_with_item(st, x, id); break;
    case 0x1bu: brick_with_item(st, x, id); break;
    case 0x1cu: brick_with_item(st, x, id); break;
    case 0x1du: brick_with_coins(st, x, id); break;
    case 0x1eu: brick_with_item(st, x, id); break;
    case 0x1fu: water_pipe(st, x); break;
    case 0x20u: empty_block(st, x); break;
    case 0x21u: jumpspring(st, x); break;
    case 0x22u: intro_pipe(st, x); break;
    case 0x23u: flagpole_object(st); break;
    case 0x24u: axe_obj(st, id); break;
    case 0x25u: chain_obj(st, id); break;
    case 0x26u: castle_bridge_obj(st, x, id); break;
    case 0x27u: scroll_lock_object_warp(st); break;
    case 0x28u: scroll_lock_object(st); break;
    case 0x29u: scroll_lock_object(st); break;
    case 0x2au: area_frenzy(st, id); break;
    case 0x2bu: area_frenzy(st, id); break;
    case 0x2cu: area_frenzy(st, id); break;
    case 0x2du: break; /* smb.asm: LoopCmdE — the loop command leaves */
    case 0x2eu: alter_area_attributes(st, x); break;
    default: break;
  }
}

/* smb.asm: DecodeAreaData — derive the object id and row adder from
 * the buffered (or incoming) level object, and when the object starts
 * on the current page and column, buffer its offset and run it */
static void decode_area_data(CoreState *st, uint8_t x) {
  uint8_t y = st->ram[SMBC_AREA_DATA_OFFSET];
  if ((int8_t)st->ram[SMBC_AREA_OBJECT_LENGTH + x] >= 0)
    y = st->ram[SMBC_AREA_OBJ_OFFSET_BUFFER + x];
  uint8_t b1 = st->area_data[y];
  if (b1 == 0xfdu) return;
  uint8_t row = (uint8_t)(b1 & 0x0fu);
  uint8_t adder; /* the $07 row offset into the jump table */
  uint8_t id;    /* the $00 object identifier */
  if (row == 0x0fu) adder = 0x10u;
  else if (row == 0x0cu) adder = 0x08u;
  else adder = 0x00u;
  if (row == 0x0eu) {
    adder = 0x00u;
    id = 0x2eu;
  } else if (row == 0x0du) {
    adder = 0x22u;
    uint8_t b2 = st->area_data[y + 1u];
    if ((b2 & 0x40u) == 0x00u) return; /* page control, handled earlier */
    uint8_t v = (uint8_t)(b2 & 0x7fu);
    if (v == 0x4bu) st->ram[SMBC_LOOP_COMMAND]++;
    id = (uint8_t)(v & 0x3fu);
  } else if (row < 0x0cu) {
    uint8_t b2 = st->area_data[y + 1u];
    uint8_t v = (uint8_t)(b2 & 0x70u);
    if (v == 0x00u) {
      adder = 0x16u; /* small object */
      id = (uint8_t)(b2 & 0x0fu);
    } else {
      if (v == 0x70u && (b2 & 0x08u) != 0x00u) v = 0x00u; /* warp pipe */
      id = (uint8_t)(v >> 4);
    }
  } else { /* rows 12-15 */
    id = (uint8_t)((st->area_data[y + 1u] & 0x70u) >> 4);
  }
  if ((int8_t)st->ram[SMBC_AREA_OBJECT_LENGTH + x] >= 0) {
    run_object(st, x, (uint8_t)(id + adder), id); /* RunAObj */
    return;
  }
  if (st->ram[SMBC_AREA_OBJECT_PAGE_LOC] == st->ram[SMBC_CURRENT_PAGE_LOC]) {
    if (st->ram[SMBC_BACKLOADING_FLAG] != 0x00u) { /* InitRear */
      st->ram[SMBC_BACKLOADING_FLAG] = 0x00u;
      st->ram[SMBC_BEHIND_AREA_PARSER_FLAG] = 0x00u;
      st->ram[SMBC_OBJECT_OFFSET] = 0x00u;
      return;
    }
    if ((uint8_t)(b1 >> 4) != st->ram[SMBC_CURRENT_COLUMN_POS]) return;
  } else {
    if (row != 0x0eu) return;
    if (st->ram[SMBC_BACKLOADING_FLAG] == 0x00u) return;
  }
  st->ram[SMBC_AREA_OBJ_OFFSET_BUFFER + x] = st->ram[SMBC_AREA_DATA_OFFSET];
  inc_area_obj_offset(st); /* StrAObj */
  run_object(st, x, (uint8_t)(id + adder), id);
}

/* smb.asm: CheckRear — true (with the behind-renderer flag set) when
 * the level object's page sits behind the renderer's */
static bool check_rear(const CoreState *st) {
  if (st->ram[SMBC_AREA_OBJECT_PAGE_LOC] >= st->ram[SMBC_CURRENT_PAGE_LOC])
    return false;
  return true;
}

/* smb.asm: ProcessAreaData — the area object parser. Runs the three
 * slot object buffer from the top, decoding whatever the pointer sits
 * on, advancing past objects behind or already rendered, and looping
 * the whole pass while the behind-renderer or backloading flags call
 * for more data. */
static void process_area_data(CoreState *st) {
  /* the hardware only runs the parser after GetAreaDataAddrs has set
   * the pointer; host-side, leave when it has not been set up */
  if (st->area_data == NULL) return;
  for (;;) {
    int x = 0x02;
    while (x >= 0) { /* ProcADLoop */
      st->ram[SMBC_OBJECT_OFFSET] = (uint8_t)x;
      st->ram[SMBC_BEHIND_AREA_PARSER_FLAG] = 0x00u;
      uint8_t y = st->ram[SMBC_AREA_DATA_OFFSET];
      uint8_t b1 = st->area_data[y];
      bool advance = false;
      if (b1 != 0xfdu &&
          (int8_t)st->ram[SMBC_AREA_OBJECT_LENGTH + x] < 0) {
        /* the page select bit is d7 of the second byte */
        if ((st->area_data[y + 1u] & 0x80u) != 0x00u &&
            st->ram[SMBC_AREA_OBJECT_PAGE_SEL] == 0x00u) {
          st->ram[SMBC_AREA_OBJECT_PAGE_SEL]++;
          st->ram[SMBC_AREA_OBJECT_PAGE_LOC]++;
        }
        uint8_t row = (uint8_t)(b1 & 0x0fu);
        if (row == 0x0du) {
          uint8_t b2 = st->area_data[y + 1u];
          if ((b2 & 0x40u) == 0x00u &&
              st->ram[SMBC_AREA_OBJECT_PAGE_SEL] == 0x00u) {
            /* page control object — store the new page and move on */
            st->ram[SMBC_AREA_OBJECT_PAGE_LOC] = (uint8_t)(b2 & 0x1fu);
            st->ram[SMBC_AREA_OBJECT_PAGE_SEL]++;
            advance = true;
          } else if (check_rear(st)) {
            st->ram[SMBC_BEHIND_AREA_PARSER_FLAG]++;
            advance = true;
          } else {
            decode_area_data(st, (uint8_t)x);
          }
        } else if (row == 0x0eu &&
                   st->ram[SMBC_BACKLOADING_FLAG] != 0x00u) {
          decode_area_data(st, (uint8_t)x);
        } else if (check_rear(st)) {
          st->ram[SMBC_BEHIND_AREA_PARSER_FLAG]++;
          advance = true;
        } else {
          decode_area_data(st, (uint8_t)x);
        }
      } else {
        decode_area_data(st, (uint8_t)x);
      }
      if (advance) inc_area_obj_offset(st); /* NextAObj */
      x = st->ram[SMBC_OBJECT_OFFSET];      /* ChkLength */
      if ((int8_t)st->ram[SMBC_AREA_OBJECT_LENGTH + x] >= 0)
        st->ram[SMBC_AREA_OBJECT_LENGTH + x]--;
      x--;
    }
    if (st->ram[SMBC_BEHIND_AREA_PARSER_FLAG] == 0x00u &&
        st->ram[SMBC_BACKLOADING_FLAG] == 0x00u)
      break;
  }
}

/* smb.asm: GetBlockBufferAddr — the block buffer address for a column
 * position: buffer 1 for columns $00-$0f, buffer 2 for $10-$1f */
static uint16_t get_block_buffer_addr(uint8_t col_pos) {
  return (uint16_t)(SMBC_BLOCK_BUFFER_1 + (col_pos & 0x10u) / 0x10u * 0xd0u +
                    (col_pos & 0x0fu));
}

/* smb.asm: RenderSceneryTerrain — clear the metatile buffer, lay in the
 * background scenery (chosen every third page), the foreground scenery,
 * and the terrain ceiling/floor from the render bits; then the tail end
 * runs the object decoder and stores the column into the block buffer,
 * dropping metatiles under their attribute bound. */
static void render_scenery_terrain(CoreState *st) {
  for (uint8_t x = 0; x <= 0x0cu; x++) { /* ClrMTBuf */
    st->ram[SMBC_METATILE_BUFFER + x] = 0x00u;
  }
  uint8_t bg = st->ram[SMBC_BACKGROUND_SCENERY];
  if (bg != 0x00u) {
    uint8_t page = st->ram[SMBC_CURRENT_PAGE_LOC];
    while (page >= 0x03u) page = (uint8_t)(page - 0x03u); /* ThirdP */
    uint8_t x = (uint8_t)((uint8_t)(page << 4) +
                          SMBC_B_SCENE_DATA_OFFSETS[bg - 1u] +
                          st->ram[SMBC_CURRENT_COLUMN_POS]);
    uint8_t sd = SMBC_BACK_SCENERY_DATA[x];
    if (sd != 0x00u) {
      uint8_t low = (uint8_t)((sd & 0x0fu) - 1u); /* shapes are 1-based */
      uint8_t sx = (uint8_t)(low * 3u);
      uint8_t y = (uint8_t)(sd >> 4); /* the buffer height */
      uint8_t count = 0x03u;
      for (;;) { /* SceLoop1 */
        st->ram[SMBC_METATILE_BUFFER + y] = SMBC_BACK_SCENERY_METATILES[sx];
        sx++;
        y++;
        if (y == 0x0bu) break;
        if (--count == 0x00u) break;
      }
    }
  }
  uint8_t fg = st->ram[SMBC_FOREGROUND_SCENERY];
  if (fg != 0x00u) {
    uint8_t y = SMBC_F_SCENE_DATA_OFFSETS[fg - 1u];
    for (uint8_t x = 0x00u; x < 0x0du; x++) { /* SceLoop2 */
      uint8_t d = SMBC_FORE_SCENERY_DATA[y];
      y++;
      if (d != 0x00u) st->ram[SMBC_METATILE_BUFFER + x] = d;
    }
  }
  /* RendTerr: the terrain metatile — castle wall in world 8's water */
  uint8_t terrain;
  uint8_t area_type = st->ram[SMBC_AREA_TYPE];
  if (area_type == 0x00u && st->ram[SMBC_WORLD_NUMBER] == SMBC_WORLD8) {
    terrain = 0x62u;
  } else {
    terrain = SMBC_TERRAIN_METATILES[area_type];
  }
  if (st->ram[SMBC_CLOUD_TYPE_OVERRIDE] != 0x00u) {
    terrain = 0x88u; /* the cloud block */
  }
  uint8_t x = 0x00u;
  uint8_t y = (uint8_t)(st->ram[SMBC_TERRAIN_CONTROL] * 2u);
  for (;;) { /* TerrLoop: the ceiling byte, then the floor byte */
    uint8_t bits = SMBC_TERRAIN_RENDER_BITS[y];
    y++;
    if (st->ram[SMBC_CLOUD_TYPE_OVERRIDE] != 0x00u && x != 0x00u) {
      bits &= 0x08u; /* clouds only keep the d3 floor edge */
    }
    for (uint8_t by = 0x00u; by < 0x08u; by++) { /* TerrBChk */
      if ((SMBC_BITMASKS[by] & bits) != 0x00u) {
        st->ram[SMBC_METATILE_BUFFER + x] = terrain;
      }
      x++;
      if (x == 0x0du) break;
      if (area_type == 0x02u && x == 0x0bu) {
        terrain = 0x54u; /* underground: ground level at the bottom */
      }
    }
    if (x == 0x0du) break; /* RendBBuf */
  }
  process_area_data(st); /* RendBBuf */
  uint16_t addr = get_block_buffer_addr(st->ram[SMBC_BLOCK_BUFFER_COLUMN_POS]);
  uint8_t by = 0x00u;
  for (x = 0x00u; x < 0x0du; x++) { /* ChkMTLow */
    uint8_t mt = st->ram[SMBC_METATILE_BUFFER + x];
    if (mt < SMBC_BLOCK_BUFF_LOW_BOUNDS[(mt & 0xc0u) >> 6]) {
      mt = 0x00u;
    }
    st->ram[addr + by] = mt;
    by = (uint8_t)(by + 0x10u);
  }
}

/* smb.asm: AreaParserCore — with the backloading flag set the decoder
 * runs once to skip ahead, then the column renders as usual (the
 * original falls through; there is no return between) */
static void area_parser_core(CoreState *st) {
  if (st->ram[SMBC_BACKLOADING_FLAG] != 0x00u) {
    process_area_data(st);
  }
  render_scenery_terrain(st);
}

/* smb.asm: IncrementColumnPos */
static void increment_column_pos(CoreState *st) {
  uint8_t col = (uint8_t)(st->ram[SMBC_CURRENT_COLUMN_POS] + 1u);
  if ((col & 0x0fu) == 0x00u) { /* a wrapped column is a new page */
    col = 0x00u;
    st->ram[SMBC_CURRENT_PAGE_LOC]++;
  }
  st->ram[SMBC_CURRENT_COLUMN_POS] = col;
  st->ram[SMBC_BLOCK_BUFFER_COLUMN_POS] =
      (uint8_t)(st->ram[SMBC_BLOCK_BUFFER_COLUMN_POS] + 1u) & 0x1fu;
}

/* smb.asm: RenderAreaGraphics — draw one column of metatiles from the
 * metatile buffer into VRAM buffer 2 as 26 tile bytes, OR-ing each
 * metatile's attribute bits into the attribute buffer. The two top bits
 * of a metatile number select its palette's attribute bits; the rest is
 * the metatile index, four bytes per metatile (UL, LL, UR, LR), and the
 * task number's parity picks the left or right half. */
static uint32_t render_area_graphics(CoreState *st) {
  uint32_t cycles = 48u; /* prologue up to the row loop */
  uint8_t col_lsb = st->ram[SMBC_CURRENT_COLUMN_POS] & 0x01u; /* $05 */
  uint8_t buf_off = st->ram[SMBC_VRAM_BUFFER2_OFFSET];        /* $00 */
  st->ram[SMBC_VRAM_BUFFER2 + buf_off] = st->ram[SMBC_CURRENT_NT_ADDR_HIGH];
  st->ram[SMBC_VRAM_BUFFER2 + buf_off + 1] = st->ram[SMBC_CURRENT_NT_ADDR_LOW];
  st->ram[SMBC_VRAM_BUFFER2 + buf_off + 2] = 0x9au; /* 26 bytes, +32 stride */
  uint8_t attr_row = 0x00u; /* $04 */
  /* ((task & 1) ^ 1) << 1: the metatile half this pass draws */
  uint8_t half =
      (uint8_t)(((st->ram[SMBC_AREA_PARSER_TASK_NUM] & 0x01u) ^ 0x01u) << 1);
  for (uint8_t row = 0x00u; row < 0x0du; row++) { /* DrawMTLoop */
    uint8_t mt = st->ram[SMBC_METATILE_BUFFER + row];
    uint8_t attr = mt & 0xc0u; /* $03: attribute table bits */
    /* asl/rol/rol: the attribute bits, moved to d1-d0 */
    uint8_t palette = (uint8_t)((mt & 0xc0u) >> 6);
    const uint8_t *table = SMBC_METATILE_GRAPHICS[palette];
    uint8_t tile_off = (uint8_t)(half + (uint8_t)(mt << 2)); /* asl asl */
    /* lda ($06),y takes a page-cross cycle when the table's low byte
     * plus Y carries out of it */
    uint16_t table_end = (uint16_t)kPaletteTableLow[palette] + tile_off;
    cycles += (table_end > 0xffu) + (table_end + 1u > 0xffu);
    st->ram[SMBC_VRAM_BUFFER2 + buf_off + 3] = table[tile_off];
    st->ram[SMBC_VRAM_BUFFER2 + buf_off + 4] = table[(uint8_t)(tile_off + 1u)];
    bool odd_row = (row & 0x01u) != 0x00u;
    if (col_lsb == 0x00u) {
      cycles += odd_row ? 23u : 25u; /* left: lsr, bcs, then rol x3 or lsr x2 */
    } else {
      cycles += odd_row ? 13u : 30u; /* right: bne taken, bcs, then lsr x4 */
    }
    cycles += 120u                  /* the fixed body of a row */
              + (col_lsb != 0x00u ? 3u : 2u) /* bne */
              + (row == 0x0cu ? 2u : 4u);    /* bcc DrawMTLoop */
    uint8_t next = 0x00u; /* NextMTRow: the row bump takes effect after
                           * SetAttrib — the asm's y is loaded from $04
                           * before the inc, so the OR always lands in the
                           * pre-increment cell */
    if (col_lsb == 0x00u) { /* left attribute square */
      if ((row & 0x01u) == 0x00u) {
        attr >>= 6; /* upper left: three rol through carry, d1-d0 */
      } else {
        attr >>= 2; /* LLeft: lower left square, d5-d4 */
        next = 0x01u;
      }
    } else if ((row & 0x01u) == 0x00u) {
      attr >>= 4; /* RightCheck: upper right square, d3-d2 */
    } else {
      next = 0x01u; /* NextMTRow: lower right keeps d7-d6 */
    }
    st->ram[SMBC_ATTRIBUTE_BUFFER + attr_row] |= attr; /* SetAttrib */
    attr_row = (uint8_t)(attr_row + next);
    buf_off = (uint8_t)(buf_off + 2u);
  }
  buf_off = (uint8_t)(buf_off + 3u); /* past the address and length */
  st->ram[SMBC_VRAM_BUFFER2 + buf_off] = 0x00u; /* null terminator */
  st->ram[SMBC_VRAM_BUFFER2_OFFSET] = buf_off;
  /* advance the name table address one column, wrapping under the status
   * bar and into the other name table */
  uint8_t low = (uint8_t)(st->ram[SMBC_CURRENT_NT_ADDR_LOW] + 1u);
  st->ram[SMBC_CURRENT_NT_ADDR_LOW] = low;
  cycles += 50u; /* the tail to SetVRAMCtrl, its rts included */
  if ((low & 0x1fu) == 0x00u) {
    st->ram[SMBC_CURRENT_NT_ADDR_LOW] = 0x80u;
    st->ram[SMBC_CURRENT_NT_ADDR_HIGH] ^= 0x04u;
    cycles += 15u; /* the wrap's extra stores, and the bne not taken */
  }
  st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] = 0x06u; /* SetVRAMCtrl */
  return cycles;
}

/* smb.asm: RenderAttributeTables — flush the attribute buffer into VRAM
 * buffer 2, one byte per attribute cell, and clear the buffer behind it */
static void render_attribute_tables(CoreState *st) {
  uint8_t low = st->ram[SMBC_CURRENT_NT_ADDR_LOW];
  /* sec; sbc #$04 — the borrow flags a name-table boundary crossing */
  bool borrow = (uint8_t)(low & 0x1fu) < 0x04u;
  low = (uint8_t)((low & 0x1fu) - 0x04u) & 0x1fu;
  uint8_t high = st->ram[SMBC_CURRENT_NT_ADDR_HIGH];
  if (borrow) {
    high ^= 0x04u; /* crossed into the other name table's half */
  }
  high = (uint8_t)((high & 0x04u) | 0x23u);
  /* low / 4 + $c0 selects the attribute row; the two lsrs leave the old
   * bit 1 in the carry the adc consumes */
  low = (uint8_t)((low >> 2) + 0xc0u + ((low >> 1) & 0x01u));
  uint8_t y = st->ram[SMBC_VRAM_BUFFER2_OFFSET];
  for (uint8_t x = 0; x < 0x07u; x++) { /* AttribLoop */
    st->ram[SMBC_VRAM_BUFFER2 + y] = high;
    low = (uint8_t)(low + 0x08u); /* start below the status bar */
    st->ram[SMBC_VRAM_BUFFER2 + y + 1] = low;
    st->ram[SMBC_VRAM_BUFFER2 + y + 2] = 0x01u; /* one byte per write */
    st->ram[SMBC_VRAM_BUFFER2 + y + 3] = st->ram[SMBC_ATTRIBUTE_BUFFER + x];
    st->ram[SMBC_ATTRIBUTE_BUFFER + x] = 0x00u; /* clear behind */
    y = (uint8_t)(y + 4u);
  }
  st->ram[SMBC_VRAM_BUFFER2 + y] = 0x00u; /* null terminator */
  st->ram[SMBC_VRAM_BUFFER2_OFFSET] = y;
  st->ram[SMBC_VRAM_BUFFER_ADDR_CTRL] = 0x06u; /* SetVRAMCtrl */
}

static uint32_t run_task(CoreState *st, uint8_t task) {
  switch (task) { /* smb.asm: AreaParserTasks, jump table order */
    case 0: increment_column_pos(st); return 0;
    case 1: return render_area_graphics(st);
    case 2: return render_area_graphics(st);
    case 3: area_parser_core(st); return 0;
    case 4: increment_column_pos(st); return 0;
    case 5: return render_area_graphics(st);
    case 6: return render_area_graphics(st);
    default: area_parser_core(st); return 0;
  }
}

uint32_t area_parser_task_handler(CoreState *st) {
  if (st->ram[SMBC_AREA_PARSER_TASK_NUM] == 0x00u) {
    st->ram[SMBC_AREA_PARSER_TASK_NUM] = 0x08u; /* eight tasks by default */
  }
  uint32_t cycles =
      run_task(st, (uint8_t)(st->ram[SMBC_AREA_PARSER_TASK_NUM] - 1u));
  st->ram[SMBC_AREA_PARSER_TASK_NUM]--;
  if (st->ram[SMBC_AREA_PARSER_TASK_NUM] == 0x00u) {
    render_attribute_tables(st); /* the cycle flushes its attributes */
  }
  return cycles;
}

/* core/snapshot.c — see core/snapshot.h for the contract. */
#include "snapshot.h"

#include <string.h>

#define SMBC_SNAPSHOT_MAGIC "SMBCSNAP"
#define SMBC_SNAPSHOT_HEADER 12u /* magic + version + reserved */
#define SMBC_SCROLL_POINT_SIZE 5u /* u16 scanline + x + y + nt */

/* Payload, in order. Keep this list in step with smbc_snapshot_write and
 * smbc_snapshot_read: it is the same set of fields smbc_state_hash covers. */
#define SMBC_SNAPSHOT_PAYLOAD                                              \
  (SMBC_RAM_SIZE                                                           \
   + 3u + 3u                                  /* joypads: buttons, shift, \
                                               * strobe */                 \
   + 3u + 2u + 0x4000u + 2u + 1u + 0x100u     /* PPU registers and VRAM,  \
                                               * scroll, read buffer, OAM */ \
   + 3u + 4u + 1u + 1u + SMBC_PPU_SCROLL_LOG * SMBC_SCROLL_POINT_SIZE /* \
                                               * latch, flags, clock, \
                                               * t_nt, scroll log */ \
   + SMBC_APU_REGISTER_COUNT + 1u             /* APU image + $4015 status */ \
   + 4u + 1u + SMBC_PPU_SCROLL_LOG * SMBC_SCROLL_POINT_SIZE) /* NMI overrun, \
                                               * overrun scroll writes */

size_t smbc_snapshot_size(void) {
  return SMBC_SNAPSHOT_HEADER + SMBC_SNAPSHOT_PAYLOAD;
}

/* ------------------------------------------------------------ cursor */

typedef struct {
  uint8_t *out;   /* write side */
  const uint8_t *in; /* read side */
  size_t pos;
} Cursor;

static void put8(Cursor *c, uint8_t v) { c->out[c->pos++] = v; }

static void put16(Cursor *c, uint16_t v) {
  put8(c, (uint8_t)v);
  put8(c, (uint8_t)(v >> 8));
}

static void put32(Cursor *c, uint32_t v) {
  put16(c, (uint16_t)v);
  put16(c, (uint16_t)(v >> 16));
}

static void put_bytes(Cursor *c, const void *data, size_t len) {
  memcpy(c->out + c->pos, data, len);
  c->pos += len;
}

static uint8_t get8(Cursor *c) { return c->in[c->pos++]; }

static uint16_t get16(Cursor *c) {
  uint16_t lo = get8(c);
  uint16_t hi = get8(c);
  return (uint16_t)(lo | (hi << 8));
}

static uint32_t get32(Cursor *c) {
  uint32_t lo = get16(c);
  uint32_t hi = get16(c);
  return lo | (hi << 16);
}

static void get_bytes(Cursor *c, void *data, size_t len) {
  memcpy(data, c->in + c->pos, len);
  c->pos += len;
}

/* ----------------------------------------------------------- payload */

static void write_payload(Cursor *c, const Smbc *smbc) {
  put_bytes(c, smbc->state.ram, SMBC_RAM_SIZE);

  const Joypad *pads[2] = {&smbc->joypad1, &smbc->joypad2};
  for (int i = 0; i < 2; i++) {
    put8(c, pads[i]->buttons);
    put8(c, pads[i]->shift);
    put8(c, pads[i]->strobe ? 1u : 0u);
  }

  const Ppu *p = &smbc->ppu;
  put8(c, p->ctrl1);
  put8(c, p->ctrl2);
  put8(c, p->spr_addr);
  put16(c, p->vram_addr);
  put_bytes(c, p->vram, sizeof(p->vram));
  put8(c, p->scroll_x);
  put8(c, p->scroll_y);
  put8(c, p->read_buffer);
  put_bytes(c, p->oam, sizeof(p->oam));
  put8(c, p->latch ? 1u : 0u);
  put8(c, p->vblank ? 1u : 0u);
  put8(c, p->sprite0 ? 1u : 0u);
  put32(c, p->cycle);
  put8(c, p->t_nt);
  put8(c, p->scroll_log_len);
  for (unsigned i = 0; i < SMBC_PPU_SCROLL_LOG; i++) {
    put16(c, i < p->scroll_log_len ? p->scroll_log[i].scanline : 0u);
    put8(c, i < p->scroll_log_len ? p->scroll_log[i].x : 0u);
    put8(c, i < p->scroll_log_len ? p->scroll_log[i].y : 0u);
    put8(c, i < p->scroll_log_len ? p->scroll_log[i].nt : 0u);
  }

  put_bytes(c, smbc->apu.regs, SMBC_APU_REGISTER_COUNT);
  put8(c, smbc->apu.status4015);

  put32(c, smbc->nmi_owed_cycles);
  put8(c, p->scroll_carry_len);
  for (unsigned i = 0; i < SMBC_PPU_SCROLL_LOG; i++) {
    put16(c, i < p->scroll_carry_len ? p->scroll_carry[i].scanline : 0u);
    put8(c, i < p->scroll_carry_len ? p->scroll_carry[i].x : 0u);
    put8(c, i < p->scroll_carry_len ? p->scroll_carry[i].y : 0u);
    put8(c, i < p->scroll_carry_len ? p->scroll_carry[i].nt : 0u);
  }
}

/* Read the payload into smbc. The caller has already checked the length and
 * the scroll-log count, so nothing here can fail. */
static void read_payload(Cursor *c, Smbc *smbc) {
  get_bytes(c, smbc->state.ram, SMBC_RAM_SIZE);

  Joypad *pads[2] = {&smbc->joypad1, &smbc->joypad2};
  for (int i = 0; i < 2; i++) {
    pads[i]->buttons = get8(c);
    pads[i]->shift = get8(c);
    pads[i]->strobe = get8(c) != 0;
  }

  Ppu *p = &smbc->ppu;
  p->ctrl1 = get8(c);
  p->ctrl2 = get8(c);
  p->spr_addr = get8(c);
  p->vram_addr = get16(c);
  get_bytes(c, p->vram, sizeof(p->vram));
  p->scroll_x = get8(c);
  p->scroll_y = get8(c);
  p->read_buffer = get8(c);
  get_bytes(c, p->oam, sizeof(p->oam));
  p->latch = get8(c) != 0;
  p->vblank = get8(c) != 0;
  p->sprite0 = get8(c) != 0;
  p->cycle = get32(c);
  p->t_nt = get8(c);
  p->scroll_log_len = get8(c);
  /* the hash reads the scroll log's struct bytes, padding included, so start
   * from zeros rather than whatever the target held */
  memset(p->scroll_log, 0, sizeof(p->scroll_log));
  for (unsigned i = 0; i < SMBC_PPU_SCROLL_LOG; i++) {
    p->scroll_log[i].scanline = get16(c);
    p->scroll_log[i].x = get8(c);
    p->scroll_log[i].y = get8(c);
    p->scroll_log[i].nt = get8(c);
  }

  get_bytes(c, smbc->apu.regs, SMBC_APU_REGISTER_COUNT);
  smbc->apu.status4015 = get8(c);
  smbc->apu.log_len = 0; /* per-frame; not part of the image */

  smbc->nmi_owed_cycles = get32(c);
  p->scroll_carry_len = get8(c);
  memset(p->scroll_carry, 0, sizeof(p->scroll_carry));
  for (unsigned i = 0; i < SMBC_PPU_SCROLL_LOG; i++) {
    p->scroll_carry[i].scanline = get16(c);
    p->scroll_carry[i].x = get8(c);
    p->scroll_carry[i].y = get8(c);
    p->scroll_carry[i].nt = get8(c);
  }
}

/* ------------------------------------------------------------ images */

bool smbc_snapshot_write(const Smbc *smbc, uint8_t *out, size_t cap) {
  if (out == NULL || cap < smbc_snapshot_size()) {
    return false;
  }
  Cursor c = {.out = out, .in = NULL, .pos = 0};
  put_bytes(&c, SMBC_SNAPSHOT_MAGIC, 8);
  put16(&c, SMBC_SNAPSHOT_VERSION);
  put16(&c, 0);
  write_payload(&c, smbc);
  return c.pos == smbc_snapshot_size();
}

bool smbc_snapshot_read(Smbc *smbc, const uint8_t *in, size_t len) {
  if (in == NULL || len != smbc_snapshot_size()) {
    return false;
  }
  if (memcmp(in, SMBC_SNAPSHOT_MAGIC, 8) != 0) {
    return false;
  }
  Cursor c = {.out = NULL, .in = in, .pos = 8};
  if (get16(&c) != SMBC_SNAPSHOT_VERSION || get16(&c) != 0) {
    return false;
  }
  /* The scroll-log count is the one field that can index past its array,
   * so check it before anything is written. */
  size_t scroll_len_at = SMBC_SNAPSHOT_HEADER + SMBC_SNAPSHOT_PAYLOAD -
                         (SMBC_APU_REGISTER_COUNT + 1u) - (4u + 1u) -
                         SMBC_PPU_SCROLL_LOG * SMBC_SCROLL_POINT_SIZE - 1u -
                         SMBC_PPU_SCROLL_LOG * SMBC_SCROLL_POINT_SIZE;
  if (in[scroll_len_at] > SMBC_PPU_SCROLL_LOG) {
    return false;
  }
  size_t carry_len_at = SMBC_SNAPSHOT_HEADER + SMBC_SNAPSHOT_PAYLOAD -
                        SMBC_PPU_SCROLL_LOG * SMBC_SCROLL_POINT_SIZE - 1u;
  if (in[carry_len_at] > SMBC_PPU_SCROLL_LOG) {
    return false;
  }
  c.pos = SMBC_SNAPSHOT_HEADER;
  read_payload(&c, smbc);
  return true;
}

/* ----------------------------------------------------------- history */

void history_init(SmbcHistory *history, uint8_t *storage, uint32_t capacity) {
  history->storage = storage;
  history->capacity = capacity;
  history->count = 0;
  history->head = 0;
}

void history_push(SmbcHistory *history, const Smbc *smbc) {
  if (history->capacity == 0) {
    return;
  }
  size_t size = smbc_snapshot_size();
  smbc_snapshot_write(smbc, history->storage + (size_t)history->head * size,
                      size);
  history->head = (history->head + 1) % history->capacity;
  if (history->count < history->capacity) {
    history->count++;
  }
}

bool history_pop(SmbcHistory *history, Smbc *smbc) {
  if (history->count == 0) {
    return false;
  }
  history->head = (history->head + history->capacity - 1) % history->capacity;
  history->count--;
  size_t size = smbc_snapshot_size();
  return smbc_snapshot_read(smbc, history->storage + (size_t)history->head * size,
                            size);
}

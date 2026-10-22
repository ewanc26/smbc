/* core/replay.c — deterministic input recording and replay. */
#include "replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMBC_REPLAY_MAGIC "SMBCRPLY"
#define SMBC_REPLAY_HEADER 16u /* magic + version + reserved + count */
#define SMBC_REPLAY_FRAME_SIZE 10u /* p1 + p2 + hash */

/* ---------------------------------------------------------------- hash */

static uint64_t hash_bytes(uint64_t h, const void *data, size_t len) {
  const uint8_t *p = data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 1099511628211ull; /* FNV prime */
  }
  return h;
}

static uint64_t hash_u8(uint64_t h, uint8_t v) {
  return hash_bytes(h, &v, 1);
}

static uint64_t hash_u16(uint64_t h, uint16_t v) {
  return hash_bytes(hash_u8(h, (uint8_t)v), &(uint8_t){(uint8_t)(v >> 8)},
                    1);
}

uint64_t smbc_state_hash(const Smbc *smbc) {
  uint64_t h = 1469598103934665603ull; /* FNV offset basis */
  h = hash_bytes(h, smbc->state.ram, SMBC_RAM_SIZE);
  h = hash_u8(h, smbc->joypad1.buttons);
  h = hash_u8(h, smbc->joypad1.shift);
  h = hash_u8(h, smbc->joypad1.strobe ? 1u : 0u);
  h = hash_u8(h, smbc->joypad2.buttons);
  h = hash_u8(h, smbc->joypad2.shift);
  h = hash_u8(h, smbc->joypad2.strobe ? 1u : 0u);
  const Ppu *p = &smbc->ppu;
  h = hash_u8(h, p->ctrl1);
  h = hash_u8(h, p->ctrl2);
  h = hash_u8(h, p->spr_addr);
  h = hash_u16(h, p->vram_addr);
  h = hash_bytes(h, p->vram, sizeof(p->vram));
  h = hash_u8(h, p->scroll_x);
  h = hash_u8(h, p->scroll_y);
  h = hash_u8(h, p->read_buffer);
  h = hash_bytes(h, p->oam, sizeof(p->oam));
  h = hash_u8(h, p->latch ? 1u : 0u);
  h = hash_u8(h, p->vblank ? 1u : 0u);
  h = hash_u8(h, p->sprite0 ? 1u : 0u);
  h = hash_u16(h, ppu_scanline(p)); /* the cycle is a function of the line for now */
  h = hash_u8(h, p->t_nt);
  h = hash_u8(h, p->scroll_log_len);
  h = hash_bytes(h, p->scroll_log,
                 sizeof(PpuScrollPoint) * p->scroll_log_len);
  h = hash_bytes(h, smbc->apu.regs, SMBC_APU_REGISTER_COUNT);
  /* overrun scroll writes (#41) are state the next picture depends on; like
   * the overrun below, they are hashed only when present */
  if (p->scroll_carry_len != 0u) {
    h = hash_u8(h, p->scroll_carry_len);
    h = hash_bytes(h, p->scroll_carry,
                   sizeof(PpuScrollPoint) * p->scroll_carry_len);
  }
  /* an NMI overrun (#41) is state the next frame depends on. It is hashed
   * only when set, so recordings made before it existed still match. */
  if (smbc->nmi_owed_cycles != 0u) {
    for (unsigned i = 0; i < 4u; i++) {
      h = hash_u8(h, (uint8_t)(smbc->nmi_owed_cycles >> (8u * i)));
    }
  }
  return h;
}

/* ------------------------------------------------------- recording */

void replay_init(SmbcReplay *replay) {
  replay->frames = NULL;
  replay->count = 0;
  replay->cap = 0;
}

void replay_free(SmbcReplay *replay) {
  free(replay->frames);
  replay_init(replay);
}

bool replay_append(SmbcReplay *replay, uint8_t p1, uint8_t p2,
                   uint64_t hash) {
  if (replay->count == replay->cap) {
    uint32_t cap = replay->cap == 0 ? 1024u : replay->cap * 2u;
    SmbcReplayFrame *frames =
        realloc(replay->frames, (size_t)cap * sizeof(SmbcReplayFrame));
    if (frames == NULL) {
      return false;
    }
    replay->frames = frames;
    replay->cap = cap;
  }
  replay->frames[replay->count++] =
      (SmbcReplayFrame){.p1 = p1, .p2 = p2, .hash = hash};
  return true;
}

/* ------------------------------------------------------------ file */

static void put_u16(uint8_t *out, uint16_t v) {
  out[0] = (uint8_t)v;
  out[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *out, uint32_t v) {
  for (unsigned i = 0; i < 4; i++) {
    out[i] = (uint8_t)(v >> (i * 8));
  }
}

static void put_u64(uint8_t *out, uint64_t v) {
  for (unsigned i = 0; i < 8; i++) {
    out[i] = (uint8_t)(v >> (i * 8));
  }
}

static uint16_t get_u16(const uint8_t *in) {
  return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; i++) {
    v |= (uint32_t)in[i] << (i * 8);
  }
  return v;
}

static uint64_t get_u64(const uint8_t *in) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; i++) {
    v |= (uint64_t)in[i] << (i * 8);
  }
  return v;
}

static void fail(char *err, size_t err_len, const char *msg) {
  if (err != NULL && err_len > 0) {
    snprintf(err, err_len, "%s", msg);
  }
}

bool replay_save(const SmbcReplay *replay, const char *path, char *err,
                 size_t err_len) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    fail(err, err_len, "cannot open for writing");
    return false;
  }
  uint8_t header[SMBC_REPLAY_HEADER];
  memcpy(header, SMBC_REPLAY_MAGIC, 8);
  put_u16(header + 8, SMBC_REPLAY_VERSION);
  put_u16(header + 10, 0);
  put_u32(header + 12, replay->count);
  bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
  for (uint32_t i = 0; ok && i < replay->count; i++) {
    uint8_t rec[SMBC_REPLAY_FRAME_SIZE];
    rec[0] = replay->frames[i].p1;
    rec[1] = replay->frames[i].p2;
    put_u64(rec + 2, replay->frames[i].hash);
    ok = fwrite(rec, 1, sizeof(rec), f) == sizeof(rec);
  }
  if (fclose(f) != 0) {
    ok = false;
  }
  if (!ok) {
    fail(err, err_len, "write failed");
  }
  return ok;
}

bool replay_load(SmbcReplay *replay, const char *path, char *err,
                 size_t err_len) {
  replay_init(replay);
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    fail(err, err_len, "cannot open for reading");
    return false;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  bool ok = false;
  char msg[64] = "load failed";

  if (size < (long)SMBC_REPLAY_HEADER) {
    snprintf(msg, sizeof(msg), "truncated header (%ld bytes)", size);
  } else if (((unsigned long)size - SMBC_REPLAY_HEADER) %
                 SMBC_REPLAY_FRAME_SIZE !=
             0) {
    snprintf(msg, sizeof(msg), "truncated frame data (%ld bytes)", size);
  } else {
    uint8_t *buf = malloc((size_t)size);
    if (buf == NULL) {
      snprintf(msg, sizeof(msg), "out of memory");
    } else if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
      snprintf(msg, sizeof(msg), "read failed");
      free(buf);
    } else if (memcmp(buf, SMBC_REPLAY_MAGIC, 8) != 0) {
      snprintf(msg, sizeof(msg), "bad magic — not an smbc recording");
      free(buf);
    } else if (get_u16(buf + 8) != SMBC_REPLAY_VERSION) {
      snprintf(msg, sizeof(msg), "unsupported version %u (want %u)",
               (unsigned)get_u16(buf + 8), SMBC_REPLAY_VERSION);
      free(buf);
    } else {
      uint32_t count = get_u32(buf + 12);
      if (count != (uint32_t)(((unsigned long)size - SMBC_REPLAY_HEADER) /
                              SMBC_REPLAY_FRAME_SIZE)) {
        snprintf(msg, sizeof(msg), "frame count mismatch");
        free(buf);
      } else {
        const uint8_t *p = buf + SMBC_REPLAY_HEADER;
        ok = true;
        for (uint32_t i = 0; ok && i < count; i++) {
          ok = replay_append(replay, p[0], p[1], get_u64(p + 2));
          p += SMBC_REPLAY_FRAME_SIZE;
        }
        if (!ok) {
          snprintf(msg, sizeof(msg), "out of memory");
        }
        free(buf);
      }
    }
  }

  fclose(f);
  if (!ok) {
    fail(err, err_len, msg);
    replay_free(replay);
  }
  return ok;
}

/* ----------------------------------------------------------- replay */

uint32_t replay_run(Smbc *smbc, const SmbcReplay *replay,
                    uint32_t stop_frame, uint32_t *diverge_frame,
                    uint64_t *got, uint64_t *want) {
  if (stop_frame > replay->count) {
    stop_frame = replay->count;
  }
  for (uint32_t i = 0; i < stop_frame; i++) {
    joypad_set_buttons(&smbc->joypad1, replay->frames[i].p1);
    joypad_set_buttons(&smbc->joypad2, replay->frames[i].p2);
    smbc_frame(smbc);
    uint64_t h = smbc_state_hash(smbc);
    if (h != replay->frames[i].hash) {
      if (diverge_frame != NULL) {
        *diverge_frame = i;
      }
      if (got != NULL) {
        *got = h;
      }
      if (want != NULL) {
        *want = replay->frames[i].hash;
      }
      return i;
    }
  }
  if (diverge_frame != NULL) {
    *diverge_frame = SMBC_REPLAY_NO_DIVERGENCE;
  }
  return stop_frame;
}

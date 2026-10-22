/* hw/ppu.c — see hw/ppu.h for the contract.
 *
 * Failure semantics: register indices are masked to 0-7, matching the NES's
 * mirrored decoding of $2000-$3FFF; out-of-range callers cannot corrupt
 * adjacent state.
 */
#include "ppu.h"

#include <stddef.h>

void ppu_init(Ppu *ppu) {
  *ppu = (Ppu){0};
}

void ppu_set_vblank(Ppu *ppu, bool active) {
  ppu->vblank = active;
}

uint32_t ppu_line_start(uint16_t line) {
  if (line > SMBC_PPU_LAST_SCANLINE) line = SMBC_PPU_LAST_SCANLINE;
  if (line < SMBC_PPU_VBLANK_SCANLINE) {
    return (uint32_t)line * SMBC_PPU_VBLANK_CYCLE / SMBC_PPU_VBLANK_SCANLINE;
  }
  uint32_t vblank_lines = SMBC_PPU_LAST_SCANLINE - SMBC_PPU_VBLANK_SCANLINE + 1u;
  return SMBC_PPU_VBLANK_CYCLE +
         (uint32_t)(line - SMBC_PPU_VBLANK_SCANLINE) *
             (SMBC_PPU_FRAME_CYCLES - SMBC_PPU_VBLANK_CYCLE) / vblank_lines;
}

static uint16_t line_of(uint32_t cycle) {
  uint16_t line = 0;
  while (line < SMBC_PPU_LAST_SCANLINE &&
         ppu_line_start((uint16_t)(line + 1u)) <= cycle) {
    line++;
  }
  return line;
}

uint16_t ppu_scanline(const Ppu *ppu) {
  return line_of(ppu->cycle);
}

void ppu_enter_vblank(Ppu *ppu) {
  ppu->cycle = ppu_line_start(SMBC_PPU_VBLANK_SCANLINE);
  ppu->vblank = true;
  /* the NMI prepares the frame's scroll fresh; writes that overran the
   * previous frame belong to this picture, ahead of the NMI's own */
  ppu->scroll_log_len = ppu->scroll_carry_len;
  for (uint8_t i = 0; i < ppu->scroll_carry_len; i++) {
    ppu->scroll_log[i] = ppu->scroll_carry[i];
  }
  ppu->scroll_carry_len = 0;
}

void ppu_end_frame(Ppu *ppu) {
  /* run out the remaining lines, cross the pre-render line (which clears
   * both status flags on hardware), and wrap to the next frame's top */
  ppu->vblank = false;
  ppu->sprite0 = false;
  ppu->cycle = 0;
}

void ppu_set_chr(Ppu *ppu, const uint8_t *chr) {
  ppu->chr = chr;
}

void ppu_oam_dma(Ppu *ppu, const uint8_t *page) {
  for (unsigned i = 0; i < 0x100u; i++) {
    ppu->oam[(ppu->spr_addr + i) & 0xffu] = page[i];
  }
}

void ppu_await_vblank(Ppu *ppu) {
  ppu->cycle = ppu_line_start(SMBC_PPU_VBLANK_SCANLINE);
  /* the polling read observes the flag, and that read clears it */
  ppu->vblank = true;
  ppu->vblank = false;
  ppu->latch = false;
}

/* A wait never moves the clock backwards: a handler already past the
 * transition it polls for does not wait at all. */
static void advance_clock(Ppu *ppu, uint32_t target) {
  if (ppu->cycle < target) ppu->cycle = target;
}

void ppu_await_sprite0_clear(Ppu *ppu) {
  /* the hardware clears both flags at the pre-render line */
  advance_clock(ppu, ppu_line_start(SMBC_PPU_LAST_SCANLINE));
  ppu->sprite0 = false;
  ppu->vblank = false;
}

/* The hit is in the picture after this frame's pre-render line, so the
 * target is one whole frame plus the hit line's start. */
void ppu_await_sprite0_set(Ppu *ppu, int hit_scanline) {
  if (hit_scanline < 0) {
    /* no overlap this frame: the original would spin forever, so the
     * clock just runs out the visible lines with the flag clear */
    advance_clock(ppu, SMBC_PPU_FRAME_CYCLES + ppu_line_start(SMBC_PPU_LAST_VISIBLE));
    ppu->sprite0 = false;
    return;
  }
  advance_clock(ppu, SMBC_PPU_FRAME_CYCLES + ppu_line_start((uint16_t)hit_scanline));
  ppu->sprite0 = true;
}

/* Record a scroll change against the raster clock. Vblank and
 * pre-render writes (scanline 241+) take effect from the top of the
 * frame they prepare; a mid-frame write takes effect below its line.
 * The name-table bits are the hardware's t bits 10-11: $2000 sets them
 * from d0/d1, and a completed $2006 pair sets them from the address —
 * the game's VRAM-buffer sequence ends at $0000 each frame, which is
 * what returns the status bar to the first name table after a
 * write_ppu_reg1 with the gameplay table selected. (The $2006 coarse
 * x/y effect on the reload is not modelled: the game always
 * re-establishes the scroll through $2005 before the frame renders.) */
static void scroll_point(PpuScrollPoint *log, uint8_t *len, uint16_t line,
                         const Ppu *ppu) {
  PpuScrollPoint *point;
  if (*len > 0 && log[*len - 1].scanline == line) {
    point = &log[*len - 1]; /* coalesce */
  } else {
    if (*len >= SMBC_PPU_SCROLL_LOG) return;
    point = &log[(*len)++];
    point->scanline = line;
  }
  point->x = ppu->scroll_x;
  point->y = ppu->scroll_y;
  point->nt = ppu->t_nt;
}

/* A write past the frame's end (an NMI that overran it) belongs to the next
 * picture at its wrapped line: it is carried over, not logged here. */
static void ppu_log_scroll(Ppu *ppu) {
  if (ppu->cycle >= SMBC_PPU_FRAME_CYCLES) {
    scroll_point(ppu->scroll_carry, &ppu->scroll_carry_len,
                 line_of(ppu->cycle - SMBC_PPU_FRAME_CYCLES), ppu);
  } else {
    scroll_point(ppu->scroll_log, &ppu->scroll_log_len, line_of(ppu->cycle),
                 ppu);
  }
}

/* $2007 increment shared by reads and writes: d2 of $2000 selects 32. */
static uint16_t ppu_data_increment(const Ppu *ppu) {
  return (ppu->ctrl1 & 0x04u) ? 32u : 1u;
}

/* Map a 14-bit PPU address onto its actual storage byte in the flat 16KB
 * image, per the donor cartridge (vertical mirroring / horizontal
 * arrangement per its iNES header):
 *   - name tables ($2000-$2FFF): A11 decodes to the duplicate, A10 selects
 *     the physical 1KB bank, so $2800/$2C00 alias $2000/$2400;
 *   - the bus mirrors $3000-$3EFF back onto $2000-$2EFF (only PA13
 *     distinguishes them), then the name-table rule applies;
 *   - palette RAM ($3F00-$3FFF) occupies 32 bytes: address bits 5+ are
 *     ignored, so $3F20-$3FFF alias $3F00-$3F1F, and the sprite-palette
 *     colour-0 slots $3F10/14/18/1C alias the backdrop slots
 *     $3F00/04/08/0C. The game relies on the $3F10 alias: GetPlayerColors
 *     targets $3F10 with the background colour first (smb.asm SetBGColor),
 *     which the hardware lands in $3F00 — the sky's light blue.
 * The rest of the space ($0000-$1FFF) is stored flat; the port keeps its
 * CHR tables separately in src/data/chr. */
static uint16_t ppu_vram_addr(uint16_t addr) {
  addr &= 0x3fffu;
  if (addr >= 0x3f00u) {
    addr = (uint16_t)(0x3f00u | (addr & 0x1fu));
    if ((addr & 0x13u) == 0x10u) { /* $3F10/14/18/1C alias $3F00/04/08/0C */
      addr = (uint16_t)(addr - 0x10u);
    }
    return addr;
  }
  if (addr >= 0x3000u) {
    addr = (uint16_t)(addr & 0x2fffu); /* $3000-$3EFF <-> $2000-$2EFF */
  }
  if (addr >= 0x2000u) {
    addr = (uint16_t)(0x2000u | (addr & 0x07ffu)); /* drop A11, keep A10 */
  }
  return addr;
}

void ppu_write(Ppu *ppu, uint8_t reg, uint8_t value) {
  switch (reg & 0x7u) {
    case 0:
      ppu->ctrl1 = value;
      ppu->t_nt = (uint8_t)(value & 0x03u);
      ppu_log_scroll(ppu); /* the base name-table bits are scroll */
      break;
    case 1: ppu->ctrl2 = value; break;
    case 3: ppu->spr_addr = value; break;
    case 4: /* $2004: write at the OAM address, then increment */
      ppu->oam[ppu->spr_addr] = value;
      ppu->spr_addr = (uint8_t)(ppu->spr_addr + 1u);
      break;
    case 5: /* $2005: first write X, second Y, toggling */
      if (ppu->latch) {
        ppu->scroll_y = value;
        ppu_log_scroll(ppu); /* the pair is complete */
      } else {
        ppu->scroll_x = value;
      }
      ppu->latch = !ppu->latch;
      break;
    case 6: /* $2006: high byte then low byte, toggling */
      if (ppu->latch) {
        ppu->vram_addr = (uint16_t)(ppu->vram_addr | value);
        /* the completed pair reloads t, name-table bits included */
        ppu->t_nt = (uint8_t)((ppu->vram_addr >> 10) & 0x03u);
        ppu_log_scroll(ppu);
      } else {
        ppu->vram_addr = (uint16_t)((value & 0x3fu) << 8); /* 14-bit space */
      }
      ppu->latch = !ppu->latch;
      break;
    case 7: /* $2007: write at the address, then increment by 1 or 32 */
      ppu->vram[ppu_vram_addr(ppu->vram_addr)] = value;
      ppu->vram_addr = (uint16_t)(ppu->vram_addr + ppu_data_increment(ppu));
      break;
    default: break; /* $2002 is read-only */
  }
}


uint8_t ppu_read(Ppu *ppu, uint8_t reg) {
  switch (reg & 0x7u) {
    case 2: { /* $2002: observe status, clear vblank and the write latch.
               * d6 is the sprite-0 hit; the read does not clear it (the
               * pre-render line does), matching the hardware. */
      uint8_t status = (uint8_t)((ppu->vblank ? 0x80u : 0x00u) |
                                 (ppu->sprite0 ? 0x40u : 0x00u));
      ppu->vblank = false;
      ppu->latch = false;
      return status;
    }
    case 7: { /* $2007: reads outside the palette are buffered one access
               * behind. The palette region ($3F00+) returns its byte
               * immediately — no priming read — and the read buffer is
               * filled with the byte "underneath" (the $2Fxx bus mirror of
               * the name tables), which a later buffered read can observe. */
      uint16_t addr = (uint16_t)(ppu->vram_addr & 0x3fffu);
      if (addr >= 0x3f00u) {
        uint8_t palette = ppu->vram[ppu_vram_addr(addr)];
        ppu->read_buffer =
            ppu->vram[ppu_vram_addr((uint16_t)(addr & 0x2fffu))];
        ppu->vram_addr = (uint16_t)(addr + ppu_data_increment(ppu));
        return palette;
      }
      uint8_t buffered = ppu->read_buffer;
      /* the pattern tables read from the cartridge's CHR when one is
       * nominated; the flat image answers otherwise */
      ppu->read_buffer = (addr < 0x2000u && ppu->chr != NULL)
                             ? ppu->chr[addr]
                             : ppu->vram[ppu_vram_addr(addr)];
      ppu->vram_addr = (uint16_t)(addr + ppu_data_increment(ppu));
      return buffered;
    }
    case 4: /* $2004: read the byte at the OAM address, no increment */
      return ppu->oam[ppu->spr_addr];
    default:
      return 0;
  }
}

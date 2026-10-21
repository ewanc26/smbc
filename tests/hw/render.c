/* tests/hw/render.c — exercises src/hw/render.c: tile decoding, palette
 * selection, transparency, scroll, sprite flips, priority, ordering and
 * the sprite-0 hit. Frames are asserted as 2C02 palette indices. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/hw/render.h"

static uint8_t chr[0x2000];
static uint8_t fb[SMBC_RENDER_PIXELS];

static uint8_t fb_at(unsigned x, unsigned y) {
  return fb[y * SMBC_RENDER_WIDTH + x];
}

/* A fresh PPU: background and sprites on, no clipping, CHR nominated. */
static void setup(Ppu *ppu) {
  ppu_init(ppu);
  memset(chr, 0, sizeof(chr));
  ppu_set_chr(ppu, chr);
  ppu->ctrl2 = 0x1eu;
}

/* Fill tile n's pattern with a solid pixel value (1-3). */
static void solid_tile(uint8_t tile, uint8_t pix) {
  uint8_t lo = (pix & 0x01u) ? 0xffu : 0x00u;
  uint8_t hi = (pix & 0x02u) ? 0xffu : 0x00u;
  for (unsigned r = 0; r < 8u; r++) {
    chr[tile * 16u + r] = lo;
    chr[tile * 16u + 8u + r] = hi;
  }
}

/* Write a background palette: entry p of palette pal becomes colour. */
static void bg_colour(Ppu *ppu, uint8_t pal, uint8_t p, uint8_t colour) {
  ppu->vram[0x3f00u + pal * 4u + p] = colour;
}

int main(void) {
  Ppu ppu;
  int hit = -2;

  /* background tile decode: name-table cell 0 holds tile 1, whose solid
   * pattern renders palette 0 entry 1 across the whole 8×8 cell */
  setup(&ppu);
  ppu.vram[0x2000] = 0x01u;
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  ppu.vram[0x3f00] = 0x0fu; /* backdrop */
  ppu_render_frame(&ppu, fb, &hit);
  assert(fb_at(0, 0) == 0x21u);
  assert(fb_at(7, 7) == 0x21u);
  assert(fb_at(8, 0) == 0x0fu); /* next cell is empty: backdrop */
  assert(hit == -1);

  /* palette selection: the attribute table paints the 32×32 block at
   * tile (4,0) with palette 2's quadrant colour */
  setup(&ppu);
  ppu.vram[0x2000 + 4] = 0x01u;
  solid_tile(1, 2);
  bg_colour(&ppu, 2, 2, 0x16u);
  ppu.vram[0x23c0 + 1] = 0x02u; /* attr byte for block x=4-7: top-left = pal 2 */
  ppu_render_frame(&ppu, fb, NULL);
  assert(fb_at(32, 0) == 0x16u);
  assert(fb_at(39, 7) == 0x16u);

  /* transparency: zero pattern bits fall through to the backdrop */
  setup(&ppu);
  ppu.vram[0x2000] = 0x01u;
  chr[16] = 0x80u; /* only the tile's first pixel is set */
  bg_colour(&ppu, 0, 1, 0x21u);
  ppu.vram[0x3f00] = 0x0fu;
  ppu_render_frame(&ppu, fb, NULL);
  assert(fb_at(0, 0) == 0x21u);
  assert(fb_at(1, 0) == 0x0fu);

  /* scroll: $2005 X shifts the background left within the frame */
  setup(&ppu);
  ppu.vram[0x2000 + 1] = 0x01u; /* tile 1 sits at cell x=1 */
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  ppu.vram[0x3f00] = 0x0fu;
  ppu.scroll_x = 8;
  ppu_render_frame(&ppu, fb, NULL);
  assert(fb_at(0, 0) == 0x21u); /* scrolled into view from cell x=1 */
  assert(fb_at(8, 0) == 0x0fu);

  /* name-table select: ctrl1 d0 composes from the second table */
  setup(&ppu);
  ppu.vram[0x2400] = 0x01u;
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  ppu.ctrl1 = 0x01u;
  ppu_render_frame(&ppu, fb, NULL);
  assert(fb_at(0, 0) == 0x21u);

  /* sprite over background: the opaque sprite pixel wins */
  setup(&ppu);
  ppu.vram[0x2000] = 0x01u;
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  solid_tile(2, 2);
  ppu.vram[0x3f10 + 2] = 0x30u; /* sprite palette 0 entry 2 */
  ppu.oam[0] = 0x00u;           /* sprite 0 at Y=1 (the +1 delay) */
  ppu.oam[1] = 0x02u;
  ppu.oam[2] = 0x00u;
  ppu.oam[3] = 0x10u; /* X=16, clear of the background tile */
  ppu_render_frame(&ppu, fb, &hit);
  assert(fb_at(16, 1) == 0x30u);
  assert(fb_at(23, 8) == 0x30u);
  assert(hit == -1); /* no overlap with the opaque background */

  /* sprite-0 hit: sprite 0 overlapping opaque background reports the
   * first hit scanline */
  setup(&ppu);
  ppu.vram[0x2000 + 32] = 0x01u; /* background tile on scanlines 8-15 */
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  solid_tile(2, 2);
  ppu.vram[0x3f10 + 2] = 0x30u;
  ppu.oam[0] = 0x07u; /* sprite 0 covers scanlines 8-15 at X=0 */
  ppu.oam[1] = 0x02u;
  ppu_render_frame(&ppu, fb, &hit);
  assert(hit == 8);

  /* sprite behind opaque background loses; over backdrop it shows */
  setup(&ppu);
  ppu.vram[0x2000 + 32] = 0x01u;
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  solid_tile(2, 2);
  ppu.vram[0x3f10 + 2] = 0x30u;
  ppu.vram[0x3f00] = 0x0fu;
  ppu.oam[0] = 0x07u;
  ppu.oam[1] = 0x02u;
  ppu.oam[2] = 0x20u; /* priority: behind background */
  ppu.oam[3] = 0x04u; /* straddling the tile edge at x=8 */
  ppu_render_frame(&ppu, fb, NULL);
  assert(fb_at(6, 8) == 0x21u);  /* over the background tile: bg wins */
  assert(fb_at(10, 8) == 0x30u); /* over backdrop: sprite shows */

  /* sprite ordering: the lower OAM index wins the overlap */
  setup(&ppu);
  solid_tile(2, 2);
  solid_tile(3, 3);
  ppu.vram[0x3f10 + 2] = 0x30u;
  ppu.vram[0x3f10 + 3] = 0x25u;
  ppu.oam[0] = 0x00u;
  ppu.oam[1] = 0x02u;
  ppu.oam[3] = 0x20u; /* sprite 0 at X=32 */
  ppu.oam[4] = 0x00u;
  ppu.oam[5] = 0x03u;
  ppu.oam[7] = 0x20u; /* sprite 1, same spot */
  ppu_render_frame(&ppu, fb, NULL);
  assert(fb_at(32, 1) == 0x30u);

  /* horizontal flip: an asymmetric pattern mirrors about the sprite */
  setup(&ppu);
  chr[2 * 16] = 0x80u; /* tile 2: only the leftmost pixel of row 0 */
  ppu.vram[0x3f10 + 1] = 0x30u;
  ppu.vram[0x3f00] = 0x0fu;
  ppu.oam[0] = 0x00u;
  ppu.oam[1] = 0x02u;
  ppu.oam[2] = 0x40u; /* horizontal flip */
  ppu.oam[3] = 0x20u;
  ppu_render_frame(&ppu, fb, NULL);
  assert(fb_at(32 + 7, 1) == 0x30u);
  assert(fb_at(32, 1) == 0x0fu);

  /* the split: vblank scroll holds the status bar still, the gameplay
   * scroll written at the sprite-0 hit applies below its scanline */
  setup(&ppu);
  for (unsigned r = 0; r < 30u; r++) { /* tile 1 fills cell column x=1 */
    ppu.vram[0x2000 + r * 32u + 1] = 0x01u;
  }
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  ppu.vram[0x3f00] = 0x0fu;
  ppu_enter_vblank(&ppu);
  ppu_write(&ppu, 5, 0x00u); /* InitScroll: the status bar's (0,0) */
  ppu_write(&ppu, 5, 0x00u);
  ppu.cycle = ppu_line_start(31u); /* the sprite-0 hit line */
  ppu_write(&ppu, 5, 0x08u); /* SkipSprite0: gameplay scroll X=8 */
  ppu_write(&ppu, 5, 0x00u);
  ppu_end_frame(&ppu);
  ppu_render_frame(&ppu, fb, NULL);
  assert(fb_at(8, 16) == 0x21u);  /* above the split: unshifted cell */
  assert(fb_at(0, 16) == 0x0fu);
  assert(fb_at(0, 40) == 0x21u);  /* below the split: scrolled into view */
  assert(fb_at(8, 40) == 0x0fu);

  /* the same split, when the hit's gameplay scroll overran the previous
   * frame: it still splits this picture at its sprite-0 line */
  setup(&ppu);
  for (unsigned r = 0; r < 30u; r++) {
    ppu.vram[0x2000 + r * 32u + 1] = 0x01u;
  }
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  ppu.vram[0x3f00] = 0x0fu;
  ppu.cycle = SMBC_PPU_FRAME_CYCLES + ppu_line_start(31u);
  ppu_write(&ppu, 5, 0x08u);
  ppu_write(&ppu, 5, 0x00u);
  ppu_end_frame(&ppu);
  ppu_enter_vblank(&ppu);
  ppu_write(&ppu, 5, 0x00u);
  ppu_write(&ppu, 5, 0x00u);
  ppu_end_frame(&ppu);
  ppu_render_frame(&ppu, fb, NULL);
  assert(fb_at(8, 16) == 0x21u);
  assert(fb_at(0, 16) == 0x0fu);
  assert(fb_at(0, 40) == 0x21u);
  assert(fb_at(8, 40) == 0x0fu);

  /* ppu_sprite0_hit_scanline: overlap, no overlap, layers disabled */
  setup(&ppu);
  ppu.vram[0x2000 + 32] = 0x01u;
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  solid_tile(2, 2);
  ppu.oam[0] = 0x07u;
  ppu.oam[1] = 0x02u;
  assert(ppu_sprite0_hit_scanline(&ppu) == 8);
  ppu.oam[3] = 0x80u; /* moved clear of the background tile */
  assert(ppu_sprite0_hit_scanline(&ppu) == -1);
  ppu.oam[3] = 0x00u;
  ppu.ctrl2 = 0x08u; /* sprites off: no evaluation */
  assert(ppu_sprite0_hit_scanline(&ppu) == -1);

  /* determinism: identical PPU state renders an identical frame */
  setup(&ppu);
  ppu.vram[0x2000] = 0x01u;
  solid_tile(1, 1);
  bg_colour(&ppu, 0, 1, 0x21u);
  static uint8_t fb2[SMBC_RENDER_PIXELS];
  ppu_render_frame(&ppu, fb, NULL);
  ppu_render_frame(&ppu, fb2, NULL);
  assert(memcmp(fb, fb2, SMBC_RENDER_PIXELS) == 0);

  printf("tests/hw/render: ok\n");
  return 0;
}

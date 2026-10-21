/* tests/hw/ppu.c — exercises src/hw/ppu.c: the $2005/$2006 write-pair
 * latch, the $2002 status read clearing vblank and the latch, the OAM
 * registers and DMA, the raster clock's vblank/sprite-0 transitions and
 * the scroll log. */
#include <assert.h>
#include <stdio.h>

#include "../../src/hw/ppu.h"

int main(void) {
  Ppu ppu;
  ppu_init(&ppu);

  /* $2006 pair assembles a 16-bit VRAM address, high byte first */
  ppu_write(&ppu, 6, 0x21u);
  ppu_write(&ppu, 6, 0x08u);
  assert(ppu.vram_addr == 0x2108u);

  /* $2005 pair stores X then Y */
  ppu_write(&ppu, 5, 0x40u);
  ppu_write(&ppu, 5, 0x18u);
  assert(ppu.scroll_x == 0x40u);
  assert(ppu.scroll_y == 0x18u);

  /* vblank is observable at $2002 bit 7, and the read clears it */
  ppu_set_vblank(&ppu, true);
  assert((ppu_read(&ppu, 2) & 0x80u) != 0);
  assert((ppu_read(&ppu, 2) & 0x80u) == 0);

  /* a $2002 read also resets the write-pair latch */
  ppu_write(&ppu, 6, 0x3fu);
  (void)ppu_read(&ppu, 2);
  ppu_write(&ppu, 6, 0x10u);
  assert(ppu.vram_addr == 0x1000u);

  /* register indices mirror mod 8, as the hardware decoding does */
  ppu_write(&ppu, 8, 0x99u); /* $2000 mirrored */
  assert(ppu.ctrl1 == 0x99u);

  /* $2006/$2007: write through the address pair, then walk by 1 or 32.
   * The latch is mid-pair from the checks above; a $2002 read re-arms it,
   * and ctrl1's d2 must be clear for the increment-by-1 case. */
  (void)ppu_read(&ppu, 2);
  ppu_write(&ppu, 0, 0x00);
  ppu_write(&ppu, 6, 0x23);
  ppu_write(&ppu, 6, 0x10);
  ppu_write(&ppu, 7, 0x5a);
  ppu_write(&ppu, 7, 0x6b);
  assert(ppu.vram[0x2310] == 0x5a);
  assert(ppu.vram[0x2311] == 0x6b);

  ppu_write(&ppu, 0, 0x04); /* d2: increment by 32 */
  ppu_write(&ppu, 6, 0x20);
  ppu_write(&ppu, 6, 0x00);
  ppu_write(&ppu, 7, 0x77);
  ppu_write(&ppu, 7, 0x88);
  assert(ppu.vram[0x2000] == 0x77);
  assert(ppu.vram[0x2020] == 0x88);

  /* $2007 reads are buffered one access behind */
  ppu_write(&ppu, 0, 0x00);
  ppu_write(&ppu, 6, 0x23);
  ppu_write(&ppu, 6, 0x10);
  assert(ppu_read(&ppu, 7) == 0x00); /* stale buffer */
  assert(ppu_read(&ppu, 7) == 0x5a);

  /* name-table mirroring: the donor cartridge is vertical mirroring /
   * horizontal arrangement, so A11 decodes to the duplicate and A10
   * selects the physical 1KB bank */
  (void)ppu_read(&ppu, 2);
  ppu_write(&ppu, 0, 0x00);
  ppu_write(&ppu, 6, 0x20);
  ppu_write(&ppu, 6, 0x00);
  ppu_write(&ppu, 7, 0x11); /* $2000 -> storage $2000 */
  assert(ppu.vram[0x2000] == 0x11);
  assert(ppu.vram[0x2800] == 0x00); /* name table 2 is a mirror */

  /* a write through the mirror lands on the same storage */
  ppu_write(&ppu, 6, 0x28);
  ppu_write(&ppu, 6, 0x07);
  ppu_write(&ppu, 7, 0x33); /* $2807 -> storage $2007 */
  assert(ppu.vram[0x2007] == 0x33);
  assert(ppu.vram[0x2807] == 0x00);

  /* name table 1 is a distinct bank */
  ppu_write(&ppu, 6, 0x24);
  ppu_write(&ppu, 6, 0x00);
  ppu_write(&ppu, 7, 0x22); /* $2400 -> storage $2400 */
  assert(ppu.vram[0x2400] == 0x22);
  assert(ppu.vram[0x2000] == 0x11);

  /* $2C00 is name table 3, the mirror of name table 1 */
  ppu_write(&ppu, 6, 0x2c);
  ppu_write(&ppu, 6, 0x00);
  ppu_write(&ppu, 7, 0x44);
  assert(ppu.vram[0x2400] == 0x44);
  assert(ppu.vram[0x2c00] == 0x00);

  /* $3000-$3EFF is a bus mirror of $2000-$2EFF: no extra storage */
  ppu_write(&ppu, 6, 0x30);
  ppu_write(&ppu, 6, 0x00);
  ppu_write(&ppu, 7, 0x55);
  assert(ppu.vram[0x2000] == 0x55);
  assert(ppu.vram[0x3000] == 0x00);

  /* palette-region reads return the palette byte immediately — no priming
   * read — and fill the buffer with the byte "underneath" at $2Fxx (the
   * $3000-$3EFF mirror of the name tables) */
  (void)ppu_read(&ppu, 2);
  ppu_write(&ppu, 0, 0x00);
  ppu_write(&ppu, 6, 0x3f);
  ppu_write(&ppu, 6, 0x00);
  ppu_write(&ppu, 7, 0x2a); /* palette byte at $3F00 */
  ppu_write(&ppu, 6, 0x2f);
  ppu_write(&ppu, 6, 0x00);
  ppu_write(&ppu, 7, 0x5b); /* underlying $2F00 -> storage at $2700 */
  ppu_write(&ppu, 6, 0x3f);
  ppu_write(&ppu, 6, 0x00);
  assert(ppu_read(&ppu, 7) == 0x2a); /* immediate palette byte */
  /* the read buffer now holds the shadowed $2700 byte; a buffered read
   * outside the palette region returns it */
  ppu_write(&ppu, 6, 0x20);
  ppu_write(&ppu, 6, 0x00);
  assert(ppu_read(&ppu, 7) == 0x5b);

  /* palette RAM is 32 bytes: $3F20-$3FFF aliases $3F00-$3F1F */
  ppu_write(&ppu, 6, 0x3f);
  ppu_write(&ppu, 6, 0x21);
  ppu_write(&ppu, 7, 0x7e);
  assert(ppu.vram[0x3f01] == 0x7e); /* $3F21 folds onto $3F01 */
  assert(ppu.vram[0x3f21] == 0x00); /* the mirror carries no storage */
  assert(ppu.vram[0x3f20] == 0x00);

  /* $2007 with a nominated CHR: the pattern tables read from it behind
   * the one-access buffer */
  static uint8_t chr[0x2000];
  ppu_init(&ppu);
  ppu_set_chr(&ppu, chr);
  chr[0x1ec0] = 0xaau;
  chr[0x1ec1] = 0xbbu;
  ppu_write(&ppu, 6, 0x1eu);
  ppu_write(&ppu, 6, 0xc0u);
  (void)ppu_read(&ppu, 7); /* the priming read returns garbage */
  assert(ppu_read(&ppu, 7) == 0xaau);
  assert(ppu_read(&ppu, 7) == 0xbbu);
  /* without a CHR the flat image answers */
  ppu_init(&ppu);
  ppu_write(&ppu, 6, 0x1eu);
  ppu_write(&ppu, 6, 0xc0u);
  (void)ppu_read(&ppu, 7);
  assert(ppu_read(&ppu, 7) == 0x00u);

  /* $2003/$2004: writes land at the OAM address and walk it forward */
  ppu_init(&ppu);
  ppu_write(&ppu, 3, 0x08u);
  ppu_write(&ppu, 4, 0x5au);
  ppu_write(&ppu, 4, 0xa5u);
  assert(ppu.oam[0x08] == 0x5au);
  assert(ppu.oam[0x09] == 0xa5u);
  assert(ppu.spr_addr == 0x0au);

  /* $2004 reads observe the byte at the address without walking it */
  ppu_write(&ppu, 3, 0x09u);
  assert(ppu_read(&ppu, 4) == 0xa5u);
  assert(ppu_read(&ppu, 4) == 0xa5u);
  assert(ppu.spr_addr == 0x09u);

  /* $4014 OAM DMA: the full page copies across, in order, starting at
   * the $2003 address — the game's NMI zeroes $2003 first, so the copy
   * is linear and sprite ordering is preserved verbatim */
  static uint8_t page[0x100];
  for (unsigned i = 0; i < 0x100u; i++) page[i] = (uint8_t)i;
  ppu_init(&ppu);
  ppu_write(&ppu, 3, 0x00u);
  ppu_oam_dma(&ppu, page);
  for (unsigned i = 0; i < 0x100u; i++) assert(ppu.oam[i] == (uint8_t)i);

  /* a nonzero $2003 wraps the copy around the 256-byte memory */
  ppu_init(&ppu);
  ppu_write(&ppu, 3, 0xf0u);
  ppu_oam_dma(&ppu, page);
  for (unsigned i = 0; i < 0x100u; i++) {
    assert(ppu.oam[(0xf0u + i) & 0xffu] == (uint8_t)i);
  }

  /* the raster clock: vblank begins at scanline 241 with a fresh scroll
   * log, and the status read observes d7 */
  ppu_init(&ppu);
  ppu_enter_vblank(&ppu);
  assert(ppu_scanline(&ppu) == 241u);
  assert(ppu.cycle == SMBC_PPU_VBLANK_CYCLE);
  assert((ppu_read(&ppu, 2) & 0x80u) != 0);
  assert(ppu.scroll_log_len == 0);

  /* d6 is the sprite-0 hit: a $2002 read observes but does not clear it
   * (the pre-render line clears it), while clearing vblank as always */
  ppu_init(&ppu);
  ppu_enter_vblank(&ppu);
  ppu_await_sprite0_clear(&ppu);
  ppu_await_sprite0_set(&ppu, 31);
  assert(ppu_scanline(&ppu) == 31u);
  assert((ppu_read(&ppu, 2) & 0xc0u) == 0x40u); /* vblank already clear */
  assert((ppu_read(&ppu, 2) & 0x40u) != 0);     /* hit survives the read */
  ppu_await_sprite0_clear(&ppu);                /* the pre-render line */
  assert(ppu_scanline(&ppu) == 261u);
  assert((ppu_read(&ppu, 2) & 0x40u) == 0);

  /* a wait with no overlap runs the visible frame out, flag clear */
  ppu_init(&ppu);
  ppu_await_sprite0_set(&ppu, -1);
  assert(ppu_scanline(&ppu) == 239u);
  assert((ppu_read(&ppu, 2) & 0x40u) == 0);

  /* the frame's end wraps the clock and clears any standing flags */
  ppu_init(&ppu);
  ppu_enter_vblank(&ppu);
  ppu_end_frame(&ppu);
  assert(ppu.cycle == 0u);
  assert(ppu_scanline(&ppu) == 0);
  assert((ppu_read(&ppu, 2) & 0xc0u) == 0);

  /* the scroll log: vblank writes tag at 241, mid-frame writes at the
   * clock's position, and vblank starts the frame's log fresh */
  ppu_init(&ppu);
  ppu_enter_vblank(&ppu);
  ppu_write(&ppu, 5, 0x08u); /* X */
  ppu_write(&ppu, 5, 0x00u); /* Y: the pair completes, logged */
  assert(ppu.scroll_log_len == 1);
  assert(ppu.scroll_log[0].scanline == 241u);
  assert(ppu.scroll_log[0].x == 0x08u);
  /* writes on the same scanline coalesce: t's name-table bits fold
   * into the standing record, matching the horizontal reload's timing */
  ppu_write(&ppu, 0, 0x01u);
  assert(ppu.scroll_log_len == 1);
  assert(ppu.scroll_log[0].nt == 0x01u);
  /* a completed $2006 pair reloads t's name-table bits from the
   * address — the VRAM sequence's final $0000 is what returns the
   * status bar to the first name table */
  ppu_write(&ppu, 6, 0x28u);
  ppu_write(&ppu, 6, 0x00u);
  assert(ppu.scroll_log_len == 1);
  assert(ppu.scroll_log[0].nt == 0x02u);
  ppu.cycle = ppu_line_start(31u); /* the sprite-0 hit line */
  ppu_write(&ppu, 5, 0x40u);
  ppu_write(&ppu, 5, 0x00u);
  assert(ppu.scroll_log_len == 2);
  assert(ppu.scroll_log[1].scanline == 31u);
  assert(ppu.scroll_log[1].x == 0x40u);
  ppu_enter_vblank(&ppu); /* next frame: the log starts fresh */
  assert(ppu.scroll_log_len == 0);

  /* a write past the frame's end (an NMI that overran it) is carried to the
   * next picture at its wrapped line, ahead of that picture's own NMI writes */
  ppu_init(&ppu);
  ppu_enter_vblank(&ppu);
  ppu.cycle = SMBC_PPU_FRAME_CYCLES + ppu_line_start(31u);
  ppu_write(&ppu, 5, 0x40u);
  ppu_write(&ppu, 5, 0x00u);
  assert(ppu.scroll_log_len == 0);
  assert(ppu.scroll_carry_len == 1);
  ppu_end_frame(&ppu);
  ppu_enter_vblank(&ppu);
  assert(ppu.scroll_carry_len == 0);
  assert(ppu.scroll_log_len == 1);
  assert(ppu.scroll_log[0].scanline == 31u);
  assert(ppu.scroll_log[0].x == 0x40u);

  /* the clock: scanline starts strictly increase, line 241 begins at the
   * vblank cycle, and every cycle maps back to the line that contains it */
  assert(ppu_line_start(0) == 0u);
  assert(ppu_line_start(SMBC_PPU_VBLANK_SCANLINE) == SMBC_PPU_VBLANK_CYCLE);
  assert(ppu_line_start(SMBC_PPU_LAST_SCANLINE) < SMBC_PPU_FRAME_CYCLES);
  for (uint16_t y = 0; y < SMBC_PPU_LAST_SCANLINE; y++) {
    assert(ppu_line_start((uint16_t)(y + 1u)) > ppu_line_start(y));
  }
  for (uint16_t y = 0; y <= SMBC_PPU_LAST_SCANLINE; y++) {
    ppu.cycle = ppu_line_start(y);
    assert(ppu_scanline(&ppu) == y);
    if (y < SMBC_PPU_LAST_SCANLINE) {
      ppu.cycle = ppu_line_start((uint16_t)(y + 1u)) - 1u;
      assert(ppu_scanline(&ppu) == y);
    }
  }

  printf("tests/hw/ppu: ok\n");
  return 0;
}

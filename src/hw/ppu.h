/* hw/ppu.h — PPU register model.
 *
 * Owns: the CPU-visible state of the picture processing unit's eight
 * mirrored registers, including the $2005/$2006 write-pair latch and the
 * status flags reads can clear, a flat VRAM image written through
 * $2006/$2007, and the 256-byte OAM sprite attribute memory filled
 * through $2004 and the $4014 DMA. Reads and writes reach actual
 * storage through the donor cartridge's address decoding: name-table
 * mirroring ($2800/$2C00 alias $2000/$2400), the $3000-$3EFF bus mirror
 * of the name tables, and the 32-byte palette RAM at $3F00-$3FFF.
 * Rendering is not modelled; a host frontend consumes the VRAM image
 * and the OAM copy later.
 *
 * Collaborators: core/frame drives the raster clock and vblank;
 * translated game code touches the registers exactly as the original
 * does; hw/render consumes the scroll history and the OAM copy.
 *
 * Timing contract (issue #33): one frame of emulated time is one NTSC
 * frame, 262 scanlines. The PPU carries a virtual raster clock,
 * scanline, advanced at explicit phase boundaries by the frame driver
 * and by the translated wait loops — never by wall-clock time, host
 * refresh rate or audio latency, so identical frame-indexed inputs
 * always produce identical transitions. The clock is scanline-granular
 * by design: CPU work between phase calls consumes no emulated time
 * (the original's instruction timing is not modelled), and the
 * sub-scanline HBlankDelay after the sprite-0 hit is absorbed into the
 * hit scanline itself.
 */
#ifndef SMBC_HW_PPU_H
#define SMBC_HW_PPU_H

#include <stdbool.h>
#include <stdint.h>

/* The raster clock's hardware landmarks (NTSC 2C02). */
#define SMBC_PPU_LAST_SCANLINE 261u /* pre-render: flags clear here */
#define SMBC_PPU_VBLANK_SCANLINE 241u /* vblank begins, NMI fires */
#define SMBC_PPU_LAST_VISIBLE 239u

/* The clock counts CPU cycles. One frame is 29780 of them, and vblank begins
 * 27395 cycles in (issue #41). */
#define SMBC_PPU_FRAME_CYCLES 29780u
#define SMBC_PPU_VBLANK_CYCLE 27395u

/* One scroll record: the raster line the state took effect on and the
 * scroll it selects (the $2005 pair plus t's name-table bits). Writes
 * on the same scanline coalesce into one record — the hardware's
 * horizontal reload applies the whole of t at the next scanline
 * boundary, so sub-scanline ordering is invisible at this model's
 * granularity. The renderer resolves each scanline against the newest
 * record whose line already took effect. */
typedef struct {
  uint16_t scanline;
  uint8_t x;
  uint8_t y;
  uint8_t nt; /* $2000 d0/d1 */
} PpuScrollPoint;

#define SMBC_PPU_SCROLL_LOG 8u

typedef struct {
  const uint8_t *chr; /* CHR ROM for the pattern tables ($0000-$1FFF),
                       * supplied by the host like a cartridge's; $2007
                       * reads consult it, writes still land in the flat
                       * image */
  uint8_t ctrl1;      /* $2000 writes, and the game's mirror lives in RAM */
  uint8_t ctrl2;      /* $2001 */
  uint8_t spr_addr;   /* $2003 */
  uint16_t vram_addr; /* assembled by the $2006 write pair */
  uint8_t vram[0x4000]; /* flat VRAM image; mirroring applied on access */
  uint8_t scroll_x;   /* first $2005 write */
  uint8_t scroll_y;   /* second $2005 write */
  uint8_t read_buffer; /* buffered $2007 data */
  uint8_t oam[0x100]; /* sprite attribute memory: 64 sprites, 4 bytes
                       * each (Y, tile, attributes, X), in the original
                       * ordering; written via $2004 and the $4014 DMA */
  bool latch;         /* $2005/$2006 first/second-write toggle */
  bool vblank;        /* status bit 7, set by the frame loop */
  bool sprite0;       /* status bit 6, driven by the raster clock */
  uint32_t cycle;     /* virtual raster clock: CPU cycles into the frame;
                       * the scanline is derived with ppu_scanline */
  uint8_t t_nt;       /* t's name-table bits: $2000 d0/d1, or address
                       * bits 10-11 after a completed $2006 pair — kept
                       * apart from ctrl1 because the VRAM address
                       * sequence rewrites them between $2000 writes */
  PpuScrollPoint scroll_log[SMBC_PPU_SCROLL_LOG]; /* this frame's scroll
                       * writes, in order; reset when vblank begins */
  uint8_t scroll_log_len;
  PpuScrollPoint scroll_carry[SMBC_PPU_SCROLL_LOG]; /* writes that overran
                       * the frame, for the next picture (see ppu_log_scroll) */
  uint8_t scroll_carry_len;
} Ppu;

void ppu_init(Ppu *ppu);

/* Register access by index 0-7, as the CPU sees $2000-$2007. */
void ppu_write(Ppu *ppu, uint8_t reg, uint8_t value);
uint8_t ppu_read(Ppu *ppu, uint8_t reg);

/* Driven by the frame loop: entering vblank sets the flag, and a $2002 read
 * observes then clears it. */
void ppu_set_vblank(Ppu *ppu, bool active);

/* Raster-clock phase boundaries, driven by the frame loop:
 *  - ppu_enter_vblank: scanline 241 — vblank sets, the NMI fires, and
 *    the scroll log starts fresh for the frame the NMI prepares;
 *  - ppu_end_frame: the frame's remaining lines run out, the clock
 *    wraps past the pre-render line, and any flag still standing
 *    clears (a game path that skipped the sprite-0 waits would
 *    otherwise leave stale state). */
void ppu_enter_vblank(Ppu *ppu);
void ppu_end_frame(Ppu *ppu);

/* The cycle at which a scanline begins, and the scanline the clock is on.
 * Scanline starts strictly increase, and line 241 begins at the vblank
 * cycle. ppu_scanline is clamped to the pre-render line. */
uint32_t ppu_line_start(uint16_t line);
uint16_t ppu_scanline(const Ppu *ppu);

/* Nominate the cartridge's CHR ROM for pattern-table reads. */
void ppu_set_chr(Ppu *ppu, const uint8_t *chr);

/* $4014 OAM DMA: copy the nominated 256-byte CPU RAM page (the $0200
 * sprite page during SMB's NMI) into OAM. The hardware copies all 256
 * bytes starting at the current $2003 OAM address, wrapping around;
 * the game's NMI writes $2003 = 0 first, so the copy is linear in
 * practice. Sprite ordering is preserved verbatim.
 *
 * Timing: real DMA stalls the CPU for 513/514 cycles while the copy
 * streams one byte per cycle. At this port's frame-step abstraction
 * the copy is atomic — the translated code never observes a partial
 * DMA (the game performs no reads between sta SPR_DMA and the NMI
 * tail), so the stall has no observable effect to model. A
 * cycle-level frontend would account for it in the frame budget.
 *
 * The $2002 d5 sprite-overflow flag is not set here: overflow is a
 * per-scanline rendering evaluation and belongs with the renderer
 * (#32) and timing (#33) work. */
void ppu_oam_dma(Ppu *ppu, const uint8_t *page);

/* Host synchronisation points. The original spins on $2002 until the
 * hardware reaches vblank / the sprite-0 hit; in this port the raster
 * clock advances to the waited-for transition, deterministically:
 *  - ppu_await_vblank advances to scanline 241 and presents the flag
 *    as the original's polling read would observe it;
 *  - ppu_await_sprite0_clear advances to the pre-render line, where
 *    the hardware clears both status flags;
 *  - ppu_await_sprite0_set advances to the sprite-0 hit scanline
 *    supplied by the caller — computed by hw/render from the current
 *    PPU state, keeping the timing layer free of pixel work. When the
 *    caller reports no hit (-1) the clock advances past the visible
 *    frame and the flag stays clear; the original would hang on the
 *    wait, and the game never arranges that. */
void ppu_await_vblank(Ppu *ppu);
void ppu_await_sprite0_clear(Ppu *ppu);
void ppu_await_sprite0_set(Ppu *ppu, int hit_scanline);

#endif /* SMBC_HW_PPU_H */

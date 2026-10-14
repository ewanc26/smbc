/* core/frame.h — the frame loop and the console state bundle.
 *
 * Owns: the Smbc struct (RAM, both joypads, PPU, APU) and one frame of
 * emulated time: the raster clock enters vblank, the translated NMI
 * handler runs (advancing the clock through its hardware waits), and
 * the frame runs out. Initialisation runs the disassembly's reset
 * sequence. The full timing contract lives in hw/ppu.h.
 *
 * Deterministic — no wall-clock reads anywhere below the frontend.
 */
#ifndef SMBC_CORE_FRAME_H
#define SMBC_CORE_FRAME_H

#include "../hw/apu.h"
#include "../hw/joypad.h"
#include "../hw/ppu.h"
#include "state.h"

typedef struct Smbc {
  CoreState state;
  Joypad joypad1;
  Joypad joypad2;
  Ppu ppu;
  Apu apu;
  /* frames the power-on reset consumes before the cold boot finishes
   * (boot.h). These are emulated frames with the screen still blank,
   * so a frame-indexed comparison counts them first. */
  unsigned reset_frames;
  /* #41: CPU cycles the NMI handler that overruns this frame still owes,
   * measured from this frame's start. 0 when no handler overruns. */
  uint32_t nmi_owed_cycles;
  /* CPU cycles the handler has charged in the current NMI, through
   * smbc_charge_cycles. Reset before each handler runs. */
  uint32_t nmi_cycles;
} Smbc;

/* NTSC timing in CPU cycles. A frame is 29780 cycles; the NMI fires at the
 * start of line 241, which the issue measured at 27395 cycles into the frame
 * (smb.asm: NonMaskableInterrupt, issue #41). */
#define SMBC_CPU_CYCLES_PER_FRAME 29780u
#define SMBC_CPU_CYCLES_TO_VBLANK 27395u

/* Charge CPU cost to the NMI handler that is running. Translated routines
 * call this with their cycle counts; nothing charges yet, so every handler
 * costs zero and the NMI timeline below never overruns. */
void smbc_charge_cycles(Smbc *smbc, uint32_t cycles);

/* The NMI timeline (#41), as pure steps. nmi_timeline_runs says whether the
 * handler fires at this frame's vblank point, given the cycles still owed
 * from earlier frames: a handler still running has NMIs disabled, so the
 * vblank is lost. nmi_timeline_carry gives what a handler that ends at
 * `end` (cycles from this frame's start) still owes the next frame. */
bool nmi_timeline_runs(uint32_t owed);
uint32_t nmi_timeline_carry(uint32_t end);

/* Zero all state, then run the reset sequence (smb.asm: Start). */
void smbc_init(Smbc *smbc);

/* Run one frame: vblank in, NMI (smb.asm: NonMaskableInterrupt) with its
 * raster-clock waits, frame out. A handler that overruns the frame carries
 * its remaining cycles into the next frame, where a vblank that falls inside
 * them raises no NMI. */
void smbc_frame(Smbc *smbc);

#endif /* SMBC_CORE_FRAME_H */

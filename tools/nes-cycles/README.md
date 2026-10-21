# NMI cycle reference (dev-only, #41)

A cycle-counting 6502 that runs the original ROM with the port's PPU, joypad
and APU models behind its bus, to measure what each NMI costs. Nothing here is
part of the game build; `src/` does not depend on it.

Files:

- `cpu6502.c/.h` — the official 6502 opcodes with their base cycles, page-cross
  and taken-branch penalties, NMI entry, and RTI. All 151 legal opcodes execute.
- `nes_trace.c` — NROM-256 machine: raster clock (29780 cycles per frame,
  vblank at 27395), NMI when `$2000` bit 7 is set, sprite-0 hit from the port's
  renderer, OAM DMA stall. Writes `<prefix>-nmi.csv` (frame, start cycle, cost,
  depth) and `<prefix>-ram.bin` (RAM after each frame).
- `emu_ram.py` — the same frames from the reference emulator (needs
  `SMBC_ROM` and the pinned nes-py), for comparison.
- `ram_compare.py` — reports frame-counter and mode mismatches between the two.

Build and run (inputs are one button byte per frame, as in `frame_dump`):

    cc -std=c23 -O2 -o nes_trace nes_trace.c cpu6502.c \
       ../../src/hw/ppu.c ../../src/hw/render.c ../../src/hw/joypad.c \
       ../../src/hw/apu.c ../../src/data/chr.c
    ./nes_trace /path/to/smb.nes inputs.bin 450 out

With `NES_PROFILE=1` the run also writes `out-routines.csv`: for each ROM
address, the self cycles spent in it inside NMI handlers and how many times it
was entered (JSR or the NMI vector). Addresses are ROM addresses.

`asm_labels.py` gives the address of every `smb.asm` label. It assembles the
source and checks the result byte for byte against the ROM (0 mismatches for
the 32 KB PRG). Use `--syms` to list the labels, then join them to the profile.

`static_costs.py smb.asm <prefix>...` takes each instruction's base cycles from
the `BASE` table in `cpu6502.c` and checks the `-pc.csv` profiles against it. The
measured cost per execution must never be below base, and must equal it unless a
branch or indexed read adds a penalty. It exits 1 on a violation or an address
the assembler does not map as an instruction start.

Current result on the five gameplay profiles: 0 addresses below base; 3
unexplained. 80b3 is a sprite-0 poll site whose wait is charged to the store
(about 500 cycles per execution). 8222 and e3eb hold BIT abs bytes but are not
instruction starts in `asm_labels`, so the profile may attribute cycles to a
mid-instruction PC. Resolve this before charging NMI cost from the CPU clock.

## Validation so far

Against the reference emulator's RAM, on the title and demo scenarios:

- The first title NMI costs 71599 cycles starting at 27397 in frame 3. The
  issue measured 71600 and 27397 — the same figure, from the ROM alone.
- Frames 4 and 5 run no NMI; the next NMI runs in frame 6. `OperMode` and
  `OperMode_Task` agree on every frame of all four scenarios.
- Title: FrameCounter agrees on every frame except frame 26 (one count).
- Demo (700 frames): FrameCounter disagrees on 9 frames.

Known residuals:

- Gameplay (start, walk, jump): FrameCounter diverges from frame 205. The NMI at
  frame 201 waits 40481 cycles. The wait is for a sprite-0 hit the renderer
  reports only in a later frame, because rendering is off at the poll. Whether
  the emulator agrees depends on pixel-exact rendering and sprite-0 timing
  (issues #32 and #33), which are not done.
- Incidental RAM (stack, zero page, sprite page) differs in a few bytes.

## Step 1 findings: sprite-0 and the renderer (#32, #33)

- `NES_FRAMEBUFFER=1` makes `nes_trace` also write the renderer's frame at each
  boundary (`<prefix>-fb.bin`, 256×240 palette indices).
- Walk scenario, against the emulator's screen (palette mapped by majority
  colour, as `emulator_compare.py` does): 220 of 240 frames are pixel-exact.
- The gameplay wait at frame 201 is the sprite-0 hit missing from the renderer's
  state at that moment. The frame 201 picture differs in 143 pixels, all in one
  colour (palette index 0x1A on the port, 0x16 in the emulator).
- Frames 204 and 8 differ across the whole screen: the port's backdrop has
  already changed to black (`$3F00` = 0x0F) at the boundary, while the emulator
  still shows the sky colour. Palette writes happen inside the NMI, so these
  frames depend on NMI timing: the same gap as #41.

Conclusion: the remaining mismatches share one cause, when NMI effects land
relative to the frame boundary. A renderer change alone will not close #32 or
#33. Next steps are to pin the NMI cost and write timing down further, using
this harness's frame output as the reference.

## Step 1 calibration: where the emulator samples its frame

The RAM and frame samples are taken `SAMPLE_SHIFT` (default 190) cycles after the
frame boundary. This is an empirical fit, not a hardware constant. Sweeping it
against nes-py's frame counter over the walk scenario (949 frames):

| sample point after boundary | FrameCounter mismatches |
|---|---|
| 0 (old default) | 402 |
| 150 | 50 |
| 190 (default) | 45 |
| 250 | 78 |
| sample after the NMI ends | 942 (worse; sample overwritten) |

Frame-length variants (29780 vs 29781, alternating) and the vblank point
(27392–27398) made no difference, so the residual is not a frame-length drift.

With the default, the scenarios agree as follows (FrameCounter mismatches):
idle 2 of 89 (frame 10 is new, frame 26 is the old one), start 9 of 449, walk 45
of 949, jump 42 of 949, demo 8 of 699. `OperMode_Task` disagrees on one frame in
start, walk and jump; before the calibration it agreed on every frame. The
calibration trades a title-screen frame for gameplay agreement, so it is a
compromise.

Open: the residual disagreements are single NMIs whose frame-counter increment
falls within about ±60 cycles of the sample point. The cause is a few-tens-of-cycles
difference in the NMI's cost before the increment, or in how nes-py's frame
boundary is defined. Closing it needs the emulator's per-instruction state, which
nes-py does not expose.

## Visual parity (#31): what the pictures show

- **Display offset.** The port's frame *f* matches the emulator's screen at step
  *f*+1. Game state written in NMI *f* is first drawn in frame *f*+1, so
  `compare_pictures.py` defaults to offset 1. (The RAM comparisons above use the
  same index, not offset 1.)
- With that offset the renderer is pixel-exact wherever the NMI timing agrees:
  - walk, frames 0–355 (all exact), then 747 of 949 frames overall;
  - start, 448 of 449 frames (frame 356 differs by 171 pixels);
  - jump, the same as walk;
  - demo, frames 0–556.
- **Frame 356** (start and walk): the emulator draws a 171-pixel blue sky line on
  the top row; the port's top row is black. A palette write at the first visible line.
- **Frame 748 on** (walk, jump): the scroll drifts by a few pixels from frame to
  frame, and no single frame offset fits. This looks like an NMI skipped or run
  one frame off earlier, which is the #41 gap.
- **Frame 557** (demo): the title logo and copyright line differ, again from NMI
  timing.

Parity is not complete. Every remaining difference traces to when the NMI's
writes land relative to the frame, which is the #41 problem: per-routine cycle
costs and their order within the handler. The renderer itself is not the blocker.

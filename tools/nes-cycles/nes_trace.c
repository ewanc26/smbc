/* tools/nes-cycles/nes_trace.c — runs the original ROM on a cycle-counting
 * 6502 (cpu6502.c) and records the cost of every NMI (#41). Dev-only.
 *
 *   nes_trace rom.nes inputs.bin frames out_prefix
 *
 * The machine is NROM-256 with the port's PPU, joypad and APU register models
 * behind the bus. Timing: one frame is 29780 CPU cycles; vblank rises at 27395
 * and raises an NMI when $2000 bit 7 is set; the sprite-0 flag rises at the
 * hit line that the port's renderer computes, in the next frame's visible
 * lines. Writes:
 *   out_prefix-nmi.csv  one line per NMI: frame, start cycle in that frame,
 *                       cost in cycles (entry to RTI), nesting depth
 *   out_prefix-ram.bin  2 KB of CPU RAM after each frame, in order
 *
 * Not a game-build file: nothing under src/ depends on it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu6502.h"
#include "../../src/data/chr.h"
#include "../../src/hw/apu.h"
#include "../../src/hw/joypad.h"
#include "../../src/hw/ppu.h"
#include "../../src/hw/render.h"

/* The RAM and frame samples are taken this many cycles after the frame
 * boundary. 190 is an empirical fit to the reference emulator's frame output
 * (nes-py): it sits where its frame counter increments fall, and it is not a
 * hardware constant. See README.md. */
#ifndef SAMPLE_SHIFT
#define SAMPLE_SHIFT 190
#endif
#ifndef PIXEL_SHIFT
#define PIXEL_SHIFT 190
#endif
#ifndef FRAME_CYCLES
#define FRAME_CYCLES 29780u
#endif
#ifndef VBLANK_CYCLE
#define VBLANK_CYCLE 27395u
#endif
#ifndef DMA_STALL
#define DMA_STALL 513u
#endif
#define LINE_COUNT 262u
#define MAX_NMI_LOG 200000u

typedef struct {
  Cpu6502 cpu;
  uint8_t ram[0x800];
  uint8_t prg[0x8000];
  Ppu ppu;
  Joypad pad1, pad2;
  Apu apu;

  uint64_t frame_base;   /* cycle at which the current frame began */
  unsigned frame_len;    /* length of the current frame, in CPU cycles */
  uint64_t frame;        /* index of the current frame */
  bool vblank_raised;    /* this frame's vblank point has been handled */
  bool hit_known;        /* sprite-0 hit computed for this frame's NMI */
  bool hit_present;
  uint64_t hit_cycle;    /* absolute cycle the sprite-0 flag rises */

  unsigned nmi_depth;
  uint64_t nmi_start[8]; /* absolute entry cycle of each open handler */
  unsigned nmi_frame[8];
  uint8_t *input;
  unsigned input_len;
  unsigned frames_total;

  FILE *nmi_out;
  FILE *ram_out;
  FILE *fb_out; /* optional: the renderer's frame at each boundary */
  bool sample_pending;   /* a frame has closed; its sample is not yet taken */
  uint64_t sample_at;    /* cycle at which that sample is taken */
  bool fb_pending;       /* the renderer's frame, at its own offset */
  uint64_t fb_at;

  /* NES_PROFILE: self cycles and calls per routine, inside NMI handlers */
  bool profiling;
  FILE *trace_out; /* NES_TRACE_RANGE=lo-hi: per-execution cycles for NMI code in range */
  unsigned trace_lo, trace_hi;
  uint64_t self_cycles[0x10000];
  uint64_t calls[0x10000];
  uint64_t pc_cycles[0x10000]; /* self cycles per instruction address */
  uint64_t pc_count[0x10000];
  uint16_t shadow[256];  /* routine stack: JSR targets and the handler entry */
  unsigned shadow_n;
  /* NES_PROFILE: cycles per top-level routine (called straight from the
   * handler body, callees included) for the NMI in progress */
  uint16_t top_addr[64];
  uint64_t top_cycles[64];
  unsigned top_n;
  FILE *top_out;
} Nes;

static Nes nes;

static uint8_t input_for(unsigned frame) {
  return frame < nes.input_len ? nes.input[frame] : 0;
}

static uint8_t bus_read(void *ctx, uint16_t addr) {
  Nes *n = ctx;
  if (addr < 0x2000u) return n->ram[addr & 0x7ffu];
  if (addr < 0x4000u) {
    uint8_t reg = (uint8_t)(addr & 7u);
    if (reg == 2u) {
      /* The sprite-0 hit is the renderer's, evaluated from the PPU state at
       * this read: the NMI's wait polls it while the game is still writing
       * scroll and sprites. It rises on its line of the frame that will be
       * drawn next — this frame while it is still in its visible lines, the
       * following frame once vblank has begun. */
      uint64_t frame_start = n->vblank_raised ? n->frame_base + FRAME_CYCLES
                                              : n->frame_base;
      int line = ppu_sprite0_hit_scanline(&n->ppu);
      n->ppu.sprite0 = line >= 0 &&
                       n->cpu.cycles >= frame_start +
                           (uint64_t)line * FRAME_CYCLES / LINE_COUNT;
    }
    return ppu_read(&n->ppu, reg);
  }
  if (addr == 0x4016u) return joypad_read(&n->pad1);
  if (addr == 0x4017u) return joypad_read(&n->pad2);
  if (addr < 0x4018u) return apu_read(&n->apu, addr);
  if (addr < 0x8000u) return 0;
  return n->prg[(addr - 0x8000u) & 0x7fffu];
}

static void bus_write(void *ctx, uint16_t addr, uint8_t value) {
  Nes *n = ctx;
  if (addr < 0x2000u) {
    n->ram[addr & 0x7ffu] = value;
    return;
  }
  if (addr < 0x4000u) {
    uint8_t reg = (uint8_t)(addr & 7u);
    bool was_on = (n->ppu.ctrl1 & 0x80u) != 0;
    ppu_write(&n->ppu, reg, value);
    /* enabling NMI while the vblank flag is up raises one at once */
    if (reg == 0u && !was_on && (value & 0x80u) && n->ppu.vblank) {
      n->cpu.nmi_requested = true;
    }
    return;
  }
  if (addr == 0x4014u) {
    /* OAM DMA: the 256-byte page from CPU RAM, with the 513-cycle stall */
    uint16_t base = (uint16_t)(value << 8);
    uint8_t page[256];
    for (unsigned i = 0; i < 256u; i++) page[i] = bus_read(ctx, (uint16_t)(base + i));
    ppu_oam_dma(&n->ppu, page);
    cpu6502_stall(&n->cpu, DMA_STALL);
    return;
  }
  if (addr == 0x4016u) {
    joypad_write(&n->pad1, value);
    joypad_write(&n->pad2, value);
    return;
  }
  if (addr < 0x4018u) {
    apu_write(&n->apu, addr, value);
  }
}

/* Advance the raster to the CPU clock: raise vblank (and NMI) at its point,
 * and close frames at the frame length. */
static void advance_raster(Nes *n) {
  for (;;) {
    if (n->sample_pending && n->cpu.cycles >= n->sample_at) {
      n->sample_pending = false;
      if (n->ram_out) fwrite(n->ram, 1, sizeof(n->ram), n->ram_out);
    }
    if (n->fb_pending && n->cpu.cycles >= n->fb_at) {
      n->fb_pending = false;
      if (n->fb_out) {
        static uint8_t fb[SMBC_RENDER_PIXELS];
        ppu_render_frame(&n->ppu, fb, NULL);
        fwrite(fb, 1, sizeof(fb), n->fb_out);
      }
    }
    if (!n->vblank_raised && n->cpu.cycles >= n->frame_base + VBLANK_CYCLE) {
      n->vblank_raised = true;
      n->hit_known = false;
      ppu_enter_vblank(&n->ppu);
      if (n->ppu.ctrl1 & 0x80u) n->cpu.nmi_requested = true;
      continue;
    }
    if (n->cpu.cycles >= n->frame_base + n->frame_len) {
      ppu_end_frame(&n->ppu);
      /* sampled at the frame boundary, even inside a running handler: that is
       * where the emulator's frame boundary falls (checked against nes-py) */
      n->sample_pending = true;
      n->sample_at = n->frame_base + n->frame_len + SAMPLE_SHIFT;
      n->fb_pending = true;
      n->fb_at = n->frame_base + n->frame_len + PIXEL_SHIFT;
      n->frame_base += n->frame_len;
      n->frame++;
#ifdef FRAME_ALTERNATE
      n->frame_len = (n->frame & 1u) ? FRAME_CYCLES + 1u : FRAME_CYCLES;
#endif
      n->vblank_raised = false;
      if (n->frame < n->frames_total) {
        joypad_set_buttons(&n->pad1, input_for((unsigned)n->frame));
      }
      continue;
    }
    break;
  }
}

static void dump_nmi(Nes *n, uint64_t entry, unsigned frame, uint64_t cost,
                     unsigned depth) {
  if (!n->nmi_out) return;
  fprintf(n->nmi_out, "%u,%llu,%llu,%u\n", frame,
          (unsigned long long)entry, (unsigned long long)cost, depth);
}

/* Attribute the cycles of one instruction executed inside a handler to the
 * routine on top of the shadow stack. */
static void add_top(Nes *n, uint16_t addr, uint64_t cost) {
  for (unsigned i = 0; i < n->top_n; i++) {
    if (n->top_addr[i] == addr) {
      n->top_cycles[i] += cost;
      return;
    }
  }
  if (n->top_n < 64) {
    n->top_addr[n->top_n] = addr;
    n->top_cycles[n->top_n++] = cost;
  }
}

static void profile_step(Nes *n, uint8_t op, uint16_t pc, uint64_t cost) {
  if (n->shadow_n == 0) return;
  if (n->shadow_n >= 2) add_top(n, n->shadow[1], cost); /* inclusive of callees */
  else if (n->shadow_n == 1) add_top(n, n->shadow[0], cost);
  n->pc_cycles[pc] += cost;
  n->pc_count[pc]++;
  n->self_cycles[n->shadow[n->shadow_n - 1]] += cost;
  if (op == 0x20u) { /* JSR: enter the target */
    uint16_t target = (uint16_t)(n->prg[(pc + 1u - 0x8000u) & 0x7fffu] |
                                 (n->prg[(pc + 2u - 0x8000u) & 0x7fffu] << 8));
    if (n->shadow_n < 256) n->shadow[n->shadow_n++] = target;
    n->calls[target]++;
  } else if (op == 0x60u && n->shadow_n > 1) { /* RTS: leave the routine */
    n->shadow_n--;
  }
}

static void dump_profile(const Nes *n, const char *prefix) {
  char path[512];
  snprintf(path, sizeof(path), "%s-pc.csv", prefix);
  FILE *pcf = fopen(path, "w");
  if (pcf) {
    fprintf(pcf, "address,cycles,executions\n");
    for (unsigned a = 0; a < 0x10000u; a++) {
      if (n->pc_count[a] != 0) {
        fprintf(pcf, "%04x,%llu,%llu\n", a,
                (unsigned long long)n->pc_cycles[a],
                (unsigned long long)n->pc_count[a]);
      }
    }
    fclose(pcf);
  }
  snprintf(path, sizeof(path), "%s-routines.csv", prefix);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "address,self_cycles,calls\n");
  for (unsigned a = 0; a < 0x10000u; a++) {
    if (n->self_cycles[a] != 0 || n->calls[a] != 0) {
      fprintf(f, "%04x,%llu,%llu\n", a,
              (unsigned long long)n->self_cycles[a],
              (unsigned long long)n->calls[a]);
    }
  }
  fclose(f);
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: nes_trace rom.nes inputs.bin frames out_prefix\n");
    return 2;
  }
  FILE *rom = fopen(argv[1], "rb");
  if (!rom) { perror(argv[1]); return 1; }
  uint8_t header[16];
  if (fread(header, 1, 16, rom) != 16 || memcmp(header, "NES\x1a", 4) != 0) {
    fprintf(stderr, "not an iNES file\n");
    return 1;
  }
  if (header[4] != 2 || (header[6] >> 4) != 0 || (header[6] & 0x08u)) {
    fprintf(stderr, "only NROM-256 (mapper 0, 32 KB PRG) is modelled\n");
    return 1;
  }
  if (fread(nes.prg, 1, sizeof(nes.prg), rom) != sizeof(nes.prg)) {
    fprintf(stderr, "short PRG\n");
    return 1;
  }
  fclose(rom);

  FILE *in = fopen(argv[2], "rb");
  if (!in) { perror(argv[2]); return 1; }
  nes.input = malloc(1u << 20);
  nes.input_len = (unsigned)fread(nes.input, 1, 1u << 20, in);
  fclose(in);
  nes.frames_total = (unsigned)strtoul(argv[3], NULL, 10);

  char path[512];
  snprintf(path, sizeof(path), "%s-nmi.csv", argv[4]);
  nes.nmi_out = fopen(path, "w");
  snprintf(path, sizeof(path), "%s-ram.bin", argv[4]);
  nes.ram_out = fopen(path, "wb");
  if (getenv("NES_FRAMEBUFFER")) {
    snprintf(path, sizeof(path), "%s-fb.bin", argv[4]);
    nes.fb_out = fopen(path, "wb");
  }
  if (!nes.nmi_out || !nes.ram_out) { fprintf(stderr, "cannot write output\n"); return 1; }
  fprintf(nes.nmi_out, "frame,start,cost,depth\n");
  nes.profiling = getenv("NES_PROFILE") != NULL;
  if (getenv("NES_TRACE_RANGE")) {
    sscanf(getenv("NES_TRACE_RANGE"), "%x-%x", &nes.trace_lo, &nes.trace_hi);
    snprintf(path, sizeof(path), "%s-trace.csv", argv[4]);
    nes.trace_out = fopen(path, "w");
  }
  if (nes.profiling) {
    snprintf(path, sizeof(path), "%s-nmi-routines.csv", argv[4]);
    nes.top_out = fopen(path, "w");
    if (nes.top_out) fprintf(nes.top_out, "frame,routine,cycles\n");
  }

  nes.frame_len = FRAME_CYCLES;
#ifdef FRAME_ALTERNATE
  nes.frame_len = FRAME_CYCLES; /* frame 0 is even */
#endif
  ppu_init(&nes.ppu);
  ppu_set_chr(&nes.ppu, SMBC_CHR_ROM);
  joypad_init(&nes.pad1);
  joypad_init(&nes.pad2);
  apu_init(&nes.apu);
  Cpu6502Bus bus = {bus_read, bus_write, &nes};
  cpu6502_reset(&nes.cpu, bus);
  joypad_set_buttons(&nes.pad1, input_for(0));

  while (nes.frame < nes.frames_total) {
    advance_raster(&nes);
    if (nes.frame >= nes.frames_total) break;

    bool entering_nmi = nes.cpu.nmi_requested;
    uint8_t next_op = nes.prg[(nes.cpu.pc - 0x8000u) & 0x7fffu];
    bool leaving_nmi = nes.nmi_depth > 0 && next_op == 0x40u && nes.cpu.pc >= 0x8000u;

    if (entering_nmi && nes.nmi_depth < 8) {
      nes.nmi_start[nes.nmi_depth] = nes.cpu.cycles;
      nes.nmi_frame[nes.nmi_depth] = (unsigned)nes.frame;
    }
    uint64_t before = nes.cpu.cycles;
    uint16_t pc_before = nes.cpu.pc;
    uint8_t op_before = nes.prg[(pc_before - 0x8000u) & 0x7fffu];
    if (entering_nmi && nes.profiling) {
      nes.shadow_n = 0;
      nes.top_n = 0;
      uint16_t vector = (uint16_t)(nes.prg[0x7ffau] | (nes.prg[0x7ffbu] << 8));
      nes.shadow[nes.shadow_n++] = vector;
      nes.calls[vector]++;
    }
    cpu6502_step(&nes.cpu);
    if (nes.trace_out && nes.nmi_depth > 0 && !entering_nmi &&
        pc_before >= nes.trace_lo && pc_before <= nes.trace_hi) {
      fprintf(nes.trace_out, "%u,%04x,%02x,%llu\n", (unsigned)nes.frame, pc_before,
              op_before, (unsigned long long)(nes.cpu.cycles - before));
    }
    if (nes.profiling && (nes.nmi_depth > 0 || entering_nmi)) {
      uint64_t cost = nes.cpu.cycles - before;
      if (entering_nmi) {
        nes.self_cycles[nes.shadow[0]] += cost; /* the interrupt entry */
      } else {
        profile_step(&nes, op_before, pc_before, cost);
      }
    }
    if (leaving_nmi) {
      if (nes.profiling && nes.top_out) {
        for (unsigned i = 0; i < nes.top_n; i++) {
          fprintf(nes.top_out, "%llu,%04x,%llu\n", (unsigned long long)nes.frame,
                  nes.top_addr[i], (unsigned long long)nes.top_cycles[i]);
        }
      }
      nes.top_n = 0;
      nes.shadow_n = 0;
    }
    if (nes.cpu.jammed) {
      fprintf(stderr, "illegal opcode %02x at %04x (frame %llu)\n",
              nes.cpu.jam_opcode, nes.cpu.jam_pc,
              (unsigned long long)nes.frame);
      return 1;
    }
    if (entering_nmi) {
      nes.nmi_depth++;
    }
    if (leaving_nmi) {
      nes.nmi_depth--;
      unsigned d = nes.nmi_depth;
      uint64_t start = nes.nmi_start[d];
      uint64_t cost = nes.cpu.cycles - start;
      unsigned frame = nes.nmi_frame[d];
      dump_nmi(&nes, start - (frame * (uint64_t)FRAME_CYCLES), frame, cost, d);
    }
  }
  if (nes.profiling) dump_profile(&nes, argv[4]);
  if (nes.top_out) fclose(nes.top_out);
  fclose(nes.nmi_out);
  fclose(nes.ram_out);
  if (nes.fb_out) fclose(nes.fb_out);
  free(nes.input);
  return 0;
}

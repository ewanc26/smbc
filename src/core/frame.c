/* core/frame.c — see core/frame.h for the contract. */
#include "frame.h"

#include "../data/chr.h"
#include "boot.h"
#include "nmi.h"

void smbc_init(Smbc *smbc) {
  state_init(&smbc->state);
  joypad_init(&smbc->joypad1);
  joypad_init(&smbc->joypad2);
  ppu_init(&smbc->ppu);
  ppu_set_chr(&smbc->ppu, SMBC_CHR_ROM); /* the donor cartridge's pattern tables */
  apu_init(&smbc->apu);
  smbc->nmi_owed_cycles = 0;
  boot_run(smbc);
}

void smbc_charge_cycles(Smbc *smbc, uint32_t cycles) {
  smbc->ppu.cycle += cycles;
}

bool nmi_timeline_runs(uint32_t owed) {
  /* the handler still running at this frame's vblank point blocks it */
  return owed <= SMBC_CPU_CYCLES_TO_VBLANK;
}

uint32_t nmi_timeline_carry(uint32_t end) {
  return end > SMBC_CPU_CYCLES_PER_FRAME ? end - SMBC_CPU_CYCLES_PER_FRAME : 0u;
}

void smbc_frame(Smbc *smbc) {
  /* the timing contract (hw/ppu.h): one call is one NTSC frame. The
   * clock enters vblank, the translated NMI runs — its sprite-0 waits
   * advance the clock through the pre-render line and the hit — and
   * whatever remains runs out to the frame's end. */
  ppu_enter_vblank(&smbc->ppu);

  /* smb.asm: NonMaskableInterrupt — the handler clears NMIs on entry, so a
   * vblank that lands inside a still-running handler raises no NMI and the
   * handler does not run for it. Where it runs, its cost (charged as it
   * goes) decides how far into the next frame it overruns. */
  uint32_t end;
  if (nmi_timeline_runs(smbc->nmi_owed_cycles)) {
    nmi_run(smbc);
    end = smbc->ppu.cycle; /* the clock, through the handler's charges and waits */
  } else {
    end = smbc->nmi_owed_cycles;
  }
  smbc->nmi_owed_cycles = nmi_timeline_carry(end);

  ppu_end_frame(&smbc->ppu);
}

/* tools/frame_dump.c — dev tool for the emulator comparison (#31).
 *
 * Runs the port from power-on over an input file (one button byte per
 * frame, hw/joypad.h layout) and writes one 256×240 frame of 2C02 palette
 * indices per frame, counted from power-on. Frames before the reset ends
 * are blank: the power-on screen shows palette index $00 everywhere, as
 * the reference emulator does while the reset polls VBlank.
 *
 *   frame_dump input.bin output.bin frames
 *
 * Not part of the game build: nothing under core/, hw/ or data/ depends
 * on it. */
#include <stdio.h>
#include <stdlib.h>

#include "../src/core/boot.h"
#include "../src/core/frame.h"
#include "../src/hw/joypad.h"
#include "../src/hw/render.h"

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: frame_dump input.bin output.bin frames\n");
    return 2;
  }
  FILE *in = fopen(argv[1], "rb");
  FILE *out = fopen(argv[2], "wb");
  if (in == NULL || out == NULL) {
    fprintf(stderr, "frame_dump: cannot open %s or %s\n", argv[1], argv[2]);
    return 1;
  }
  unsigned frames = (unsigned)strtoul(argv[3], NULL, 10);

  static uint8_t fb[SMBC_RENDER_PIXELS];
  static Smbc smbc;
  smbc_init(&smbc);
  for (unsigned f = 0; f < frames; f++) {
    int b = fgetc(in);
    if (f < smbc.reset_frames) {
      for (unsigned i = 0; i < SMBC_RENDER_PIXELS; i++) fb[i] = 0x00u;
    } else {
      joypad_set_buttons(&smbc.joypad1, (uint8_t)(b < 0 ? 0 : b));
      smbc_frame(&smbc);
      ppu_render_frame(&smbc.ppu, fb, NULL);
    }
    fwrite(fb, 1, SMBC_RENDER_PIXELS, out);
  }
  fclose(in);
  fclose(out);
  return 0;
}

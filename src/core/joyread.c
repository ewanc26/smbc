/* core/joyread.c — see core/joyread.h for the contract. */
#include "joyread.h"

#include "../hw/joypad.h"
#include "frame.h"
#include "state.h"

/* smb.asm: ReadPortBits — eight serial reads, first bit (A) ending in d7.
 * The port value's d0 and d1 are OR'd together, as the original does for
 * old Famicom systems; our hw model only ever drives d0. */
static uint32_t read_port_bits(Smbc *smbc, int port) {
  /* ldy; the eight-bit loop (lda abs,x; sta; lsr; ora; lsr; pla; rol; dey;
   * bne), with pha before and after each bit: 2 + 8*25 + 7*3 + 2 */
  uint32_t cycles = 2u + 8u * 25u + 7u * 3u + 2u;
  Joypad *pad = port == 0 ? &smbc->joypad1 : &smbc->joypad2;
  uint8_t bits = 0;
  for (int i = 0; i < 8; i++) {
    uint8_t v = joypad_read(pad);
    bits = (uint8_t)((bits << 1) | ((v | (v >> 1)) & 0x01u));
  }
  CoreState *state = &smbc->state;
  state->ram[SMBC_SAVED_JOYPAD1_BITS + port] = bits;
  /* sta SavedJoypadBits,x; pha; and; and JoypadBitMask,x */
  cycles += 5u + 3u + 2u + 4u;
  if ((bits & 0x30u & state->ram[SMBC_JOYPAD_BIT_MASK + port]) != 0) {
    /* select or start seen twice running: strip them this read */
    state->ram[SMBC_SAVED_JOYPAD1_BITS + port] = bits & 0xcfu;
    return cycles + 2u + 4u + 2u + 5u + 6u; /* beq, not taken; pla; and; sta; rts */
  }
  state->ram[SMBC_JOYPAD_BIT_MASK + port] = bits;
  return cycles + 3u + 4u + 5u + 6u; /* beq Save8Bits, taken; pla; sta; rts */
}

uint32_t read_joypads(Smbc *smbc) {
  /* smb.asm: ReadJoypads — one strobe pair clocks both ports; the second
   * port is read by falling through from the first call's return */
  joypad_write(&smbc->joypad1, 0x01u);
  joypad_write(&smbc->joypad2, 0x01u);
  joypad_write(&smbc->joypad1, 0x00u);
  joypad_write(&smbc->joypad2, 0x00u);
  uint32_t cycles = 2u + 4u + 2u + 2u + 4u; /* lda; sta; lsr; tax; sta */
  cycles += 6u + read_port_bits(smbc, 0); /* jsr ReadPortBits */
  cycles += 2u; /* inx */
  return cycles + read_port_bits(smbc, 1);
}

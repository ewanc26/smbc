/* core/pause.c — see core/pause.h for the contract. */
#include "pause.h"

#include "frame.h"
#include "state.h"

/* smb.asm: PauseRoutine. Each return charges the cycles of the ROM path it
 * takes (branch-taken penalties included; no branch in this routine crosses
 * a page). */
void pause_routine(Smbc *smbc) {
  CoreState *state = &smbc->state;
  uint8_t mode = state->ram[SMBC_OPER_MODE];
  uint32_t cycles = 4u + 2u; /* lda OperMode; cmp #VictoryModeValue */

  if (mode == SMBC_MODE_VICTORY) {
    cycles += 3u; /* beq ChkPauseTimer, taken */
  } else {
    cycles += 2u + 2u; /* beq not taken; cmp #GameModeValue */
    if (mode != SMBC_MODE_GAME) {
      smbc_charge_cycles(smbc, cycles + 3u + 6u); /* bne ExitPause; rts */
      return;
    }
    cycles += 2u + 4u + 2u; /* bne not taken; lda OperMode_Task; cmp #$03 */
    if (state->ram[SMBC_OPER_MODE_TASK] != 0x03) {
      smbc_charge_cycles(smbc, cycles + 3u + 6u); /* bne ExitPause; rts */
      return;
    }
    cycles += 2u; /* bne not taken */
  }

  cycles += 4u; /* ChkPauseTimer: lda GamePauseTimer */
  if (state->ram[SMBC_GAME_PAUSE_TIMER] != 0) {
    state->ram[SMBC_GAME_PAUSE_TIMER]--;
    smbc_charge_cycles(smbc, cycles + 2u + 6u + 6u); /* beq; dec; rts */
    return;
  }

  cycles += 3u + 4u + 2u; /* beq ChkStart, taken; lda SavedJoypad1Bits; and */
  if ((state->ram[SMBC_SAVED_JOYPAD1_BITS] & SMBC_PAD_START) == 0) {
    state->ram[SMBC_GAME_PAUSE_STATUS] &= 0x7fu; /* ClrPauseTimer */
    smbc_charge_cycles(smbc, cycles + 3u + 4u + 2u + 4u + 6u);
    return;                                      /* beq; lda; and; sta; rts */
  }

  cycles += 2u + 4u + 2u; /* beq not taken; lda GamePauseStatus; and #$80 */
  if ((state->ram[SMBC_GAME_PAUSE_STATUS] & 0x80u) != 0) {
    smbc_charge_cycles(smbc, cycles + 3u + 6u); /* bne ExitPause; rts */
    return;
  }

  state->ram[SMBC_GAME_PAUSE_TIMER] = 0x2bu;
  uint8_t status = state->ram[SMBC_GAME_PAUSE_STATUS];
  state->ram[SMBC_PAUSE_SOUND_QUEUE] = (uint8_t)(status + 1); /* tay; iny; sty */
  state->ram[SMBC_GAME_PAUSE_STATUS] = (uint8_t)((status ^ 0x01u) | 0x80u);
  /* bne SetPause, taken; sta; rts */
  smbc_charge_cycles(smbc, cycles + 2u + 2u + 4u + 4u + 2u + 2u + 4u + 2u + 2u +
                               3u + 4u + 6u);
}

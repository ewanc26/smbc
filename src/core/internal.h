/* core/internal.h — shared declarations private to the core scope.
 *
 * Holds the sound engine's cross-atom internals: the register/frequency
 * helpers used by both the sound effect handlers (core/sfx) and the music
 * sequencer (core/music), and the entry points core/sound calls into those
 * atoms. Not a public interface — only the core scope includes this.
 */
#ifndef SMBC_CORE_INTERNAL_H
#define SMBC_CORE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

/* smb.asm: FreqRegLookupTbl + MusicLengthLookupTbl, read as one run. A
 * value < 102 is the frequency table; 102..149 spills into the length
 * table; anything past that is past the table (returns 0). */
uint8_t sound_freq_lookup(uint8_t index);

/* smb.asm: Dump_Freq_Regs — write low byte to $400x+2 and high byte (with
 * the length bit) to $400x+3 from FreqRegLookupTbl[a]. Returns false when
 * the table entry is zero (no tone), matching the Z flag the callers test. */
bool sound_dump_freq_regs(Smbc *smbc, uint8_t x, uint8_t a);

/* cycles of Dump_Freq_Regs for index a: 30 when a tone is written, 15 when not,
 * plus one when the $FF01,y read carries past the page (a == $FF) */
uint32_t sound_freq_dump_cycles(uint8_t a, bool wrote);

/* smb.asm: Dump_Squ1_Regs / Dump_Sq2_Regs — control ($4000/$4001) and
 * ($4004/$4005) writes used by the music release path. */
void sound_dump_squ1_regs(Smbc *smbc, uint8_t x, uint8_t y);
void sound_dump_sq2_regs(Smbc *smbc, uint8_t x, uint8_t y);

/* smb.asm: PlaySqu1Sfx / PlaySqu2Sfx — set control bytes then the
 * frequency registers. Returns whether a tone was produced. */
bool sound_play_squ1_sfx(Smbc *smbc, uint8_t a, uint8_t x, uint8_t y);
bool sound_play_squ2_sfx(Smbc *smbc, uint8_t a, uint8_t x, uint8_t y);

/* smb.asm: SetFreq_Squ1 / SetFreq_Squ2 / SetFreq_Tri */
bool sound_set_freq_squ1(Smbc *smbc, uint8_t a);
bool sound_set_freq_squ2(Smbc *smbc, uint8_t a);
bool sound_set_freq_tri(Smbc *smbc, uint8_t a);

/* smb.asm: StopSquare1Sfx / StopSquare2Sfx */
void sound_stop_square1_sfx(Smbc *smbc);
void sound_stop_square2_sfx(Smbc *smbc);

/* core/sfx.c — the three channel sound-effect handlers, called from
 * run_sound_subroutines. */
uint32_t sfx_square1_handler(Smbc *smbc);
uint32_t sfx_square2_handler(Smbc *smbc);
uint32_t sfx_noise_handler(Smbc *smbc);

/* core/sfx.c — jsr PlaySqu1Sfx with its cost, shared with the pause tone */
uint32_t sfx_play_squ1(Smbc *smbc, uint8_t a, uint8_t x, uint8_t y);

/* core/music.c — smb.asm: MusicHandler, returns its cycles including rts */
uint32_t sound_music_handler(Smbc *smbc);

#endif /* SMBC_CORE_INTERNAL_H */

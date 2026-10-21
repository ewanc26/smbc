/* core/sound.h — the sound engine.
 *
 * Owns: music and sound effect processing against the APU register image.
 * The engine runs once per frame from the NMI handler (smb.asm: SoundEngine):
 * it dispatches queued sound effects to the two pulse channels and the noise
 * channel, drives the music sequencer, and writes the resulting $4000-$4017
 * register values into the hw/apu register capture.
 *
 * The queue bytes are the game's own sound constants, written from core code
 * as it is translated; this header names them.
 */
#ifndef SMBC_CORE_SOUND_H
#define SMBC_CORE_SOUND_H

#include <stdint.h>

struct Smbc;

/* smb.asm: SoundEngine — one frame of sound processing. Returns the CPU
 * cycles charged so far for the path taken. */
uint32_t sound_engine(struct Smbc *smbc);

/* smb.asm: square 1 sound queue bits (Sfx_*) */
#define SMBC_SFX_SMALL_JUMP       0x80u /* smb.asm: Sfx_SmallJump */
#define SMBC_SFX_FLAGPOLE         0x40u /* smb.asm: Sfx_Flagpole */
#define SMBC_SFX_FIREBALL         0x20u /* smb.asm: Sfx_Fireball */
#define SMBC_SFX_PIPE_DOWN_INJURY 0x10u /* smb.asm: Sfx_PipeDown_Injury */
#define SMBC_SFX_ENEMY_SMACK      0x08u /* smb.asm: Sfx_EnemySmack */
#define SMBC_SFX_ENEMY_STOMP      0x04u /* smb.asm: Sfx_EnemyStomp */
#define SMBC_SFX_BUMP             0x02u /* smb.asm: Sfx_Bump */
#define SMBC_SFX_BIG_JUMP         0x01u /* smb.asm: Sfx_BigJump */

/* smb.asm: square 2 sound queue bits (Sfx_*) */
#define SMBC_SFX_BOWSER_FALL    0x80u /* smb.asm: Sfx_BowserFall */
#define SMBC_SFX_EXTRA_LIFE     0x40u /* smb.asm: Sfx_ExtraLife */
#define SMBC_SFX_POWER_UP_GRAB  0x20u /* smb.asm: Sfx_PowerUpGrab */
#define SMBC_SFX_TIMER_TICK     0x10u /* smb.asm: Sfx_TimerTick */
#define SMBC_SFX_BLAST          0x08u /* smb.asm: Sfx_Blast */
#define SMBC_SFX_GROW_VINE      0x04u /* smb.asm: Sfx_GrowVine */
#define SMBC_SFX_GROW_POWER_UP  0x02u /* smb.asm: Sfx_GrowPowerUp */
#define SMBC_SFX_COIN_GRAB      0x01u /* smb.asm: Sfx_CoinGrab */

/* smb.asm: noise sound queue bits (Sfx_*) */
#define SMBC_SFX_BOWSER_FLAME 0x02u /* smb.asm: Sfx_BowserFlame */
#define SMBC_SFX_BRICK_SHATTER 0x01u /* smb.asm: Sfx_BrickShatter */

#endif /* SMBC_CORE_SOUND_H */

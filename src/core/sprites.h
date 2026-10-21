/* core/sprites.h — sprite (OAM) housekeeping.
 *
 * Owns: moving sprites offscreen and the per-frame sprite data offset
 * shuffle the game uses to distribute sprite-0-priority flicker.
 */
#ifndef SMBC_CORE_SPRITES_H
#define SMBC_CORE_SPRITES_H

#include "state.h"

/* smb.asm: MoveAllSpritesOffscreen — all 64 sprites. */
void move_all_sprites_offscreen(CoreState *state);

/* smb.asm: MoveSpritesOffscreen — all but sprite 0. */
void move_sprites_offscreen(CoreState *state);

/* smb.asm: SpriteShuffler — rotates the sprite data offsets by the amounts
 * in SprShuffleAmt (RAM, seeded by SecondaryGameSetup, still pending). */
uint32_t sprite_shuffler(CoreState *state);

#endif /* SMBC_CORE_SPRITES_H */

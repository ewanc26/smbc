/* data/areadata.h — the game's level layouts.
 *
 * Owns: every enemy object stream (smb.asm's E_* areas, terminated by
 * $ff) and level object stream (L_* areas, terminated by $fd), the two
 * pointer tables that index them (EnemyDataAddr / AreaDataAddr — pointer
 * tables in the port rather than the asm's low/high byte splits), and
 * the high-byte offset bases the area type selects between
 * (EnemyAddrHOffsets / AreaDataHOffsets). E_GroundArea9 shares its
 * terminator with E_GroundArea10 in the asm; here each array carries
 * its own, which the decoder reads identically. Consumed by
 * core/parser's GetAreaDataAddrs and ProcessAreaData.
 */
#ifndef SMBC_DATA_AREADATA_H
#define SMBC_DATA_AREADATA_H

#include <stdint.h>

#define SMBC_ENEMY_AREAS_COUNT    34u
#define SMBC_LEVEL_AREAS_COUNT    34u
#define SMBC_ADDR_H_OFFSETS_SIZE  4u

/* smb.asm: EnemyDataAddr — object streams for the 34 enemy layouts, in
 * the asm's table order (castles, grounds, undergrounds, waters) */
extern const uint8_t *const SMBC_ENEMY_AREAS[SMBC_ENEMY_AREAS_COUNT];

/* smb.asm: AreaDataAddr — object streams for the 34 level layouts, in
 * the asm's table order (waters, grounds, undergrounds, castles) */
extern const uint8_t *const SMBC_LEVEL_AREAS[SMBC_LEVEL_AREAS_COUNT];

/* smb.asm: AreaDataAddrLow + 2, the low byte of each level stream's
 * pointer as ProcessAreaData reads through it (the ROM skips the header
 * by adding 2); only its carry out matters, for the page-cross cycles of
 * the decoder's indirect reads */
extern const uint8_t SMBC_LEVEL_AREA_PTR_LO[SMBC_LEVEL_AREAS_COUNT];

/* smb.asm: EnemyAddrHOffsets — table base per area type (ground, water,
 * underground, castle after the type bits rotate down) */
extern const uint8_t SMBC_ENEMY_ADDR_H_OFFSETS[SMBC_ADDR_H_OFFSETS_SIZE];

/* smb.asm: AreaDataHOffsets */
extern const uint8_t SMBC_AREA_DATA_H_OFFSETS[SMBC_ADDR_H_OFFSETS_SIZE];

#endif /* SMBC_DATA_AREADATA_H */

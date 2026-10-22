/* core/parser.h — the area parser: drawing the level's column sets.
 *
 * Owns: the AreaParserTaskHandler task tree (smb.asm's eight-task cycle
 * of column position bookkeeping, area graphics and the object decoder),
 * IncrementColumnPos, RenderAreaGraphics, RenderAttributeTables and
 * AreaParserCore's scenery and terrain rendering. ProcessAreaData, the
 * object decoder, is charged; the object routines it dispatches to are
 * pending their own atom and their cycles are not yet counted.
 *
 * Collaborators: core/screen runs AreaParserTaskControl as screen task 8
 * behind a disabled screen; data/metatiles supplies the attribute bits
 * the block drawing fills AttributeBuffer with.
 */
#ifndef SMBC_CORE_PARSER_H
#define SMBC_CORE_PARSER_H

#include "state.h"

/* smb.asm: AreaParserTaskHandler — run one task of the eight-task cycle,
 * defaulting the counter to eight; when the cycle completes, flush the
 * attribute buffer into VRAM buffer 2. Returns the CPU cycles of the task
 * run, including the object decoder's and excluding the object routines'
 * (not yet charged). */
uint32_t area_parser_task_handler(CoreState *state);

/* smb.asm: KillEnemies — clear the enemy flag on every slot holding
 * the given identifier */
void kill_enemies(CoreState *state, uint8_t id);

#endif /* SMBC_CORE_PARSER_H */

/* data/objects.c — see data/objects.h for the contract. */
#include "objects.h"

/* smb.asm: FrenzyIDData */
const uint8_t SMBC_FRENZY_ID_DATA[3] = {0x14u, 0x17u, 0x18u};

/* smb.asm: PulleyRopeMetatiles */
const uint8_t SMBC_PULLEY_ROPE_METATILES[3] = {0x42u, 0x41u, 0x43u};

/* smb.asm: CastleMetatiles */
const uint8_t SMBC_CASTLE_METATILES[55] = {
    0x00u, 0x45u, 0x45u, 0x45u, 0x00u, 0x00u, 0x48u, 0x47u, 0x46u, 0x00u,
    0x45u, 0x49u, 0x49u, 0x49u, 0x45u, 0x47u, 0x47u, 0x4au, 0x47u, 0x47u,
    0x47u, 0x47u, 0x4bu, 0x47u, 0x47u, 0x49u, 0x49u, 0x49u, 0x49u, 0x49u,
    0x47u, 0x4au, 0x47u, 0x4au, 0x47u, 0x47u, 0x4bu, 0x47u, 0x4bu, 0x47u,
    0x47u, 0x47u, 0x47u, 0x47u, 0x47u, 0x4au, 0x47u, 0x4au, 0x47u, 0x4au,
    0x4bu, 0x47u, 0x4bu, 0x47u, 0x4bu};

/* smb.asm: SidePipeShaftData/SidePipeTopPart/SidePipeBottomPart */
const uint8_t SMBC_SIDE_PIPE_SHAFT_DATA[4] = {0x15u, 0x14u, 0x00u, 0x00u};
const uint8_t SMBC_SIDE_PIPE_TOP_PART[4] = {0x15u, 0x1eu, 0x1du, 0x1cu};
const uint8_t SMBC_SIDE_PIPE_BOTTOM_PART[4] = {0x15u, 0x21u, 0x20u, 0x1fu};

/* smb.asm: VerticalPipeData */
const uint8_t SMBC_VERTICAL_PIPE_DATA[8] = {0x11u, 0x10u, 0x15u, 0x14u,
                                            0x13u, 0x12u, 0x15u, 0x14u};

/* smb.asm: CoinMetatileData */
const uint8_t SMBC_COIN_METATILE_DATA[4] = {0xc3u, 0xc2u, 0xc2u, 0xc2u};

/* smb.asm: C_ObjectRow/C_ObjectMetatile */
const uint8_t SMBC_C_OBJECT_ROW[3] = {0x06u, 0x07u, 0x08u};
const uint8_t SMBC_C_OBJECT_METATILE[3] = {0xc5u, 0x0cu, 0x89u};

/* smb.asm: SolidBlockMetatiles */
const uint8_t SMBC_SOLID_BLOCK_METATILES[4] = {0x69u, 0x61u, 0x61u, 0x62u};

/* smb.asm: BrickMetatiles */
const uint8_t SMBC_BRICK_METATILES[5] = {0x22u, 0x51u, 0x52u, 0x52u, 0x88u};

/* smb.asm: StaircaseHeightData/StaircaseRowData */
const uint8_t SMBC_STAIRCASE_HEIGHT_DATA[9] = {0x07u, 0x07u, 0x06u, 0x05u,
                                               0x04u, 0x03u, 0x02u, 0x01u,
                                               0x00u};
const uint8_t SMBC_STAIRCASE_ROW_DATA[9] = {0x03u, 0x03u, 0x04u, 0x05u,
                                            0x06u, 0x07u, 0x08u, 0x09u,
                                            0x0au};

/* smb.asm: HoleMetatiles */
const uint8_t SMBC_HOLE_METATILES[4] = {0x87u, 0x00u, 0x00u, 0x00u};

/* smb.asm: BrickQBlockMetatiles */
const uint8_t SMBC_BRICK_Q_BLOCK_METATILES[14] = {
    0xc1u, 0xc0u, 0x5fu, 0x60u, /* question blocks */
    0x55u, 0x56u, 0x57u, 0x58u, 0x59u, /* ground level types */
    0x5au, 0x5bu, 0x5cu, 0x5du, 0x5eu}; /* other level types */

/* smb.asm: XOffscreenBitsData */
const uint8_t SMBC_X_OFFSCREEN_BITS_DATA[16] = {
    0x7fu, 0x3fu, 0x1fu, 0x0fu, 0x07u, 0x03u, 0x01u, 0x00u,
    0x80u, 0xc0u, 0xe0u, 0xf0u, 0xf8u, 0xfcu, 0xfeu, 0xffu};

/* smb.asm: YOffscreenBitsData */
const uint8_t SMBC_Y_OFFSCREEN_BITS_DATA[9] = {
    0x00u, 0x08u, 0x0cu, 0x0eu, 0x0fu, 0x07u, 0x03u, 0x01u, 0x00u};

/* smb.asm: DefaultXOnscreenOfs */
const uint8_t SMBC_DEFAULT_X_ONSCREEN_OFS[3] = {0x07u, 0x0fu, 0x07u};

/* smb.asm: DefaultYOnscreenOfs */
const uint8_t SMBC_DEFAULT_Y_ONSCREEN_OFS[3] = {0x04u, 0x00u, 0x04u};

/* smb.asm: HighPosUnitData */
const uint8_t SMBC_HIGH_POS_UNIT_DATA[2] = {0xffu, 0x00u};

/* smb.asm: XSpeedAdderData */
const uint8_t SMBC_X_SPEED_ADDER_DATA[4] = {0x00u, 0xe8u, 0x00u, 0x18u};

/* smb.asm: RevivedXSpeed */
const uint8_t SMBC_REVIVED_X_SPEED[4] = {0x08u, 0xf8u, 0x0cu, 0xf4u};

/* smb.asm: BlooberBitmasks */
const uint8_t SMBC_BLOOBER_BITMASKS[2] = {0x3fu, 0x03u};

/* smb.asm: SwimCCXMoveData (the residual $04, $04 pair omitted) */
const uint8_t SMBC_SWIM_CC_X_MOVE_DATA[2] = {0x40u, 0x80u};

/* smb.asm: HammerEnemyOfsData */
const uint8_t SMBC_HAMMER_ENEMY_OFS_DATA[9] = {0x04u, 0x04u, 0x04u,
                                               0x05u, 0x05u, 0x05u,
                                               0x06u, 0x06u, 0x06u};

/* smb.asm: HammerThrowTmrData */
const uint8_t SMBC_HAMMER_THROW_TMR_DATA[2] = {0x30u, 0x1cu};

/* smb.asm: HammerBroJumpLData */
const uint8_t SMBC_HAMMER_BRO_JUMP_L_DATA[2] = {0x20u, 0x37u};

/* smb.asm: PRandomSubtracter, with FlyCCBPriority visible past the
 * fifth entry as on the cartridge */
const uint8_t SMBC_PRANDOM_SUBTRACTER[10] = {0xf8u, 0xa0u, 0x70u, 0xbdu, 0x00u,
                                             0x20u, 0x20u, 0x20u, 0x00u, 0x00u};

/* smb.asm: FlyCCBPriority */
const uint8_t SMBC_FLY_CC_B_PRIORITY[5] = {0x20u, 0x20u, 0x20u, 0x00u, 0x00u};

/**
*  jax-gemma bridge for yrpp-spawner  (GPLv3, same as the spawner)
*
*  Phase 1: OBSERVATION bridge. Each logic frame, serialize the agent house's
*  fog-honored state into a named shared-memory region for the Python/JAX side.
*  ACT / EventClass injection is Phase 2.
*
*  Contract mirror: ../../../docs/bridge-contract.md and yr_env/contract.py
*/
#pragma once
#include <cstdint>

namespace BridgeContract
{
    constexpr uint32_t MAGIC   = 0x59524252u; // 'YRBR'
    constexpr uint32_t VERSION = 1u;
    constexpr int      N_OWN   = 256;
    constexpr int      N_ENEMY = 256;
    constexpr char     OBS_NAME[] = "Local\\yr_bridge_obs";
}

#pragma pack(push, 1)

struct BridgeHeader
{
    uint32_t magic;        // BridgeContract::MAGIC
    uint32_t version;      // BridgeContract::VERSION
    uint64_t frame_seq;    // Unsorted::CurrentFrame
    int32_t  house_index;  // agent HouseClass::ArrayIndex
    uint32_t status;       // bit0 game_over, bit1 win, bit2 loss, bit3 paused (Phase 1: 0)
};

struct BridgeGlobals
{
    int32_t credits;          // HouseClass::Balance
    int32_t power_output;     // HouseClass::PowerOutput
    int32_t power_drain;      // HouseClass::PowerDrain
    int32_t side_index;       // HouseClass::SideIndex
    int32_t owned_units;      // HouseClass::OwnedUnits
    int32_t owned_buildings;  // HouseClass::OwnedBuildings
    int32_t owned_infantry;   // HouseClass::OwnedInfantry
    int32_t owned_aircraft;   // HouseClass::OwnedAircraft
    int32_t owned_navy;       // derived: own technos with TechnoType->Naval
};

struct BridgeEntity
{
    int32_t type_id;   // GetTechnoType()->ArrayIndex
    int16_t x, y;      // GetMapCoords() cell coords
    uint8_t hp_frac;   // GetHealthPercentage()*255
    uint8_t state;     // Phase 1: 0 (coarse state deferred)
    uint8_t group_id;  // Phase 1: 0 (groups are Phase 2)
    uint8_t cooldown;  // Phase 1: 0
};

struct BridgeOBS
{
    BridgeHeader  header;
    BridgeGlobals globals;
    uint16_t      n_own;
    uint16_t      n_enemy;
    BridgeEntity  own[BridgeContract::N_OWN];
    BridgeEntity  enemy[BridgeContract::N_ENEMY];
};

#pragma pack(pop)

// Layout guards — Python unpacks these exact sizes (bump VERSION on any change).
static_assert(sizeof(BridgeHeader) == 24, "BridgeHeader layout changed");
static_assert(sizeof(BridgeGlobals) == 36, "BridgeGlobals layout changed");
static_assert(sizeof(BridgeEntity) == 12, "BridgeEntity layout changed");
static_assert(sizeof(BridgeOBS) == 24 + 36 + 4 + 256 * 12 + 256 * 12, "BridgeOBS layout changed");

namespace Bridge
{
    // Called once per logic frame from the 0x55DDA0 (MainLoop_AfterRender) hook.
    void OnFrame();
    // Release the shared-memory mapping (optional; OS reclaims on process exit).
    void Shutdown();
}

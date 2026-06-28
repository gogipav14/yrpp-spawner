/**
*  jax-gemma bridge for yrpp-spawner  (GPLv3, same as the spawner)
*
*  Phase 1: OBSERVATION  (DLL -> Python, "Local\yr_bridge_obs")
*  Phase 2: ACTION       (Python -> DLL, "Local\yr_bridge_act") via EventClass::OutList
*           -- the SAME serialized queue the human sidebar uses. We NEVER call the
*           AI-only production path (FactoryClass::DemandProduction / HouseClass::AI_*),
*           so the shared serial production queue / economy / fog apply automatically.
*
*  Contract mirror: ../../../docs/bridge-contract.md and yr_env/contract.py
*/
#pragma once
#include <cstdint>

namespace BridgeContract
{
    constexpr uint32_t MAGIC     = 0x59524252u; // 'YRBR'
    constexpr uint32_t VERSION   = 2u;          // bumped for Phase 2 (entity+factory+ACT)
    constexpr int      N_OWN     = 256;
    constexpr int      N_ENEMY   = 256;
    constexpr int      N_FACTORY = 16;          // max own factories surfaced
    constexpr char     OBS_NAME[] = "Local\\yr_bridge_obs";
    constexpr char     ACT_NAME[] = "Local\\yr_bridge_act";
}

// Action types — MUST match yr_env/contract.py ActionType ordering.
enum class ActType : uint8_t
{
    NOOP = 0, PRODUCE = 1, PLACE = 2, SET_PRIMARY = 3, SELL = 4,
    GROUP_MOVE = 5, GROUP_ATTACK = 6, GROUP_FORM = 7, SUPERWEAPON = 8, STANCE = 9,
};

// Result codes the DLL writes back after executing an action.
enum ActResult : uint32_t
{
    ACT_OK = 0, ACT_NOOP = 1, ACT_REJECTED_CANBUILD = 2, ACT_REJECTED_NOFACTORY = 3,
    ACT_BAD_TARGET = 4, ACT_BAD_RTTI = 5, ACT_BAD_INDEX = 6, ACT_PLACE_INVALID_CELL = 7,
    ACT_NO_AGENT = 8, ACT_UNSUPPORTED = 9,
};

#pragma pack(push, 1)

struct BridgeHeader
{
    uint32_t magic;        // BridgeContract::MAGIC
    uint32_t version;      // BridgeContract::VERSION
    uint64_t frame_seq;    // Unsorted::CurrentFrame
    int32_t  house_index;  // agent HouseClass::ArrayIndex
    uint32_t status;       // bit0 game_over, bit1 win, bit2 loss, bit3 paused
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
    int32_t  unique_id;     // AbstractClass::UniqueID — stable handle Python passes back in ACT
    int32_t  type_id;       // GetTechnoType()->GetArrayIndex() (per-RTTI-category index)
    int16_t  x, y;          // GetMapCoords() cell coords
    uint8_t  category_rtti; // WhatAmI(): Unit=1, Aircraft=2, Building=6, Infantry=15
    uint8_t  hp_frac;       // GetHealthPercentage()*255
    uint8_t  state;         // coarse state (0 for now)
    uint8_t  group_id;      // group assignment (0 = none; reserved)
};

struct BridgeFactory             // one own FactoryClass (from FactoryClass::Array, Owner==agent)
{
    int32_t current_type_id;     // Object's type GetArrayIndex(), -1 if idle
    int32_t queue_head_type_id;  // first QueuedObjects type index, -1 if none
    uint8_t category_rtti;       // Object->WhatAmI() (Unit/Building/Infantry/Aircraft), 0 if idle
    uint8_t progress;            // GetProgress() 0..54
    uint8_t queue_count;         // QueuedObjects.Count
    uint8_t flags;               // bit0 OnHold, bit1 IsSuspended
};

struct BridgeOBS
{
    BridgeHeader  header;
    BridgeGlobals globals;
    uint16_t      n_own;
    uint16_t      n_enemy;
    uint16_t      n_factory;
    uint16_t      _pad;
    BridgeEntity  own[BridgeContract::N_OWN];
    BridgeEntity  enemy[BridgeContract::N_ENEMY];
    BridgeFactory factories[BridgeContract::N_FACTORY];
};

struct BridgeAction
{
    uint8_t type;          // ActType
    uint8_t category_rtti; // PRODUCE/PLACE: the *Type* RTTI (UnitType=40,BuildingType=7,InfantryType=16,AircraftType=3)
    uint8_t is_naval;      // PRODUCE/PLACE naval flag
    uint8_t stance;        // STANCE: 0 guard, 1 aggressive, 2 hold (reserved)
    int32_t type_id;       // PRODUCE/PLACE/SELL per-category index; SUPERWEAPON = Supers index
    int32_t cell_x, cell_y;// PLACE/GROUP_MOVE/GROUP_ATTACK/SUPERWEAPON
    int32_t target_unique; // GROUP_MOVE/GROUP_ATTACK/SELL/SET_PRIMARY: AbstractClass::UniqueID (-1 = none)
    uint8_t group_id;      // reserved (groups deferred; v1 = single unit via target_unique)
    uint8_t _pad[3];
};

struct BridgeACT
{
    BridgeHeader header;     // magic/version/frame_seq/house_index/status
    uint64_t     action_seq; // Python increments AFTER writing a new action (publish last)
    uint64_t     ack_seq;    // DLL writes = last action_seq it executed (exactly-once)
    uint32_t     result;     // ActResult of the last executed action
    BridgeAction action;     // one action per frame (v1)
};

#pragma pack(pop)

// Layout guards — Python unpacks these exact sizes (VERSION must bump on any change).
static_assert(sizeof(BridgeHeader)  == 24, "BridgeHeader layout changed");
static_assert(sizeof(BridgeGlobals) == 36, "BridgeGlobals layout changed");
static_assert(sizeof(BridgeEntity)  == 16, "BridgeEntity layout changed");
static_assert(sizeof(BridgeFactory) == 12, "BridgeFactory layout changed");
static_assert(sizeof(BridgeOBS) == 24 + 36 + 8 + 256 * 16 + 256 * 16 + 16 * 12, "BridgeOBS layout changed");
static_assert(sizeof(BridgeAction)  == 24, "BridgeAction layout changed");
static_assert(sizeof(BridgeACT) == 24 + 8 + 8 + 4 + 24, "BridgeACT layout changed");

namespace Bridge
{
    // Called once per logic frame from the 0x55DDA0 (MainLoop_AfterRender) hook.
    // Writes OBS, then dispatches one pending ACT action (1-frame latency is fine for v1).
    void OnFrame();
    // Read the ACT mapping and inject this frame's action into EventClass::OutList.
    void DispatchACTFrame();
    // One-shot: dump the buildable type tables to type_catalog.csv (name <-> rtti/index).
    void DumpCatalogOnce();
    // Release the shared-memory mappings (optional; OS reclaims on process exit).
    void Shutdown();
}

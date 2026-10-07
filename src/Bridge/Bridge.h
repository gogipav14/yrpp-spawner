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

class HouseClass;
class CellClass;
class BuildingTypeClass;
namespace BridgeContract
{
    constexpr uint32_t MAGIC     = 0x59524252u; // 'YRBR'
    constexpr uint32_t VERSION   = 7u;          // v7: + BridgeScore (12 x u32 score-screen counters) before step_k; v6: + placement hints (type/rtti/n/cells[64]) before step_*; v5: + step_k/step_timeouts/step_decisions before pub_seq (v4: pub_seq seqlock, v3: spatial grid)
    constexpr int      N_OWN     = 256;
    constexpr int      N_ENEMY   = 256;
    constexpr int      N_FACTORY = 16;          // max own factories surfaced
    constexpr int      N_PLACE   = 64;          // legal placement cells surfaced
    constexpr int      GRID_DIM      = 64;      // downsampled spatial grid, H = W = 64
    constexpr int      GRID_CHANNELS = 7;       // passability,ore,fog,own_units,enemy_units,own_buildings,height
    constexpr char     OBS_NAME[] = "Local\\yr_bridge_obs";
    constexpr char     ACT_NAME[] = "Local\\yr_bridge_act";
    // Lockstep (opt-in, env YR_BRIDGE_STEP=K): auto-reset events, same "_<id>" suffix rule as the mappings.
    constexpr char     FRAME_EVENT_NAME[] = "Local\\yr_bridge_frame";  // DLL -> Python: decision-frame OBS published
    constexpr char     GO_EVENT_NAME[]    = "Local\\yr_bridge_go";     // Python -> DLL: done, continue
}

// Action types — MUST match yr_env/contract.py ActionType ordering.
enum class ActType : uint8_t
{
    NOOP = 0, PRODUCE = 1, PLACE = 2, SET_PRIMARY = 3, SELL = 4,
    GROUP_MOVE = 5, GROUP_ATTACK = 6, GROUP_FORM = 7, SUPERWEAPON = 8, STANCE = 9,
    DEPLOY = 10,   // deploy a deployable unit (e.g. MCV -> Construction Yard) by target_unique
    STOP = 11,     // EventType::Idle for own unit target_unique (human 'S' key)
    GUARD = 12,    // MegaMission Mission::Guard (stance 0) / Mission::Area_Guard (stance 1) for own unit (human 'G' key)
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
    uint8_t  group_id;      // OWNER_KIND (was reserved group id): 0 own, 1 enemy player, 2 neutral/civilian/non-player
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

struct BridgePlaceCell            // one legal top-left cell for the building that is ready to place
{
    int16_t x, y;                // cell coords (same convention as PLACE cell_x/cell_y)
    uint8_t dist;                // distance to nearest own ConYard (cells, clamped 255)
    uint8_t ore;                 // sum of tiberium value within 6 cells (clamped 255)
};

struct BridgeScore               // cumulative since match start; slots 2-5 are UNVERIFIED raw (see ReadScore)
{
    uint32_t kills_units_by_us;       // sum over enemy houses e of agent.KilledUnitsOfHouses[e]  (v7 fix; was killed_units_total)
    uint32_t kills_buildings_by_us;   // sum over enemy houses e of agent.KilledBuildingsOfHouses[e] (was killed_buildings_total)
    uint32_t raw_trk_infantry;        // sum KilledInfantryTypes.UnitTotals[] -- semantics unverified (kills vs losses)
    uint32_t raw_trk_vehicles;        // sum KilledUnitTypes.UnitTotals[]     -- unverified
    uint32_t raw_trk_aircraft;        // sum KilledAircraftTypes.UnitTotals[] -- unverified
    uint32_t raw_trk_buildings;       // sum KilledBuildingTypes.UnitTotals[] -- unverified
    uint32_t lost_units;              // sum over other houses h of h.KilledUnitsOfHouses[agent]
    uint32_t lost_buildings;          // sum over other houses h of h.KilledBuildingsOfHouses[agent]
    uint32_t credits_harvested_raw;   // HouseClass::HarvestedCredits (may be unused in skirmish -> 0)
    uint32_t credits_spent;           // HouseClass::CreditsSpent
    uint32_t income_derived;          // max(0, Balance_now - Balance_first_frame + CreditsSpent): harvest+sell+refund proxy
    uint32_t stolen_credits;          // HouseClass::StolenBuildingsCredits
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
    // Spatial vision grid (CHW): grid[channel][gy][gx], each a uint8. Fog-honored (no maphack).
    // Channels: 0 passability, 1 ore, 2 fog(0 shroud/1 fog/2 visible), 3 own_units,
    //           4 enemy_units(visible enemy-player units only; neutral excluded), 5 own_buildings, 6 height
    uint8_t       grid[BridgeContract::GRID_CHANNELS][BridgeContract::GRID_DIM][BridgeContract::GRID_DIM];
    uint16_t      grid_map_w;   // live MapCoordBounds width  in cells (so Python knows the mapped area)
    uint16_t      grid_map_h;   // live MapCoordBounds height in cells
    // v6 placement hints: the agent's completed building awaiting placement, and up to 64 cells where
    // BuildingTypeClass::CanPlaceHere(cell, agent) is true, sorted by dist ascending.
    int32_t       placement_type_id;  // BuildingType array index, -1 when nothing is ready
    uint8_t       placement_rtti;     // AbstractType::BuildingType when ready, else 0
    uint8_t       _pad2;
    uint16_t      n_place;            // valid entries in place_cells
    BridgePlaceCell place_cells[BridgeContract::N_PLACE];
    // v7 score-screen counters for the agent house (same data the end-game score screen shows).
    BridgeScore   score;
    // v5 lockstep info (always written; step_k==0 means free-run, the other two stay 0).
    uint32_t      step_k;          // YR_BRIDGE_STEP (1..600) if the lockstep events exist, else 0
    uint32_t      step_timeouts;   // cumulative go-wait timeouts since process start
    uint32_t      step_decisions;  // decision frames published in this match (includes this snapshot's frame)
    // Seqlock publish counter: ODD while the DLL is writing this struct, EVEN when consistent.
    // Readers: read pub_seq (retry if odd), copy, re-read; retry if it changed. frame_seq is just
    // the game frame number now. (The ACT region has its own protocol and is untouched.)
    uint64_t      pub_seq;
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
static_assert(sizeof(BridgePlaceCell) == 6, "BridgePlaceCell layout changed");
static_assert(sizeof(BridgeScore) == 48, "BridgeScore layout changed");
static_assert(sizeof(BridgeOBS) == 24 + 36 + 8 + 256 * 16 + 256 * 16 + 16 * 12 + 7 * 64 * 64 + 4 + (8 + 64 * 6) + 48 + 12 + 8, "BridgeOBS layout changed");
static_assert(sizeof(BridgeAction)  == 24, "BridgeAction layout changed");
static_assert(sizeof(BridgeACT) == 24 + 8 + 8 + 4 + 24, "BridgeACT layout changed");

namespace Bridge
{
    // Called exactly once per logic frame from the 0x55DCA3 hook (right after LogicClass::Update
    // returns inside MainLoop, before CurrentFrame++). Writes OBS, (lockstep: signals + waits on a
    // decision frame), then dispatches one pending ACT action.
    void OnFrame();
    // Scenario (re)load: drop the latched agent house + sticky outcome bits.
    void OnScenarioLoad();
    // Read the ACT mapping and inject this frame's action into EventClass::OutList.
    void DispatchACTFrame();
    // One-shot: dump the buildable type tables to type_catalog.csv (name <-> rtti/index).
    void DumpCatalogOnce();
    // One-shot per match: rules_types.json / rules_ai.json / ai_scripts.json (read-only engine dump).
    // Compute (cached, bounded) legal placement cells for the agent's ready building. Read-only.
    // Call BEFORE PubBegin, then copy into the OBS.
    void UpdatePlacement(HouseClass* pAgent, int frame, bool decision, bool lockstep,
                         int32_t& outType, uint8_t& outRtti, uint16_t& outN, BridgePlaceCell* outCells);
    void ResetPlacement();
    // Fairness-gated ore value for a cell (0..255): shrouded -> 0; visible -> live (and cached);
    // fogged -> last value seen while visible (fog on) / live (fog off). Read-only on game memory.
    int SeenOre(CellClass* c);
    void ResetSeenOre();                                  // clear ore cache + frame tracking (match/scenario reset)
    // True iff every cell of the building's foundation anchored at (x,y) is on the map and unshrouded.
    bool FoundationUnshrouded(BuildingTypeClass* bt, int x, int y);
    void ReadScore(HouseClass* pAgent, BridgeScore& s);   // v7 score-screen counters (read-only)
    void DumpRulesOnce(HouseClass* pAgent);
    void ResetRuleDump();   // re-arm on match reset
    // Release the shared-memory mappings (optional; OS reclaims on process exit).
    void Shutdown();
    // Per-instance mapping names (resolved once from env YR_BRIDGE_ID; legacy names if unset/invalid).
    const char* ObsName();
    const char* ActName();
    // YR_BRIDGE_UNCAP=1 (opt-in): the 0x55E2B4 hook skips the frame-rate limiter wait.
    bool UncapEnabled();
}

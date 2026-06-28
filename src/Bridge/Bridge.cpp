/**
*  jax-gemma bridge for yrpp-spawner  (GPLv3)
*
*  Phase 1 OBSERVATION: enumerate the agent house's fog-honored state once per
*  logic frame and write it into a named shared-memory region. No engine state is
*  mutated; reads only the agent house's own units + enemies it has discovered
*  (via the virtual ObjectClass::DiscoveredBy) so there is no maphack.
*/
#include <windows.h>

#include "Bridge.h"

#include <HouseClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <ObjectClass.h>
#include <Fundamentals.h>          // Unsorted::CurrentFrame
#include <Utilities/Debug.h>

namespace
{
    HANDLE     g_hMap = nullptr;
    BridgeOBS* g_obs  = nullptr;

    // Lazily create the shared-memory mapping on first frame (engine memory is
    // live by then; doing this in DllMain would be too early).
    bool EnsureMapping()
    {
        if (g_obs)
            return true;

        g_hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(BridgeOBS), BridgeContract::OBS_NAME);
        if (!g_hMap)
            return false;

        g_obs = static_cast<BridgeOBS*>(
            MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BridgeOBS)));
        if (!g_obs)
        {
            CloseHandle(g_hMap);
            g_hMap = nullptr;
            return false;
        }
        return true;
    }

    void FillEntity(BridgeEntity& e, TechnoClass* pT)
    {
        TechnoTypeClass* pType = pT->GetTechnoType();
        // ArrayIndex lives on the concrete *TypeClass; GetArrayIndex() is the
        // generic virtual that dispatches to it. NOTE: index is per-type-array,
        // so type_id is unique only within an RTTI category (Phase 1b adds category).
        e.type_id = pType ? pType->GetArrayIndex() : -1;

        CellStruct cell = pT->GetMapCoords();
        e.x = cell.X;
        e.y = cell.Y;

        double hp = pT->GetHealthPercentage();
        if (hp < 0.0) hp = 0.0;
        if (hp > 1.0) hp = 1.0;
        e.hp_frac = static_cast<uint8_t>(hp * 255.0);

        e.state    = 0;
        e.group_id = 0;
        e.cooldown = 0;
    }
}

void Bridge::OnFrame()
{
    HouseClass* pAgent = HouseClass::CurrentPlayer;   // local player's house
    if (!pAgent)
        return;                                       // not in a game yet / observer
    if (!EnsureMapping())
        return;

    BridgeOBS* o = g_obs;
    o->header.magic       = BridgeContract::MAGIC;
    o->header.version     = BridgeContract::VERSION;
    o->header.frame_seq   = static_cast<uint64_t>(Unsorted::CurrentFrame);
    o->header.house_index = pAgent->ArrayIndex;
    o->header.status      = 0;                         // win/loss reward bits: Phase 3

    o->globals.credits         = pAgent->Balance;
    o->globals.power_output    = pAgent->PowerOutput;
    o->globals.power_drain     = pAgent->PowerDrain;
    o->globals.side_index      = pAgent->SideIndex;
    o->globals.owned_units     = pAgent->OwnedUnits;
    o->globals.owned_buildings = pAgent->OwnedBuildings;
    o->globals.owned_infantry  = pAgent->OwnedInfantry;
    o->globals.owned_aircraft  = pAgent->OwnedAircraft;

    uint16_t nOwn = 0, nEnemy = 0;
    int navy = 0;

    DynamicVectorClass<TechnoClass*>& arr = TechnoClass::Array;
    for (int i = 0; i < arr.Count; ++i)
    {
        TechnoClass* pT = arr.Items[i];
        if (!pT || pT->InLimbo)
            continue;

        HouseClass* owner = pT->Owner;
        if (owner == pAgent)
        {
            if (nOwn < BridgeContract::N_OWN)
                FillEntity(o->own[nOwn++], pT);

            TechnoTypeClass* pType = pT->GetTechnoType();
            if (pType && pType->Naval)
                ++navy;
        }
        else if (owner && !pAgent->IsAlliedWith(owner))
        {
            // Fog gate: only enemies the agent house has actually discovered.
            if (pT->DiscoveredBy(pAgent) && nEnemy < BridgeContract::N_ENEMY)
                FillEntity(o->enemy[nEnemy++], pT);
        }
    }

    o->n_own            = nOwn;
    o->n_enemy          = nEnemy;
    o->globals.owned_navy = navy;

    // Throttled validation dump (requires -LOG). Compare vs the on-screen game.
    if ((Unsorted::CurrentFrame % 60) == 0)
    {
        Debug::Log("[BRIDGE] f=%d house=%d credits=%d powerOut=%d powerDrain=%d "
                   "ownU=%d ownB=%d ownI=%d ownA=%d navy=%d nOwn=%d nEnemyVis=%d\n",
                   Unsorted::CurrentFrame, pAgent->ArrayIndex, pAgent->Balance,
                   pAgent->PowerOutput, pAgent->PowerDrain,
                   pAgent->OwnedUnits, pAgent->OwnedBuildings,
                   pAgent->OwnedInfantry, pAgent->OwnedAircraft,
                   navy, static_cast<int>(nOwn), static_cast<int>(nEnemy));
    }
}

void Bridge::Shutdown()
{
    if (g_obs)  { UnmapViewOfFile(g_obs); g_obs = nullptr; }
    if (g_hMap) { CloseHandle(g_hMap);    g_hMap = nullptr; }
}

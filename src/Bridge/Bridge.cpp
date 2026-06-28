/**
*  jax-gemma bridge for yrpp-spawner  (GPLv3)
*
*  Phase 1 OBSERVATION: enumerate the agent house's fog-honored state once per
*  logic frame and write it into a named shared-memory region. No engine state is
*  mutated.
*
*  Non-cheat / no-maphack: enemies are emitted ONLY if their cell is currently
*  revealed to the agent (not shrouded, not fogged) — NOT merely "ever discovered"
*  (DiscoveredBy is sticky and would leak live positions through fog).
*  NOTE: Phase 1 assumes the agent house == HouseClass::CurrentPlayer, so the
*  global cell shroud/fog reflects the agent's view. Per-house visibility for an
*  arbitrary agent house is a Phase 3 concern.
*/
#include <windows.h>

#include "Bridge.h"

#include <ScenarioClass.h>         // ScenarioClass::Instance (in-match guard)
#include <HouseClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <ObjectClass.h>
#include <CellClass.h>             // IsShrouded / IsFogged
#include <FactoryClass.h>          // per-factory production state (serial-proof)
#include <Fundamentals.h>          // Unsorted::CurrentFrame
#include <Utilities/Debug.h>

namespace
{
    HANDLE     g_hMap = nullptr;
    BridgeOBS* g_obs  = nullptr;

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

    // pType is guaranteed non-null by the caller (technos with no type are skipped),
    // so GetHealthPercentage()'s internal GetType() cannot fault here.
    void FillEntity(BridgeEntity& e, TechnoClass* pT, TechnoTypeClass* pType)
    {
        e.unique_id     = static_cast<int32_t>(pT->UniqueID);   // stable handle for ACT targeting
        e.type_id       = pType->GetArrayIndex();               // per-RTTI-category index
        e.category_rtti = static_cast<uint8_t>(pT->WhatAmI());  // Unit/Building/Infantry/Aircraft

        CellStruct cell = pT->GetMapCoords();
        e.x = cell.X;
        e.y = cell.Y;

        double hp = pT->GetHealthPercentage();
        if (hp < 0.0) hp = 0.0;
        if (hp > 1.0) hp = 1.0;
        e.hp_frac = static_cast<uint8_t>(hp * 255.0);

        e.state    = 0;
        e.group_id = 0;
    }
}

void Bridge::OnFrame()
{
    // CRITICAL guard: 0x55DDA0 also fires on the main menu / score screen, where
    // CurrentPlayer is stale and the object arrays are torn down. Only run inside
    // an active scenario.
    if (!ScenarioClass::Instance)
        return;

    HouseClass* pAgent = HouseClass::CurrentPlayer;
    if (!pAgent)
        return;
    if (!EnsureMapping())
        return;

    BridgeOBS* o = g_obs;

    // --- body first; frame_seq is published LAST (see barrier below) ---
    o->header.magic       = BridgeContract::MAGIC;
    o->header.version     = BridgeContract::VERSION;
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

        TechnoTypeClass* pType = pT->GetTechnoType();
        if (!pType)                                   // transient/no-type object: skip
            continue;

        HouseClass* owner = pT->Owner;
        if (owner == pAgent)
        {
            if (nOwn < BridgeContract::N_OWN)
                FillEntity(o->own[nOwn++], pT, pType);
            if (pType->Naval)
                ++navy;
        }
        else if (owner && !pAgent->IsAlliedWith(owner))
        {
            // Current-visibility fog gate (no maphack): the enemy must stand on a
            // cell the agent can see RIGHT NOW, not one it saw at some point.
            CellClass* cell = pT->GetCell();
            if (cell && !cell->IsShrouded() && !cell->IsFogged())
            {
                if (nEnemy < BridgeContract::N_ENEMY)
                    FillEntity(o->enemy[nEnemy++], pT, pType);
            }
        }
    }

    o->n_own              = nOwn;
    o->n_enemy            = nEnemy;
    o->globals.owned_navy = navy;

    // Per-factory production state (the serial-proof read side). Enumerate the agent's
    // own FactoryClass instances; for the human/OutList path at most one per category is
    // ever in-progress at once (the AI cheat would show several simultaneously).
    uint16_t nFac = 0;
    DynamicVectorClass<FactoryClass*>& facs = FactoryClass::Array;
    for (int i = 0; i < facs.Count && nFac < BridgeContract::N_FACTORY; ++i)
    {
        FactoryClass* f = facs.Items[i];
        if (!f || f->Owner != pAgent)
            continue;

        BridgeFactory& bf = o->factories[nFac++];
        TechnoClass* obj = f->Object;
        if (obj)
        {
            TechnoTypeClass* ot = obj->GetTechnoType();
            bf.current_type_id = ot ? ot->GetArrayIndex() : -1;
            bf.category_rtti   = static_cast<uint8_t>(obj->WhatAmI());
        }
        else
        {
            bf.current_type_id = -1;
            bf.category_rtti   = 0;
        }
        bf.progress = static_cast<uint8_t>(f->GetProgress());
        bf.queue_count = static_cast<uint8_t>(f->QueuedObjects.Count);
        bf.queue_head_type_id = (f->QueuedObjects.Count > 0 && f->QueuedObjects.Items[0])
            ? f->QueuedObjects.Items[0]->GetArrayIndex() : -1;
        bf.flags = static_cast<uint8_t>((f->OnHold ? 1 : 0) | (f->IsSuspended ? 2 : 0));
    }
    o->n_factory = nFac;
    o->_pad = 0;

    // Publish: make all body writes visible before bumping the frame counter, so a
    // Python reader keying on frame_seq never sees a new seq over a half-written body.
    MemoryBarrier();
    o->header.frame_seq = static_cast<uint64_t>(Unsorted::CurrentFrame);

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

    // Phase 2: read the ACT mapping and inject this frame's action into OutList
    // (driven from the post-render hook -> ~1-frame latency, acceptable for v1).
    Bridge::DispatchACTFrame();
}

void Bridge::Shutdown()
{
    if (g_obs)  { UnmapViewOfFile(g_obs); g_obs = nullptr; }
    if (g_hMap) { CloseHandle(g_hMap);    g_hMap = nullptr; }
}

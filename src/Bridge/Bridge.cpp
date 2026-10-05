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

#include <ScenarioClass.h>
#include <SessionClass.h>          // IsCampaign (win fallback)         // ScenarioClass::Instance (in-match guard)
#include <HouseClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <ObjectClass.h>
#include <CellClass.h>             // IsShrouded / IsFogged
#include <MapClass.h>              // the cell grid — the agent's spatial vision
#include <BuildingClass.h>         // cell occupiers (own vs enemy)
#include <UnitClass.h>
#include <InfantryClass.h>
#include <FactoryClass.h>          // per-factory production state (serial-proof)
#include <Fundamentals.h>          // Unsorted::CurrentFrame
#include <Utilities/Debug.h>

namespace
{
    HANDLE     g_hMap = nullptr;
    BridgeOBS* g_obs  = nullptr;

    char g_obsName[96] = {};
    char g_actName[96] = {};
    bool g_namesResolved = false;

    // YR_BRIDGE_ID: [A-Za-z0-9_-], 1..32 chars, else ignored (legacy names).
    void ResolveNames()
    {
        if (g_namesResolved)
            return;
        g_namesResolved = true;
        char id[64] = {};
        DWORD n = GetEnvironmentVariableA("YR_BRIDGE_ID", id, sizeof(id));
        bool ok = (n > 0 && n <= 32);
        for (DWORD i = 0; ok && i < n; ++i)
        {
            char ch = id[i];
            ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
                || ch == '_' || ch == '-';
        }
        if (ok)
        {
            wsprintfA(g_obsName, "%s_%s", BridgeContract::OBS_NAME, id);
            wsprintfA(g_actName, "%s_%s", BridgeContract::ACT_NAME, id);
        }
        else
        {
            lstrcpyA(g_obsName, BridgeContract::OBS_NAME);
            lstrcpyA(g_actName, BridgeContract::ACT_NAME);
        }
        Debug::Log("[BRIDGE] mapping names: obs=%s act=%s (YR_BRIDGE_ID %s)\n",
                   g_obsName, g_actName, ok ? "set" : (n ? "invalid, ignored" : "unset"));
    }

    // Latched agent house + sticky terminal status for the current match.
    int      g_agentIdx   = -1;
    uint32_t g_termBits   = 0;     // sticky game_over/win/loss once observed
    int      g_lastFrame  = -1;
    HouseClass*    g_agentPtr   = nullptr;   // latched house object (identity check)
    const void*    g_scenario   = nullptr;   // ScenarioClass::Instance seen when latched
    bool     g_wasInMatch = false;

    // Seqlock writer (see BridgeOBS::pub_seq): odd = writing.
    void PubBegin()
    {
        volatile uint64_t* p = &g_obs->pub_seq;
        *p = *p + 1;                 // -> odd
        MemoryBarrier();
    }
    void PubEnd()
    {
        MemoryBarrier();
        volatile uint64_t* p = &g_obs->pub_seq;
        *p = *p + 1;                 // -> even
    }

    // Drop all per-match state; also clear the published outcome so a stale win/loss from the
    // previous match can never be read after a restart/load.
    void ResetMatch()
    {
        g_agentIdx = -1;
        g_agentPtr = nullptr;
        g_termBits = 0;
        g_lastFrame = -1;
        g_scenario = nullptr;
        if (g_obs)
        {
            PubBegin();
            g_obs->header.status = 0;
            PubEnd();
        }
    }

    // Status bits (bit0 game_over, bit1 win, bit2 loss, bit3 paused). Only state the player
    // legitimately knows: own defeat/victory, match ended, pause.
    uint32_t ComputeStatus(HouseClass* pAgent)
    {
        uint32_t st = 0;
        // HouseClass.h:856-859 Defeated / IsGameOver / IsWinner / IsLoser.
        // MPlayerDefeated sets Defeated; with DefeatedBecomesObserver the house stays
        // CurrentPlayer and becomes HouseClass::Observer (HouseClass.h:218-225), so these
        // flags remain readable on the latched house.
        bool loss = pAgent->Defeated || pAgent->IsLoser;
        bool win  = pAgent->IsWinner && !loss;
        if (!loss && !win)
        {
            // Fallback only (IsWinner is authoritative). Mirrors the spawner's match-end rule in
            // Spawner.Hook.cpp:138-160 (MPlayerDefeated_NoEnemies): houses that are Defeated,
            // MultiplayPassive (neutral) or the defeated house itself are skipped, and a house
            // counts as "still on our side" only if IsMutualAlly (HouseClass.h:256-260, both
            // directions) -- one-way alliances do NOT make an enemy count as an ally.
            // Initial observers (HouseClass.h:760) are excluded; campaign is excluded
            // (the spawner hook is skipped for SessionClass::IsCampaign, Spawner.Hook.cpp:117).
            int enemies = 0, alive = 0;
            if (!SessionClass::IsCampaign())
            {
                DynamicVectorClass<HouseClass*>& hs = HouseClass::Array;
                for (int i = 0; i < hs.Count; ++i)
                {
                    HouseClass* h = hs.Items[i];
                    if (!h || h == pAgent || !h->Type || h->IsNeutral() || h->IsInitiallyObserver()
                        || pAgent->IsMutualAlly(h))
                        continue;
                    ++enemies;
                    if (!h->Defeated)
                        ++alive;
                }
            }
            if (enemies > 0 && alive == 0)
                win = true;
        }
        bool over = win || loss || pAgent->IsGameOver;
        if (over)  st |= 1u;
        if (win)   st |= 2u;
        if (loss)  st |= 4u;
        // ScenarioClass.h:143 IsGamePaused
        if (ScenarioClass::Instance && ScenarioClass::Instance->IsGamePaused)
            st |= 8u;
        return st;
    }

    bool EnsureMapping()
    {
        if (g_obs)
            return true;

        g_hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(BridgeOBS), Bridge::ObsName());
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
    const bool inMatch = ScenarioClass::Instance && HouseClass::Array.Count > 0
        && HouseClass::CurrentPlayer;
    if (!inMatch)
    {
        // Teardown / menu / score screen: forget everything so the next match starts clean.
        if (g_wasInMatch)
            ResetMatch();
        g_wasInMatch = false;
        return;
    }
    if (!g_wasInMatch)
    {
        ResetMatch();                 // not-in-match -> in-match transition = new scenario
        g_wasInMatch = true;
    }
    // Other new-match signals independent of frame progression: a different scenario object, or
    // the frame counter going backwards.
    if (g_scenario && g_scenario != ScenarioClass::Instance)
        ResetMatch();
    if (Unsorted::CurrentFrame < g_lastFrame)
        ResetMatch();
    g_scenario  = ScenarioClass::Instance;
    g_lastFrame = Unsorted::CurrentFrame;

    // Latch the agent house on first sight: CurrentPlayer (HouseClass.h:171) is the local
    // player's house, and stays that house after defeat with DefeatedBecomesObserver
    // (MakeObserver, HouseClass.h:218, only sets HouseClass::Observer). If CurrentPlayer is ever
    // a different house than the latched one (load of another game, player switch), re-latch:
    // the latch must never point at another player's house.
    if (g_agentPtr && g_agentPtr != HouseClass::CurrentPlayer)
    {
        ResetMatch();
        g_scenario  = ScenarioClass::Instance;
        g_lastFrame = Unsorted::CurrentFrame;
    }
    if (!g_agentPtr)
    {
        HouseClass* cp = HouseClass::CurrentPlayer;
        int idx = cp->ArrayIndex;
        if (idx < 0 || idx >= HouseClass::Array.Count || HouseClass::Array.Items[idx] != cp)
            return;                   // not a registered house yet
        g_agentPtr = cp;
        g_agentIdx = idx;
    }
    // The array slot must still hold the latched object.
    if (g_agentIdx >= HouseClass::Array.Count || HouseClass::Array.Items[g_agentIdx] != g_agentPtr)
    {
        ResetMatch();
        return;
    }
    HouseClass* pAgent = g_agentPtr;
    if (!EnsureMapping())
        return;

    Bridge::DumpCatalogOnce();   // one-shot type-table dump (no-op after first call)

    BridgeOBS* o = g_obs;

    // --- seqlock: pub_seq odd while we write, even when the whole struct is consistent ---
    PubBegin();
    o->header.magic       = BridgeContract::MAGIC;
    o->header.version     = BridgeContract::VERSION;
    o->header.house_index = pAgent->ArrayIndex;
    const uint32_t st     = ComputeStatus(pAgent);
    g_termBits |= (st & 7u);                           // game_over/win/loss are sticky per match
    o->header.status      = g_termBits | (st & 8u);

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

    // --- Spatial vision grid (CHW, fog-honored: the agent's eyes; no maphack) ---
    // Downsample the live map into a fixed GRID_DIM x GRID_DIM tensor. Enemy occupants
    // are written ONLY where the cell is visible right now (same gate as the entity loop).
    {
        memset(o->grid, 0, sizeof(o->grid));
        const LTRBStruct& mb = MapClass::Instance.MapCoordBounds;
        const int minX = mb.Left, minY = mb.Top;
        int mapW = (mb.Right - mb.Left) + 1;
        int mapH = (mb.Bottom - mb.Top) + 1;
        if (mapW < 1) mapW = 1;
        if (mapH < 1) mapH = 1;
        o->grid_map_w = static_cast<uint16_t>(mapW);
        o->grid_map_h = static_cast<uint16_t>(mapH);
        const int G = BridgeContract::GRID_DIM;

        for (int cy = 0; cy < mapH; ++cy)
        {
            int gy = (cy * G) / mapH;
            if (gy >= G) gy = G - 1;
            for (int cx = 0; cx < mapW; ++cx)
            {
                CellStruct cs;
                cs.X = static_cast<short>(minX + cx);
                cs.Y = static_cast<short>(minY + cy);
                CellClass* c = MapClass::Instance.TryGetCellAt(cs);
                if (!c)
                    continue;
                int gx = (cx * G) / mapW;
                if (gx >= G) gx = G - 1;

                o->grid[0][gy][gx] = static_cast<uint8_t>((static_cast<int>(c->Passability) * 255) / 7);
                int ore = c->GetContainedTiberiumValue();
                if (ore > 255) ore = 255;
                if (ore > 0 && static_cast<uint8_t>(ore) > o->grid[1][gy][gx]) o->grid[1][gy][gx] = static_cast<uint8_t>(ore);
                uint8_t fog = c->IsShrouded() ? 0 : (c->IsFogged() ? 1 : 2);
                if (fog > o->grid[2][gy][gx]) o->grid[2][gy][gx] = fog;
                int lvl = c->GetLevel();
                if (lvl < 0) lvl = 0;
                if (lvl > 255) lvl = 255;
                o->grid[6][gy][gx] = static_cast<uint8_t>(lvl);

                const bool visibleNow = (fog == 2);
                if (BuildingClass* bld = c->GetBuilding())
                    if (bld->Owner == pAgent) o->grid[5][gy][gx] = 255;        // own buildings
                if (UnitClass* u = c->GetUnit(false))
                {
                    if (u->Owner == pAgent) o->grid[3][gy][gx] = 255;          // own units
                    else if (visibleNow && u->Owner && !pAgent->IsAlliedWith(u->Owner)) o->grid[4][gy][gx] = 255;
                }
                if (InfantryClass* inf = c->GetInfantry(false))
                {
                    if (inf->Owner == pAgent) o->grid[3][gy][gx] = 255;
                    else if (visibleNow && inf->Owner && !pAgent->IsAlliedWith(inf->Owner)) o->grid[4][gy][gx] = 255;
                }
            }
        }
    }

    o->header.frame_seq = static_cast<uint64_t>(Unsorted::CurrentFrame);
    PubEnd();                    // pub_seq -> even: readers may now accept the snapshot

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

const char* Bridge::ObsName() { ResolveNames(); return g_obsName; }
const char* Bridge::ActName() { ResolveNames(); return g_actName; }

void Bridge::OnScenarioLoad()
{
    ResetMatch();
    g_wasInMatch = false;
}

void Bridge::Shutdown()
{
    if (g_obs)  { UnmapViewOfFile(g_obs); g_obs = nullptr; }
    if (g_hMap) { CloseHandle(g_hMap);    g_hMap = nullptr; }
}

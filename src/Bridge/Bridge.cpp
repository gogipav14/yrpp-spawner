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
#include <cstdlib>

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
    char g_frameName[96] = {};
    char g_goName[96] = {};
    bool g_namesResolved = false;

    // Lockstep config (env, read once with YR_BRIDGE_ID) and runtime state.
    int      g_stepKCfg      = 0;        // YR_BRIDGE_STEP 1..600 as configured; 0 = free-run
    int      g_stepK         = 0;        // ACTIVE K for this match (0 after a wait failure until the next reset)
    DWORD    g_stepTimeoutMs = 10000;    // YR_BRIDGE_STEP_TIMEOUT_MS
    bool     g_uncap         = false;    // YR_BRIDGE_UNCAP=1
    HANDLE   g_hFrameEv      = nullptr;  // DLL -> Python
    HANDLE   g_hGoEv         = nullptr;  // Python -> DLL
    uint32_t g_stepTimeouts  = 0;        // cumulative
    uint32_t g_stepDecisions = 0;        // per match
    int      g_lastDecisionFrame = -1;   // dedupe if the frame counter did not advance between passes
    bool     g_overHandled   = false;    // game_over decision frame already served this match

    int ReadEnvInt(const char* name, int def, int lo, int hi, bool* present = nullptr)
    {
        char buf[32] = {};
        DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
        if (present) *present = (n > 0);
        if (n == 0 || n >= sizeof(buf))
            return def;
        char* end = nullptr;
        long v = strtol(buf, &end, 10);
        if (end == buf || *end != '\0' || v < lo || v > hi)
            return def;
        return static_cast<int>(v);
    }

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
            wsprintfA(g_frameName, "%s_%s", BridgeContract::FRAME_EVENT_NAME, id);
            wsprintfA(g_goName, "%s_%s", BridgeContract::GO_EVENT_NAME, id);
        }
        else
        {
            lstrcpyA(g_obsName, BridgeContract::OBS_NAME);
            lstrcpyA(g_actName, BridgeContract::ACT_NAME);
            lstrcpyA(g_frameName, BridgeContract::FRAME_EVENT_NAME);
            lstrcpyA(g_goName, BridgeContract::GO_EVENT_NAME);
        }
        Debug::Log("[BRIDGE] mapping names: obs=%s act=%s (YR_BRIDGE_ID %s)\n",
                   g_obsName, g_actName, ok ? "set" : (n ? "invalid, ignored" : "unset"));

        bool present = false;
        g_stepKCfg = ReadEnvInt("YR_BRIDGE_STEP", 0, 1, 600, &present);   // invalid/0 -> free-run
        g_stepK = g_stepKCfg;
        g_stepTimeoutMs = static_cast<DWORD>(ReadEnvInt("YR_BRIDGE_STEP_TIMEOUT_MS", 10000, 1, 3600000));
        g_uncap = ReadEnvInt("YR_BRIDGE_UNCAP", 0, 0, 1) == 1;
        Debug::Log("[BRIDGE] lockstep: K=%d (%s) timeout=%lums events=%s,%s uncap=%d\n",
                   g_stepK, g_stepK ? "on" : (present ? "invalid or 0 -> free-run" : "off"),
                   static_cast<unsigned long>(g_stepTimeoutMs), g_frameName, g_goName, g_uncap ? 1 : 0);
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
        Bridge::ResetRuleDump();
        Bridge::ResetPlacement();
        g_agentIdx = -1;
        g_agentPtr = nullptr;
        g_termBits = 0;
        g_lastFrame = -1;
        g_scenario = nullptr;
        g_stepDecisions = 0;
        g_lastDecisionFrame = -1;
        g_overHandled = false;
        // Drop any handshake state left by the previous match BEFORE clearing the published
        // metadata: an unconsumed frame signal must not wake a client on old data, and a late go
        // must not release the new match's first decision. Re-arm lockstep if it is configured.
        if (g_hFrameEv) ResetEvent(g_hFrameEv);
        if (g_hGoEv)    WaitForSingleObject(g_hGoEv, 0);
        g_stepK = g_stepKCfg;
        if (g_obs)
        {
            PubBegin();
            g_obs->header.status = 0;
            g_obs->step_decisions = 0;
            g_obs->step_k = static_cast<uint32_t>(g_stepK);
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
        ResolveNames();   // MUST precede any use of g_stepK / g_frameName / g_goName
        if (g_obs)
            return true;

        // Lockstep events are created BEFORE the OBS mapping so that a client that sees the
        // mapping can already OpenEvent them. Failure -> fall back to free-run (step_k = 0).
        if (g_stepK > 0 && !g_hFrameEv)
        {
            g_hFrameEv = CreateEventA(nullptr, FALSE, FALSE, g_frameName);   // auto-reset, non-signaled
            g_hGoEv    = CreateEventA(nullptr, FALSE, FALSE, g_goName);
            if (!g_hFrameEv || !g_hGoEv)
            {
                Debug::Log("[BRIDGE] lockstep event creation failed (err %lu): running free\n",
                           static_cast<unsigned long>(GetLastError()));
                if (g_hFrameEv) CloseHandle(g_hFrameEv);
                if (g_hGoEv)    CloseHandle(g_hGoEv);
                g_hFrameEv = g_hGoEv = nullptr;
                g_stepK = g_stepKCfg = 0;      // permanent: no events, no lockstep
            }
        }

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
    // Owner classification (BridgeEntity.group_id = owner_kind), same notion of "opposing house" as the
    // M1 win fallback in ComputeStatus: HouseClass::IsNeutral() (HouseClass.h:746, Type->MultiplayPassive),
    // IsInitiallyObserver() (HouseClass.h:760), and a null Type (not a registered player house).
    // 0 own, 1 enemy player (non-allied real player, human or AI), 2 neutral/non-player.
    uint8_t OwnerKind(HouseClass* pAgent, HouseClass* owner)
    {
        if (owner == pAgent)
            return 0;
        if (!owner->Type || owner->IsNeutral() || owner->IsInitiallyObserver())
            return 2;
        return 1;
    }

    // Playable-area test: same MapCoordBounds the spatial grid uses (a cell outside it is not part of
    // the visible map, e.g. GASPYSAT parked at cell (5000,31)).
    bool CellOnMap(const CellStruct& c)
    {
        const LTRBStruct& mb = MapClass::Instance.MapCoordBounds;
        return c.X >= mb.Left && c.X <= mb.Right && c.Y >= mb.Top && c.Y <= mb.Bottom;
    }

    void FillEntity(BridgeEntity& e, TechnoClass* pT, TechnoTypeClass* pType, uint8_t ownerKind)
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
        e.group_id = ownerKind;                                 // owner_kind (see OwnerKind)
    }
}

void Bridge::OnFrame()
{
    ResolveNames();   // env config (K, timeout, uncap, names) must be loaded before anything reads it
    // CRITICAL guard: the hook site is only reached from MainLoop, but menus/score screen/teardown
    // can still leave CurrentPlayer stale and the object arrays torn down. Only run inside
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
    Bridge::DumpRulesOnce(pAgent);   // one-shot per match rules/AI-script JSON dump

    BridgeOBS* o = g_obs;

    // Status is computed EVERY frame (before the decision logic needs it).
    const uint32_t st = ComputeStatus(pAgent);
    g_termBits |= (st & 7u);                           // game_over/win/loss are sticky per match

    // Lockstep decision frame: CurrentFrame % K == 0, or the frame where game_over first appears
    // (so Python sees the final state). After game_over we never block again. A pass that repeats
    // a frame already served (counter did not advance) is not a new decision.
    const int curFrame = Unsorted::CurrentFrame;
    bool decision = false;
    if (g_stepK > 0 && (!g_hFrameEv || !g_hGoEv))
    {
        // Never touch null handles: fall back to free-run.
        Debug::Log("[BRIDGE] lockstep: K=%d but event handles are null; falling back to free-run\n", g_stepK);
        g_stepK = g_stepKCfg = 0;
    }
    if (g_stepK > 0)
    {
        if (g_termBits & 1u)
        {
            if (!g_overHandled)
            {
                g_overHandled = true;
                decision = true;
            }
        }
        else if ((curFrame % g_stepK) == 0 && curFrame != g_lastDecisionFrame)
            decision = true;
    }
    if (decision)
    {
        g_lastDecisionFrame = curFrame;
        ++g_stepDecisions;
        // Drop stale handshake state: a late "go" from a timed-out frame must not skip this wait,
        // and a never-consumed "frame" signal must not pre-wake Python for this frame.
        ResetEvent(g_hFrameEv);
        WaitForSingleObject(g_hGoEv, 0);
    }

    // v6 placement hints: computed BEFORE the seqlock window (read-only engine queries), copied inside it.
    int32_t plType; uint8_t plRtti; uint16_t plN;
    BridgePlaceCell plCells[BridgeContract::N_PLACE];
    Bridge::UpdatePlacement(pAgent, static_cast<int>(Unsorted::CurrentFrame), decision, g_stepK > 0,
                            plType, plRtti, plN, plCells);

    // --- seqlock: pub_seq odd while we write, even when the whole struct is consistent ---
    PubBegin();
    o->header.magic       = BridgeContract::MAGIC;
    o->header.version     = BridgeContract::VERSION;
    o->header.house_index = pAgent->ArrayIndex;
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

        // Off-map / no-cell objects (pseudo-objects like GASPYSAT) are never emitted.
        if (!CellOnMap(pT->GetMapCoords()))
            continue;

        HouseClass* owner = pT->Owner;
        if (owner == pAgent)
        {
            if (nOwn < BridgeContract::N_OWN)
                FillEntity(o->own[nOwn++], pT, pType, 0);
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
                    FillEntity(o->enemy[nEnemy++], pT, pType, OwnerKind(pAgent, owner));
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
                    else if (visibleNow && u->Owner && !pAgent->IsAlliedWith(u->Owner) && OwnerKind(pAgent, u->Owner) == 1) o->grid[4][gy][gx] = 255;
                }
                if (InfantryClass* inf = c->GetInfantry(false))
                {
                    if (inf->Owner == pAgent) o->grid[3][gy][gx] = 255;
                    else if (visibleNow && inf->Owner && !pAgent->IsAlliedWith(inf->Owner) && OwnerKind(pAgent, inf->Owner) == 1) o->grid[4][gy][gx] = 255;
                }
            }
        }
    }

    o->header.frame_seq = static_cast<uint64_t>(curFrame);
    Bridge::ReadScore(pAgent, o->score);
    o->placement_type_id = plType;
    o->placement_rtti    = plRtti;
    o->_pad2             = 0;
    o->n_place           = plN;
    memset(o->place_cells, 0, sizeof(o->place_cells));
    for (int i = 0; i < plN; ++i) o->place_cells[i] = plCells[i];
    o->step_k           = static_cast<uint32_t>(g_stepK);
    o->step_timeouts    = g_stepTimeouts;
    o->step_decisions   = g_stepDecisions;
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

    // Lockstep handshake: OBS for this decision frame is published -> wake Python, then block until
    // it says "go" (it has written its action) or the timeout elapses. The game thread simply
    // sleeps here (no message pump), so the window may show "not responding" during a long wait.
    if (decision)
    {
        SetEvent(g_hFrameEv);
        const DWORD w = WaitForSingleObject(g_hGoEv, g_stepTimeoutMs);
        if (w != WAIT_OBJECT_0)
        {
            // First wait failure in this match: leave lockstep until the next match reset. This
            // avoids a late go for decision N releasing decision N+1 with a stale action.
            ++g_stepTimeouts;                          // counts every failed wait (timeout or error)
            const DWORD err = GetLastError();
            g_stepK = 0;                               // stop signalling/waiting for this match
            if (w == WAIT_TIMEOUT)
                Debug::Log("[BRIDGE] lockstep: WAIT_TIMEOUT, no go within %lu ms at frame %d; "
                           "leaving lockstep for the rest of this match (step_k=0)\n",
                           static_cast<unsigned long>(g_stepTimeoutMs), curFrame);
            else
                Debug::Log("[BRIDGE] lockstep: WAIT ERROR (not a timeout) wait=0x%lX GetLastError=%lu at frame %d; "
                           "leaving lockstep for the rest of this match (step_k=0)\n",
                           static_cast<unsigned long>(w), static_cast<unsigned long>(err), curFrame);
            PubBegin();                                // surface step_k=0 + counter to Python
            o->step_k = 0;
            o->step_timeouts = g_stepTimeouts;
            PubEnd();
        }
    }

    // Phase 2: read the ACT mapping and inject this frame's action into OutList. In lockstep this
    // runs AFTER the go (or timeout), so the action Python wrote for this decision frame is
    // executed on this very frame; free-run executes whatever is pending, once per logic frame.
    Bridge::DispatchACTFrame();
}

const char* Bridge::ObsName() { ResolveNames(); return g_obsName; }
const char* Bridge::ActName() { ResolveNames(); return g_actName; }
bool Bridge::UncapEnabled() { ResolveNames(); return g_uncap; }

void Bridge::OnScenarioLoad()
{
    ResetMatch();
    g_wasInMatch = false;
}

void Bridge::Shutdown()
{
    if (g_obs)  { UnmapViewOfFile(g_obs); g_obs = nullptr; }
    if (g_hMap) { CloseHandle(g_hMap);    g_hMap = nullptr; }
    if (g_hFrameEv) { CloseHandle(g_hFrameEv); g_hFrameEv = nullptr; }
    if (g_hGoEv)    { CloseHandle(g_hGoEv);    g_hGoEv = nullptr; }
}

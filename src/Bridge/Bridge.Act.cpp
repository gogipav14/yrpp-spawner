/**
*  jax-gemma bridge for yrpp-spawner  (GPLv3)
*
*  Phase 2 ACTION executor. Reads the ACT shared-memory region (Python writes,
*  DLL reads) and injects the action into EventClass::OutList — the SAME serialized
*  queue a human sidebar click uses. The engine drains OutList -> DoList and applies
*  the shared serial production queue / economy / fog, so this is non-cheating.
*
*  NEVER calls FactoryClass::DemandProduction or HouseClass::AI_*ConstructionUpdate
*  (the !IsHumanPlayer-gated parallel-production cheat path; see docs/ai-audit.md).
*
*  Crash guards (from the verified design): PRODUCE rtti MUST be one of the four
*  *Type categories (UnitType=40/BuildingType=7/InfantryType=16/AircraftType=3) —
*  passing a runtime RTTI (Unit=1/...) OOB-indexes the type array and crashes.
*  Indices are resolved via GetByTypeAndIndex (null-checked). Unit targets are
*  re-resolved to a LIVE TechnoClass* each frame by UniqueID (never a stale pointer).
*/
#include <windows.h>

#include "Bridge.h"

#include <ScenarioClass.h>
#include <HouseClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <BuildingTypeClass.h>
#include <ObjectClass.h>
#include <EventClass.h>
#include <MapClass.h>
#include <FactoryClass.h>
#include <TargetClass.h>
#include <Fundamentals.h>          // Unsorted::CurrentFrame
#include <GeneralDefinitions.h>    // AbstractType / EventType / Mission / CanBuildResult
#include <Utilities/Debug.h>

namespace
{
    HANDLE     g_hActMap = nullptr;
    BridgeACT* g_act     = nullptr;
    uint64_t   g_lastAck = 0;

    bool EnsureActMapping()
    {
        if (g_act)
            return true;
        // Python is the writer, DLL the reader+acker; both open PAGE_READWRITE.
        g_hActMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(BridgeACT), Bridge::ActName());
        if (!g_hActMap)
            return false;
        g_act = static_cast<BridgeACT*>(
            MapViewOfFile(g_hActMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BridgeACT)));
        if (!g_act)
        {
            CloseHandle(g_hActMap);
            g_hActMap = nullptr;
            return false;
        }
        return true;
    }

    // Resolve a UniqueID to a LIVE techno (survives fog/attrition; never a stale ptr).
    TechnoClass* ResolveTechno(int unique)
    {
        if (unique < 0)
            return nullptr;
        // `unique` is an opaque OBS handle; an unissued handle resolves like an absent id (-> BAD_TARGET).
        int real = Bridge::HandleToUid(unique);
        if (real < 0)
            return nullptr;
        DWORD uid = static_cast<DWORD>(real);
        DynamicVectorClass<TechnoClass*>& arr = TechnoClass::Array;
        for (int i = 0; i < arr.Count; ++i)
        {
            TechnoClass* pT = arr.Items[i];
            if (pT && !pT->InLimbo && pT->UniqueID == uid)
                return pT;
        }
        return nullptr;
    }

    bool IsProducibleType(AbstractType abs)
    {
        return abs == AbstractType::UnitType || abs == AbstractType::BuildingType
            || abs == AbstractType::InfantryType || abs == AbstractType::AircraftType;
    }

    // Validate the ORIGINAL int32 cell coordinates against the playable map rect BEFORE narrowing to short
    // ((65536+x, y) must not wrap to (x, y)).
    bool CellInPlayableRect(int x, int y, CellStruct& out)
    {
        const LTRBStruct& mb = MapClass::Instance.MapCoordBounds;
        if (x < mb.Left || x > mb.Right || y < mb.Top || y > mb.Bottom)
            return false;
        out.X = static_cast<short>(x);
        out.Y = static_cast<short>(y);
        return true;
    }

    bool Readable(const void* p, size_t n)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
    }

    bool IsFootRtti(AbstractType t)
    {
        return t == AbstractType::Unit || t == AbstractType::Infantry || t == AbstractType::Aircraft;
    }

    // Policy: the bridge never allows MORE than a human can do; being stricter is fine, and edge-case actions the agent
    // does not need return ACT_UNSUPPORTED. Unit orders (MOVE/ATTACK/GUARD) are FOOT units only that pass
    // Bridge::UnitCommandable; an owned building target is UNSUPPORTED, anything else BAD_TARGET.
    TechnoClass* ResolveControllable(HouseClass* pAgent, int handle, ActResult& err)
    {
        TechnoClass* pU = ResolveTechno(handle);
        if (!pU || pU->InLimbo || pU->Owner != pAgent) { err = ACT_BAD_TARGET; return nullptr; }
        if (!IsFootRtti(pU->WhatAmI())) { err = ACT_UNSUPPORTED; return nullptr; }
        if (!Bridge::UnitCommandable(pU, pAgent)) { err = ACT_BAD_TARGET; return nullptr; }
        return pU;
    }

    // Phobos Ammo.DeployUnlockMinimum/MaximumAmount (TechnoType ext = TechnoTypeClass+0x18; ext+0xE4 / +0xE8, default -1;
    // offsets confirmed from the installed Phobos 0.4.0.2 INI parser). Fail closed for ammo types when the extension
    // layout cannot be trusted (signature mismatch / unreadable ext).
    bool DeployAmmoOK(TechnoClass* pT)
    {
        TechnoTypeClass* tt = pT->GetTechnoType();
        if (!tt) return false;
        if (tt->Ammo <= 0) return true;                         // no ammo: no ammo restriction possible
        const int st = Bridge::PhobosState();
        if (st == 1) return true;                               // Phobos absent
        if (st != 2) return false;                              // present but unverified: fail closed
        const char* ext = *reinterpret_cast<const char* const*>(reinterpret_cast<const char*>(tt) + 0x18);
        if (!ext || !Readable(ext + 0xE4, 8)) return false;
        const int mn = *reinterpret_cast<const int*>(ext + 0xE4), mx = *reinterpret_cast<const int*>(ext + 0xE8);
        if (mn >= 0 && pT->Ammo < mn) return false;
        if (mx >= 0 && pT->Ammo > mx) return false;
        return true;
    }

    // STOP / DEPLOY admission (native 0x730ECD / 0x730C38): (IsControllable || IsActive), selectable, not Berzerk. Foot only.
    bool StopDeployOK(TechnoClass* pT)
    {
        return (pT->IsControllable() || pT->IsActive()) && pT->CanBeSelected() && pT->CanBeSelectedNow() && !pT->Berzerk;
    }

    ActResult DoProduce(HouseClass* pAgent, const BridgeAction& a)
    {
        AbstractType abs = static_cast<AbstractType>(a.category_rtti);
        if (!IsProducibleType(abs))                              // GUARD: *Type only (else crash)
            return ACT_BAD_RTTI;
        TechnoTypeClass* pTT = TechnoTypeClass::GetByTypeAndIndex(abs, a.type_id);
        if (!pTT)                                                // GUARD: index in-range
            return ACT_BAD_INDEX;
        // The naval flag is DERIVED from the type, never trusted: a caller-chosen flag would let one land type occupy both
        // Primary_ForVehicles and Primary_ForShips (parallel production a human cannot do). Mismatch = bad request.
        const bool typeNaval = pTT->Naval;
        if ((a.is_naval != 0) != typeNaval)
            return ACT_BAD_INDEX;
        if (!pAgent->HasFactoryForObject(pTT))
            return ACT_REJECTED_NOFACTORY;
        if (pAgent->CanBuild(pTT, false, true) != CanBuildResult::Buildable)
            return ACT_REJECTED_CANBUILD;
        EventClass ev(pAgent->ArrayIndex, EventType::Produce,
            static_cast<int>(abs), a.type_id, typeNaval ? TRUE : FALSE);  // ctor 0x4C6970
        EventClass::OutList.Add(ev);                                       // 0x00A802C8 (human path)
        return ACT_OK;
    }

    ActResult DoPlace(HouseClass* pAgent, const BridgeAction& a)
    {
        if (static_cast<AbstractType>(a.category_rtti) != AbstractType::BuildingType)
            return ACT_BAD_RTTI;
        TechnoTypeClass* pTT = TechnoTypeClass::GetByTypeAndIndex(AbstractType::BuildingType, a.type_id);
        if (!pTT)
            return ACT_BAD_INDEX;
        BuildingTypeClass* pBT = static_cast<BuildingTypeClass*>(pTT);
        if ((a.is_naval != 0) != pBT->Naval)   // canonical flag only (see DoProduce)
            return ACT_BAD_INDEX;
        CellStruct cell;
        if (!CellInPlayableRect(a.cell_x, a.cell_y, cell))
            return ACT_PLACE_INVALID_CELL;
        // Human-UI preconditions that Building::Unlimbo (the PLACE event path) does NOT enforce: a matching
        // building is ready, the whole foundation is explored, and it is inside the build radius. Reject here,
        // with the same codes a human mis-click gets, BEFORE any live legality query or enqueue.
        bool ready = false;
        {
            DynamicVectorClass<FactoryClass*>& facs = FactoryClass::Array;
            for (int i = 0; i < facs.Count && !ready; ++i)
            {
                FactoryClass* f = facs.Items[i];
                if (!f || f->Owner != pAgent || !f->Object) continue;
                if (f->Object->WhatAmI() != AbstractType::Building || !f->IsDone()) continue;
                if (f->Object->GetTechnoType() == pBT) ready = true;
            }
        }
        if (!ready)
            return ACT_REJECTED_CANBUILD;
        if (!Bridge::PlaceAllowed(pBT, cell.X, cell.Y, pAgent) ||
            (pBT->PowersUpBuilding[0] == '\0' && !pBT->CanPlaceHere(&cell, pAgent)))   // upgrades: host check replaces CanPlaceHere
            return ACT_PLACE_INVALID_CELL;
        EventClass ev(pAgent->ArrayIndex, EventType::Place, AbstractType::BuildingType,
            a.type_id, pBT->Naval ? 1 : 0, cell);                // ctor 0x4C6AE0
        EventClass::OutList.Add(ev);
        return ACT_OK;
    }

    // v1: single-unit move / attack-move to a cell (groups deferred).
    ActResult DoGroupOrder(HouseClass* pAgent, const BridgeAction& a, bool attackMove)
    {
        ActResult rerr = ACT_BAD_TARGET;
        TechnoClass* pU = ResolveControllable(pAgent, a.target_unique, rerr);   // own, live, foot, commandable
        if (!pU)
            return rerr;
        CellStruct cell;
        if (!CellInPlayableRect(a.cell_x, a.cell_y, cell))
            return ACT_BAD_INDEX;
        TargetClass whom(static_cast<AbstractClass*>(pU));       // 0x6E6AB0
        TargetClass dest(cell);                                  // 0x6E6B20
        TargetClass tgt, follow;                                 // empty
        Mission m = attackMove ? Mission::AttackMove : Mission::Move;
        EventClass ev(pAgent->ArrayIndex, whom, m, tgt, dest, follow);     // MegaMission 0x4C6860
        EventClass::OutList.Add(ev);
        return ACT_OK;
    }

    // DEPLOY / SELL / SET_PRIMARY share a {TargetClass Whom} payload, but the Target EventClass ctor
    // (int,EventType,int,int) @0x4C65E0 is AMBIGUOUS in YRpp with an (int,EventType,int,const int&)
    // overload. Sidestep it: build the event from a known-good ctor (Produce), then overwrite Type +
    // Whom. The DEPLOY/SELL/PRIMARY union members all begin with TargetClass Whom at the same offset,
    // so one helper serves all three. The engine handler reads only Whom; leftover union bytes ignored.
    ActResult EmitWhomEvent(HouseClass* pAgent, EventType type, TechnoClass* pT)
    {
        // Per-action admission (what the human UI would allow), NOT one shared control gate:
        //  * STOP / DEPLOY: foot units only, (IsControllable||IsActive), selectable, not Berzerk (+ DEPLOY: CanDeploySlashUnload
        //    slot 197 and the Phobos ammo-unlock window).
        //  * SET_PRIMARY: live owned building with a factory role; no controllability test (an EMP-disabled factory can be
        //    set primary by a human: 0x443450 / 0x448160).
        //  * SELL: live owned building + Building::CanBeSold (vtable slot 38 = 0x4494C0, Unsellable check at 0x4494D2);
        //    no controllability test (Phobos NoManualMove must not block SELL).
        if (!pT || pT->InLimbo || pT->Owner != pAgent)
            return ACT_BAD_TARGET;
        if (type == EventType::Sell || type == EventType::Primary)
        {
            if (pT->WhatAmI() != AbstractType::Building)
                return ACT_BAD_TARGET;
            if (type == EventType::Primary)
            {
                BuildingClass* b = static_cast<BuildingClass*>(pT);
                if (!b->Type || b->Type->Factory == AbstractType::None)
                    return ACT_BAD_TARGET;
                if (b->IsPrimaryFactory)          // the human click path suppresses PRIMARY here (it would toggle OFF)
                    return ACT_NOOP;
            }
            else
            {
                typedef bool (__thiscall *CanSellFn)(void*);
                void** vt = *reinterpret_cast<void***>(pT);
                if (!reinterpret_cast<CanSellFn>(vt[38])(pT))
                    return ACT_BAD_TARGET;
            }
        }
        else
        {
            // STOP / DEPLOY: foot units only; buildings (factory STOP, occupant unloading) are UNSUPPORTED by policy.
            if (!IsFootRtti(pT->WhatAmI()))
                return ACT_UNSUPPORTED;
            if (!StopDeployOK(pT))
                return ACT_BAD_TARGET;
            if (type == EventType::Deploy && (!pT->CanDeploySlashUnload() || !DeployAmmoOK(pT)))
                return ACT_BAD_TARGET;
        }
        EventClass ev(pAgent->ArrayIndex, EventType::Produce, 0, 0, FALSE);  // known-good base (sets Frame)
        ev.Type        = type;
        ev.IsExecuted  = false;
        ev.HouseIndex  = static_cast<char>(pAgent->ArrayIndex);
        ev.Deploy.Whom = TargetClass(static_cast<AbstractClass*>(pT));       // aliases Sell/Primary Whom
        EventClass::OutList.Add(ev);
        return ACT_OK;
    }

    // GUARD: same MegaMission construction as DoGroupOrder (ctor 0x4C6860) with Mission::Guard (stance 0)
    // or Mission::Area_Guard (stance 1); empty target/dest/follow. Own LIVE unit only. OutList only.
    ActResult DoGuard(HouseClass* pAgent, const BridgeAction& a)
    {
        ActResult rerr = ACT_BAD_TARGET;
        TechnoClass* pU = ResolveControllable(pAgent, a.target_unique, rerr);
        if (!pU)
            return rerr;
        TargetClass whom(static_cast<AbstractClass*>(pU));
        TargetClass none1, none2, none3;
        Mission m = (a.stance == 1) ? Mission::Area_Guard : Mission::Guard;
        EventClass ev(pAgent->ArrayIndex, whom, m, none1, none2, none3);
        EventClass::OutList.Add(ev);
        return ACT_OK;
    }

    // SUPERWEAPON is DISABLED: SpecialPlace reaches ClickFire/Launch without the cursor-path admission checks (Ares
    // 0x100348D0: shroud, CanFireAt, range, designators, inhibitors), and no verified read-only adapter exists. A
    // human cannot fire outside those restrictions, so the action is unsupported (no Python caller uses it).
    ActResult DoSuperWeapon(HouseClass*, const BridgeAction&)
    {
        return ACT_UNSUPPORTED;
    }

    ActResult ExecuteAction(HouseClass* pAgent, const BridgeAction& a)
    {
        switch (static_cast<ActType>(a.type))
        {
        case ActType::NOOP:         return ACT_NOOP;
        case ActType::PRODUCE:      return DoProduce(pAgent, a);
        case ActType::PLACE:        return DoPlace(pAgent, a);
        case ActType::DEPLOY:       return EmitWhomEvent(pAgent, EventType::Deploy,  ResolveTechno(a.target_unique));
        case ActType::SET_PRIMARY:  return EmitWhomEvent(pAgent, EventType::Primary, ResolveTechno(a.target_unique));
        case ActType::SELL:         return EmitWhomEvent(pAgent, EventType::Sell,    ResolveTechno(a.target_unique));
        case ActType::STOP:         return EmitWhomEvent(pAgent, EventType::Idle,    ResolveTechno(a.target_unique));
        case ActType::GUARD:        return DoGuard(pAgent, a);
        case ActType::GROUP_MOVE:   return DoGroupOrder(pAgent, a, false);
        case ActType::GROUP_ATTACK: return DoGroupOrder(pAgent, a, true);
        case ActType::SUPERWEAPON:  return DoSuperWeapon(pAgent, a);
        case ActType::GROUP_FORM:                                 // groups deferred
        case ActType::STANCE:       return ACT_UNSUPPORTED;
        default:                    return ACT_UNSUPPORTED;
        }
    }
}

void Bridge::DispatchACTFrame()
{
    if (!ScenarioClass::Instance)
        return;
    HouseClass* pAgent = HouseClass::CurrentPlayer;
    if (!pAgent)
        return;
    if (!EnsureActMapping())
        return;

    BridgeACT* act = g_act;
    // keep the header fresh so Python can validate the mapping
    act->header.magic       = BridgeContract::MAGIC;
    act->header.version     = BridgeContract::VERSION;
    act->header.house_index = pAgent->ArrayIndex;
    act->header.frame_seq   = static_cast<uint64_t>(Unsorted::CurrentFrame);

    // LOAD-BEARING: the ACT body is read without a seqlock retry. This is safe ONLY because
    // the Python writer (write_act.py) BLOCKS on ack_seq before writing the next action, so the
    // body for `seq` is never overwritten mid-read. A non-blocking writer would need a seqlock here.
    uint64_t seq = act->action_seq;
    MemoryBarrier();                       // pair with Python's publish-action_seq-last
    if (seq == g_lastAck)
        return;                            // no new action this frame (do NOT re-execute)

    ActResult r = ExecuteAction(pAgent, act->action);

    act->result = static_cast<uint32_t>(r);
    MemoryBarrier();
    act->ack_seq = seq;                    // tell Python this seq was consumed
    g_lastAck = seq;

    if (r != ACT_OK && r != ACT_NOOP)
        Debug::Log("[BRIDGE-ACT] seq=%llu type=%d result=%d\n",
                   static_cast<unsigned long long>(seq), static_cast<int>(act->action.type),
                   static_cast<int>(r));
}

// Unit-command admission (MOVE/ATTACK/GUARD/STOP/DEPLOY and the OBS "not commandable" bit): owner, live and not limbo,
// virtual IsControllable (YRpp +0xA0; buildings override it at 0x44F5C0 so the virtual is the right one), and the
// human selection path CanBeSelected + CanBeSelectedNow (0x4AC2CB / 0x4AC2D9).
bool Bridge::UnitCommandable(TechnoClass* pT, HouseClass* pAgent)
{
    return pT && !pT->InLimbo && pT->Owner == pAgent && !pT->Berzerk
        && pT->IsControllable() && pT->CanBeSelected() && pT->CanBeSelectedNow();
}

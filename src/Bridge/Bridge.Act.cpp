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
            0, sizeof(BridgeACT), BridgeContract::ACT_NAME);
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
        DWORD uid = static_cast<DWORD>(unique);
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

    ActResult DoProduce(HouseClass* pAgent, const BridgeAction& a)
    {
        AbstractType abs = static_cast<AbstractType>(a.category_rtti);
        if (!IsProducibleType(abs))                              // GUARD: *Type only (else crash)
            return ACT_BAD_RTTI;
        TechnoTypeClass* pTT = TechnoTypeClass::GetByTypeAndIndex(abs, a.type_id);
        if (!pTT)                                                // GUARD: index in-range
            return ACT_BAD_INDEX;
        if (!pAgent->HasFactoryForObject(pTT))
            return ACT_REJECTED_NOFACTORY;
        if (pAgent->CanBuild(pTT, false, true) != CanBuildResult::Buildable)
            return ACT_REJECTED_CANBUILD;
        EventClass ev(pAgent->ArrayIndex, EventType::Produce,
            static_cast<int>(abs), a.type_id, a.is_naval ? TRUE : FALSE);  // ctor 0x4C6970
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
        CellStruct cell { static_cast<short>(a.cell_x), static_cast<short>(a.cell_y) };
        if (!pBT->CanPlaceHere(&cell, pAgent))                   // GUARD: legal foundation
            return ACT_PLACE_INVALID_CELL;
        EventClass ev(pAgent->ArrayIndex, EventType::Place, AbstractType::BuildingType,
            a.type_id, a.is_naval ? 1 : 0, cell);                // ctor 0x4C6AE0
        EventClass::OutList.Add(ev);
        return ACT_OK;
    }

    // v1: single-unit move / attack-move to a cell (groups deferred).
    ActResult DoGroupOrder(HouseClass* pAgent, const BridgeAction& a, bool attackMove)
    {
        TechnoClass* pU = ResolveTechno(a.target_unique);
        if (!pU || pU->Owner != pAgent)                         // only order our own LIVE unit
            return ACT_BAD_TARGET;
        TargetClass whom(static_cast<AbstractClass*>(pU));       // 0x6E6AB0
        CellStruct cell { static_cast<short>(a.cell_x), static_cast<short>(a.cell_y) };
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
        if (!pT || pT->Owner != pAgent)
            return ACT_BAD_TARGET;
        EventClass ev(pAgent->ArrayIndex, EventType::Produce, 0, 0, FALSE);  // known-good base (sets Frame)
        ev.Type        = type;
        ev.IsExecuted  = false;
        ev.HouseIndex  = static_cast<char>(pAgent->ArrayIndex);
        ev.Deploy.Whom = TargetClass(static_cast<AbstractClass*>(pT));       // aliases Sell/Primary Whom
        EventClass::OutList.Add(ev);
        return ACT_OK;
    }

    ActResult DoSuperWeapon(HouseClass* pAgent, const BridgeAction& a)
    {
        if (a.type_id < 0 || a.type_id >= pAgent->Supers.Count)  // GUARD: Supers bounds
            return ACT_BAD_INDEX;
        if (!pAgent->Supers.Items[a.type_id])
            return ACT_BAD_TARGET;
        CellStruct cell { static_cast<short>(a.cell_x), static_cast<short>(a.cell_y) };
        EventClass ev(pAgent->ArrayIndex, EventType::SpecialPlace, a.type_id, cell); // 0x4C6B60
        EventClass::OutList.Add(ev);
        return ACT_OK;
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

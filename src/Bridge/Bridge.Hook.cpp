/**
*  jax-gemma bridge for yrpp-spawner  (GPLv3)
*
*  Per-logic-frame hook. We attach our own DEFINE_HOOK at 0x55DDA0
*  (MainLoop_AfterRender) where ProtocolZero also hooks and returns 0 — so the
*  two stubs chain cleanly (both return 0, Syringe runs the original bytes once).
*  We deliberately do NOT piggyback 0x647BEB: that ProtocolZero hook returns
*  non-zero jump addresses, which makes a chained stub order-dependent/fragile.
*  Reading settled state once per frame here is ideal for OBS.
*/
#include <Helpers/Macro.h>   // pulls in Syringe.h (DEFINE_HOOK / EXPORT_FUNC)
#include "Bridge.h"

// Scenario load (same site the spawner uses to clear UIGameMode on game load,
// Spawner.Hook.cpp ScenarioClass_Load_Suffix @0x689669). Drops the latched agent/outcome.
DEFINE_HOOK(0x689669, Bridge_ScenarioLoad, 0x6)
{
    (void)R;
    Bridge::OnScenarioLoad();
    return 0;
}

DEFINE_HOOK(0x55DDA0, Bridge_AfterFrame, 0x5)
{
    (void)R;                 // we read no registers; state is re-derived from globals
    Bridge::OnFrame();
    return 0;                // fall through to the engine / chained ProtocolZero stub
}

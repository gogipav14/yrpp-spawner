/**
*  jax-gemma bridge for yrpp-spawner  (GPLv3)
*
*  Per-logic-frame hook: 0x55DCA3, size 5, inside MainLoop (0x55D360), the instruction directly
*  after `call LogicClass::Update` (0x55DC9E: mov ecx,0x87F778 / call 0x55AFB0, whose ONLY caller
*  is this site). Straight-line code, so it runs exactly once per MainLoop frame step in every
*  session type (campaign, skirmish, LAN), after the whole logic update and before the
*  `inc CurrentFrame` at 0x55DE7E. The original instruction at 0x55DCA3 is
*  `mov eax,[0xA8EC08]` (A1 08 EC A8 00, exactly 5 bytes, absolute operand, so Syringe's relocated
*  copy is safe). No other hook (spawner, Ares, Phobos) uses 0x55DCA3; we return 0 and read no
*  registers.
*
*  Why not the other candidates: 0x55B4E1 (LogicClass_Update_RepairFPState) is mid-LogicClass::Update,
*  before its second phase, so state would be half-updated. 0x647BEB (QueueAIMultiplayer) sits behind
*  early-outs and the multiplayer-only path, and ProtocolZero returns non-zero jump targets there, so
*  a chained stub would be order-dependent. 0x55DDA0 (the old site) is also once per MainLoop pass
*  but it is after the draw phase, and the old "render frame" assumption gave no clear guarantee.
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

DEFINE_HOOK(0x55DCA3, Bridge_AfterLogicUpdate, 0x5)
{
    (void)R;                 // we read no registers; state is re-derived from globals
    Bridge::OnFrame();
    return 0;                // run the original `mov eax,[0xA8EC08]`
}

// Opt-in speed-up (YR_BRIDGE_UNCAP=1). 0x55E160 is the frame limiter called at the end of every
// MainLoop pass. For SessionType 0/5 (campaign/skirmish) it ends in the wait loop starting at
// 0x55E2B4 (`mov esi,[0x887350]` = the per-frame delay, GameOptions.GameSpeed in 1/60 s units; wait
// until timeGetTime>>4 has advanced that much, Sleep()ing the remainder). Jumping to 0x55E33B skips
// the wait and lands on the FPS-counter tail with the saved registers intact.
DEFINE_HOOK(0x55E2B4, Bridge_UncapFrameLimiter, 0x6)
{
    (void)R;
    return Bridge::UncapEnabled() ? 0x55E33B : 0;
}

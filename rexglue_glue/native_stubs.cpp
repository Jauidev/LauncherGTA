// gta4port: pieces of LibertyRecomp / ReXGlue the ported hooks reference but this runtime does not
// use: their touch-control help prompts (this runtime has its own touch controls) and ReXGlue's
// kernel state (the hooks' memory lookups go through rexglue_glue's GuestMemory()).
#include <cstdint>

#include <rex/system/xmemory.h>

#include "gta4_init.h"

namespace gta4::input {
void BeginTouchHelpDraw(uint8_t*, uint32_t) noexcept {}
void ObserveTouchHelpText(uint8_t*, uint32_t) noexcept {}
void EndTouchHelpText() noexcept {}
void EndTouchHelpDraw() noexcept {}
// Their touch HUD: the weapon sprite draws as the game does. TouchRadarLocalViewport is in radar.cpp.
void DrawTouchWeaponHudSprite(PPCContext& ctx, uint8_t* base,
                              void (*original)(PPCContext&, uint8_t*)) {
  original(ctx, base);
}
class ContextTouchGameplayTransition final {
 public:
  ContextTouchGameplayTransition() noexcept;
  ~ContextTouchGameplayTransition();
};
ContextTouchGameplayTransition::ContextTouchGameplayTransition() noexcept = default;
ContextTouchGameplayTransition::~ContextTouchGameplayTransition() = default;
}  // namespace gta4::input

// Their frontend (gta4_frontend_hooks.cpp) redraws the menu sliders; here the game's own slider
// pass runs (sub_822675A0, whose hook in gta4_aspect_hooks.cpp calls this).
namespace gta4::frontend_menu {
void DrawSliders(PPCContext& ctx, uint8_t* base) { __imp__sub_822675A0(ctx, base); }
}  // namespace gta4::frontend_menu

// LibertyRecomp's font-selection trace (gta4_font_selection_trace.cpp, diagnostics only).
void GTA4_FontSelectionTraceBinding(uint32_t, uint32_t, uint32_t, uint32_t) {}

namespace rex::system {
class KernelState;
KernelState* kernel_state() { return nullptr; }
}  // namespace rex::system

namespace rex::memory {
// The heap memory.cpp's LookupHeap returns for every address: the runtime keeps its own page
// table, so the hooks' readability checks only see whole read/write memory.
PageAccess BaseHeap::QueryRangeAccess(uint32_t low_address, uint32_t high_address) {
  (void)low_address;
  (void)high_address;
  return PageAccess::kReadWrite;
}
}  // namespace rex::memory

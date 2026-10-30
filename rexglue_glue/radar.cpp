// gta4port: the minimap's map out of place under the player arrow on screens wider than 16:9.
//
// GTA IV draws the radar in its own render phase through a private copy of the HUD_RADAR
// viewport, and every layer inside it (map tiles, GPS route, blips, the arrow, the health/armor
// ring and its mask) uses coordinates local to that viewport. LibertyRecomp lays the radar out
// once, on that copy (gta4_touch_guest_hooks.cpp sub_821BC7C8), and keeps the layers local
// (gta4_touch_coordinator.cpp TU8 sub_8239C468, TouchRadarLocalViewport). Neither file is built
// here, so TouchRadarLocalViewport was stubbed to false, and gta4_aspect_hooks.cpp's DrawRadarSection
// then applied the bottom-left screen layout (x * 0.8 at 20:9) to the local map tiles and route
// only: the map and route slid away from the arrow and the blips (2026-10-27 screenshot: the
// route started 15% of the radar's width to the right of the arrow), and the radar stayed an
// ellipse 1.25x wider than tall. These are the two pieces of those files the radar needs.
//
// gta4_radar_aspect (default on) lays the radar viewport out like the rest of the HUD: round and
// anchored bottom-left. Off, the viewport keeps the game's stretched rectangle; the layers stay
// aligned either way.
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "gta4_init.h"
#include "gta4_aspect_hooks.h"
#include "input/context_touch_radar.h"

REXCVAR_DEFINE_BOOL(gta4_radar_aspect, true, "GTA IV/Display",
                    "Draw the minimap with the HUD's aspect correction (round, bottom-left) "
                    "instead of stretched to the display");

namespace gta4::input {
namespace {

// EFLC addresses, as generated in rexglue_glue/eflc/gta4_touch_guest_hooks.cpp.
constexpr uint32_t kRadarRenderPhaseVtable = 0x820131E4;
constexpr uint32_t kViewportCopyVtable = 0x82001150;
constexpr uint32_t kCurrentRenderPhase = 0x82FEFC74;
constexpr uint32_t kCurrentViewport = 0x831C2080;
// Nonzero while the pause-menu map uses the radar phase; it keeps its frontend layout.
constexpr uint32_t kRadarMapMode = 0x82C30C14;
constexpr uint32_t kPhaseViewportOffset = 176;
constexpr uint32_t kCopiedViewportOffset = 16;
constexpr uint32_t kViewportCopySize = 1040;
constexpr size_t kMaximumCopies = 32768;

bool Span(uint8_t* base, uint32_t address, size_t size, bool write = false) {
  if (!base || !address || !size || uint64_t(address) + size > uint64_t(UINT32_MAX) + 1)
    return false;
  auto* memory = ::gta4native::GuestMemory();
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  const uint32_t last = uint32_t(uint64_t(address) + size - 1);
  if (!heap || heap != memory->LookupHeap(last)) return false;
  const auto access = heap->QueryRangeAccess(address, last);
  using rex::memory::PageAccess;
  return access == PageAccess::kReadWrite || access == PageAccess::kExecuteReadWrite ||
         (!write && (access == PageAccess::kReadOnly || access == PageAccess::kExecuteReadOnly));
}
uint32_t Read(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, rex::memory::GuestPtr(base, address), sizeof(value));
  return __builtin_bswap32(value);
}
double Float(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(Read(base, address));
}
bool GameplayRadar(uint8_t* base) {
  return Span(base, kRadarMapMode, 1) && *rex::memory::GuestPtr(base, kRadarMapMode) == 0;
}

// The viewport's normalized x, y, width, height (offset 664 holds the HUD_RADAR rectangle).
gta4::aspect::Rect ViewportRect(uint8_t* base, uint32_t viewport, uint32_t offset) {
  const double x = Float(base, viewport + offset), y = Float(base, viewport + offset + 4);
  const double width = Float(base, viewport + offset + 8);
  const double height = Float(base, viewport + offset + 12);
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
      x < 0.0 || y < 0.0 || width <= 0.0 || height <= 0.0 || x + width > 1.0 ||
      y + height > 1.0)
    return {};
  return {x, y, x + width, y + height};
}
bool Valid(gta4::aspect::Rect rect) {
  return rect.right > rect.left && rect.bottom > rect.top;
}

// Gameplay radar viewport copies by command address; the token tells a reused address apart.
struct Copy {
  uint32_t token;
  uint64_t serial;
};
std::mutex copies_mutex;
std::unordered_map<uint32_t, Copy> copies;
std::deque<std::pair<uint32_t, uint64_t>> copies_order;
uint64_t copies_serial = 0;

void CopyRadarViewport(PPCContext& ctx, uint8_t* base) {
  const uint32_t command = ctx.r3.u32, source = ctx.r4.u32;
  const uint32_t phase = Span(base, kCurrentRenderPhase, 4) ? Read(base, kCurrentRenderPhase) : 0;
  const bool radar_phase = Span(base, phase, kPhaseViewportOffset) &&
                           Read(base, phase) == kRadarRenderPhaseVtable &&
                           uint64_t(phase) + kPhaseViewportOffset == source;
  {
    std::lock_guard lock(copies_mutex);
    copies.erase(command);
  }
  __imp__sub_821BC7C8(ctx, base);
  if (!radar_phase || !GameplayRadar(base) || !Span(base, command, kViewportCopySize, true) ||
      Read(base, command) != kViewportCopyVtable)
    return;
  const uint32_t viewport = command + kCopiedViewportOffset;
  const gta4::aspect::Extent target{Read(base, viewport + 688), Read(base, viewport + 692)};
  const auto authored = ViewportRect(base, viewport, 664);
  if (REXCVAR_GET(gta4_radar_aspect) && Valid(authored) && target.valid()) {
    const auto transform = gta4::aspect::RadarLayout(target);
    const auto displayed = transform.Map(authored);
    if (Valid(displayed) && !transform.identity()) {
      // The game's own setter updates both rectangles, the viewport matrices and the
      // projection offsets of this copy; the radar camera and the phase stay untouched.
      PPCContext call = ctx;
      call.r3.u32 = viewport;
      call.f1.f64 = displayed.left;
      call.f2.f64 = displayed.top;
      call.f3.f64 = displayed.right - displayed.left;
      call.f4.f64 = displayed.bottom - displayed.top;
      call.f5.f64 = Float(base, viewport + 680);
      call.f6.f64 = Float(base, viewport + 684);
      __imp__sub_828BDD38(call, base);
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true))
        REXLOG_INFO("gta4-radar: target={}x{} authored={},{},{},{} displayed={},{},{},{}",
                    target.width, target.height, authored.left, authored.top, authored.right,
                    authored.bottom, displayed.left, displayed.top, displayed.right,
                    displayed.bottom);
    }
  }
  const uint32_t token = gta4::aspect::StableDcToken(Read(base, command + 4));
  std::lock_guard lock(copies_mutex);
  const uint64_t serial = ++copies_serial;
  copies[command] = {token, serial};
  copies_order.emplace_back(command, serial);
  for (; copies_order.size() > kMaximumCopies; copies_order.pop_front()) {
    const auto [old_command, old_serial] = copies_order.front();
    const auto it = copies.find(old_command);
    if (it != copies.end() && it->second.serial == old_serial) copies.erase(it);
  }
}

}  // namespace

// While a gameplay radar copy is the bound viewport (render thread), radar vertices are local
// to it: gta4_aspect_hooks.cpp's CurrentUi/RadarUi must not lay them out a second time.
bool TouchRadarLocalViewport(uint8_t* base) noexcept {
  if (!Span(base, kCurrentViewport, 4)) return false;
  const uint32_t viewport = Read(base, kCurrentViewport);
  if (viewport < kCopiedViewportOffset) return false;
  const uint32_t command = viewport - kCopiedViewportOffset;
  if (!Span(base, command, 8) || Read(base, command) != kViewportCopyVtable) return false;
  const uint32_t token = gta4::aspect::StableDcToken(Read(base, command + 4));
  std::lock_guard lock(copies_mutex);
  const auto it = copies.find(command);
  return it != copies.end() && it->second.token == token;
}

}  // namespace gta4::input

// Queues the radar phase's viewport copy.
PPC_FUNC(sub_821BC7C8) {
  gta4::input::CopyRadarViewport(ctx, base);
}

// The radar phase's draw (it queues the radar's draw commands on the main thread): keep the
// gameplay radar's layers local to its viewport. The pause-menu map keeps the layout it had.
// EFLC sub_8236C200 is TU8 sub_8239C468, which analysis/tu8_to_eflc_functions.tsv left
// unmatched: TU8 sub_8239C3A8 is EFLC sub_8236C140 and both functions sit 0xC0 after it, the
// next one (TU8 sub_8239C9B8) 0x550 further in both, and EFLC 0x8236C200 / 0x8236C750 are
// slots 8 and 4 of the radar phase vtable 0x820131E4.
PPC_FUNC_IMPL(__imp__sub_8236C200);
PPC_FUNC(sub_8236C200) {
  using namespace gta4::input;
  const uint32_t phase = ctx.r3.u32;
  if (Span(base, phase, 4) && Read(base, phase) == kRadarRenderPhaseVtable &&
      GameplayRadar(base)) {
    const gta4::aspect::Scope scope(gta4::aspect::RadarLocalUi());
    __imp__sub_8236C200(ctx, base);
    return;
  }
  __imp__sub_8236C200(ctx, base);
}

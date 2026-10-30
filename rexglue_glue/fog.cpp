// gta4port: fog in the distance instead of the far city (gta4_fog_distance, files/fog).
//
// GTA IV's timecycle carries the view's far clip (FarClp: 1500 m in every weather of timecyc.dat)
// and the distance its fog starts at (FogSt: 9-147 m), at +68 and +72 of each 528-byte cycle
// record. sub_822DF0C8 writes the frame's record (the hour's cycle plus modifiers) into one of the
// double-buffered render slots. The camera takes its far plane from +68 of that slot
// (sub_82256568), and the render thread sets the fog from both fields (sub_822DDCB0: linear from
// FogSt to FarClp, up to the density in the high byte of +44).
//
// Capping the far clip leaves everything past it out of the camera's frustum: no culling work,
// draws or recording for the far city. The fog ends at the far clip, so the game's own fog closes
// in to cover the cut. FogSt shrinks in the same proportion, so the fog keeps the stock shape, only
// nearer. Records already under the cap (interiors, tunnels) stay as they are. Applies live.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>

REXCVAR_DEFINE_DOUBLE(gta4_fog_distance, 0.0, "GTA IV/Graphics",
                      "Farthest the camera sees, in meters, with the fog brought in to hide the "
                      "cut (0 = the game's own, 1500 m outdoors)")
    .range(0.0, 5000.0);

namespace gta4native {
namespace {

constexpr uint32_t kFarClipOffset = 68;
constexpr uint32_t kFogStartOffset = 72;

// Guest memory is big-endian; one aligned 32-bit access, as the game's own loads and stores.
float LoadGuestFloat(uint8_t* base, uint32_t address) {
  const uint32_t raw = __builtin_bswap32(
      reinterpret_cast<std::atomic<uint32_t>*>(base + address)->load(std::memory_order_relaxed));
  float value;
  memcpy(&value, &raw, sizeof(value));
  return value;
}

void StoreGuestFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t raw;
  memcpy(&raw, &value, sizeof(raw));
  reinterpret_cast<std::atomic<uint32_t>*>(base + address)
      ->store(__builtin_bswap32(raw), std::memory_order_relaxed);
}

}  // namespace

void LimitViewDistance(uint8_t* base, uint32_t cycle) {
  const float limit = float(REXCVAR_GET(gta4_fog_distance));
  if (!cycle || !(limit > 0.0f)) return;
  const float far_clip = LoadGuestFloat(base, cycle + kFarClipOffset);
  if (!std::isfinite(far_clip) || far_clip <= limit) return;
  StoreGuestFloat(base, cycle + kFarClipOffset, limit);
  const float fog_start = LoadGuestFloat(base, cycle + kFogStartOffset);
  if (std::isfinite(fog_start) && fog_start > 0.0f)
    StoreGuestFloat(base, cycle + kFogStartOffset, fog_start * (limit / far_clip));
}

}  // namespace gta4native

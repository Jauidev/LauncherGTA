// gta4port: keeps the sun shadows of the world still while the camera moves.
//
// The game warps its sun shadow map around the camera, u = x / (R + x) in the light's plane
// (PS_lightShadowDirectional and VS_WarpDepth), so resolution is spent near the camera. Two things
// make the shadows of buildings and rails look as if they followed the car:
// - the map is centred on the camera every frame (sub_8227D298 reads the position at viewport
//   +112), so its texels travel with the car and every shadow edge is resampled by them;
// - R is only 16 m (context +52 = R / 8), so a texel is 0.43 m at 50 m and 1.3 m at 100 m: the
//   steps of those edges are large enough to be seen moving.
// The warp is not translation invariant, so rounding the position to a texel grid can't fix this.
// Instead the centre stays where it is until the camera has moved gta4_shadow_hold_distance away
// on some axis, and a larger R gives the far texels 2-3 times the resolution. The price is an edge
// jump at each re-centre and softer shadows right next to the camera.
//
// sub_8227AF38 builds the caster culling planes from the same viewport: it gets the real position,
// or casters at the edge of the view would be culled.
#include <bit>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gta4_init.h"

PPC_FUNC_IMPL(__imp__sub_8227D298);
PPC_FUNC_IMPL(__imp__sub_8227AF38);

REXCVAR_DEFINE_DOUBLE(gta4_shadow_warp_radius, 64.0, "GTA IV/Graphics/Shadows",
                      "Metres around the camera that keep the sun shadow map's full resolution "
                      "(16 = the game's, larger = sharper far shadows, softer near ones, 0 = "
                      "leave the game's value)")
    .range(0.0, 512.0);
REXCVAR_DEFINE_DOUBLE(gta4_shadow_hold_distance, 8.0, "GTA IV/Graphics/Shadows",
                      "Metres the camera moves before the sun shadow map is re-centred (0 = every "
                      "frame, as the game does)")
    .range(0.0, 64.0);

namespace {

constexpr uint32_t kShadowQualityTable = 0x82C615E0;
constexpr uint32_t kShadowQualityContextStride = 0x100;
constexpr uint32_t kShadowContextCount = 2;
constexpr uint32_t kShadowWarpRadiusOffset = 0x34;
constexpr float kShadowWarpRadiusUnit = 8.0f;
constexpr uint32_t kViewportPosition = 112;

struct Position {
  uint32_t words[3];
};

Position LoadPosition(uint8_t* base, uint32_t address) {
  return {PPC_LOAD_U32(address), PPC_LOAD_U32(address + 4), PPC_LOAD_U32(address + 8)};
}

void StorePosition(uint8_t* base, uint32_t address, const Position& position) {
  PPC_STORE_U32(address, position.words[0]);
  PPC_STORE_U32(address + 4, position.words[1]);
  PPC_STORE_U32(address + 8, position.words[2]);
}

bool IsWithin(const Position& a, const Position& b, float distance) {
  for (int axis = 0; axis < 3; ++axis) {
    const float delta =
        std::bit_cast<float>(a.words[axis]) - std::bit_cast<float>(b.words[axis]);
    if (!(delta >= -distance && delta <= distance)) return false;
  }
  return true;
}

Position g_held;
bool g_has_held = false;
// The camera's real position while sub_8227D298 runs on the held one.
thread_local bool t_holding = false;
thread_local Position t_camera;

}  // namespace

PPC_FUNC(sub_8227D298) {
  // Written every time: the switch is live and the game may initialize the table again.
  if (const double radius = REXCVAR_GET(gta4_shadow_warp_radius); radius > 0.0) {
    const uint32_t value =
        std::bit_cast<uint32_t>(static_cast<float>(radius) / kShadowWarpRadiusUnit);
    for (uint32_t context = 0; context < kShadowContextCount; ++context) {
      PPC_STORE_U32(kShadowQualityTable + context * kShadowQualityContextStride +
                        kShadowWarpRadiusOffset,
                    value);
    }
  }

  const float hold = static_cast<float>(REXCVAR_GET(gta4_shadow_hold_distance));
  if (hold <= 0.0f) {
    g_has_held = false;
    __imp__sub_8227D298(ctx, base);
    return;
  }

  const uint32_t position = ctx.r3.u32 + kViewportPosition;
  t_camera = LoadPosition(base, position);
  if (!g_has_held || !IsWithin(t_camera, g_held, hold)) {
    g_held = t_camera;
    g_has_held = true;
    static uint32_t recentres = 0;
    if (++recentres <= 8) {
      REXLOG_INFO("gta4native: sun shadow map re-centred #{} at {:.1f} {:.1f} {:.1f}", recentres,
                  std::bit_cast<float>(g_held.words[0]), std::bit_cast<float>(g_held.words[1]),
                  std::bit_cast<float>(g_held.words[2]));
    }
  }
  StorePosition(base, position, g_held);
  t_holding = true;
  __imp__sub_8227D298(ctx, base);
  t_holding = false;
  StorePosition(base, position, t_camera);
}

PPC_FUNC(sub_8227AF38) {
  if (!t_holding) {
    __imp__sub_8227AF38(ctx, base);
    return;
  }
  const uint32_t position = ctx.r5.u32 + kViewportPosition;
  const Position held = LoadPosition(base, position);
  StorePosition(base, position, t_camera);
  __imp__sub_8227AF38(ctx, base);
  StorePosition(base, position, held);
}

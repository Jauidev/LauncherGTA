#pragma once

// gta4port: the native renderer (LibertyRecomp's gta4_native, rexglue_glue/ + third_party/rexglue)
// as seen by the runtime. GTA4_GPU=native: the fake GPU keeps consuming the ring buffer (fences,
// interrupts, vblank) and the game's D3D calls, caught by the ported hooks (rexglue_glue/eflc),
// are drawn by this renderer instead.
#include <cstdint>

struct ANativeWindow;

namespace gta4native {

// Creates the renderer over the runtime's guest memory. False if Vulkan could not start.
bool Initialize(uint8_t* guest_base, uint8_t* physical_base);
// population.cpp: scales GTA IV's ambient population multipliers (gta4_population_scale).
void StartPopulationScaling(uint8_t* guest_base);
// shadows_off.cpp: shrinks the sun-shadow range while gta4_native_shadow_interval is 0.
void StartShadowRangeWatch(uint8_t* guest_base);
bool IsActive();
// The activity's surface (Android); null when it goes away.
void SetSurface(ANativeWindow* window);

}  // namespace gta4native

// Hooks this runtime and the ported LibertyRecomp hooks both define: tools/port_native_hooks.py
// renames theirs to gta4native_sub_X, and ours start with GTA4_NATIVE_DISPATCH(sub_X), which hands
// the call to theirs while the native renderer runs.
#define GTA4_NATIVE_HOOK_DECLARE(name) PPC_FUNC(gta4native_##name)
#define GTA4_NATIVE_DISPATCH(name)       \
    if (::gta4native::IsActive())        \
    {                                    \
        gta4native_##name(ctx, base);    \
        return;                          \
    }

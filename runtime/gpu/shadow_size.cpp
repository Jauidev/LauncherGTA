// gta4port: smaller shadow maps.
//
// sub_8227A990(S, r4, r5) (re)creates the game's shadow render targets from one size unit S:
// POINT_SHADOW_MAP_1616_CACHE (8S), POINT_SHADOW_MAP_1616_CUBE (4S) and SHADOW_MAP_DIR /
// SHADOW_MAP_DIR_ALIAS (5S, the sun's cascades, 1280x1280 with the game's S = 256 passed by
// sub_8227FE50 with `li r3,256`). It keeps S in a global, so the rest of the renderer follows it.
//
// GTA4_SHADOW_SIZE=<S> replaces the 256 (128 = half the resolution, a quarter of the pixels);
// unset or 0 keeps the game's size.
#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <kernel/function.h>
#include <os/logger.h>
#include <gta4_native.h>

namespace
{
    uint32_t ShadowSize()
    {
        static const uint32_t size = [] {
            const char* value = getenv("GTA4_SHADOW_SIZE");
            return value ? uint32_t(strtoul(value, nullptr, 10)) : 0u;
        }();
        return size;
    }
}

PPC_FUNC_IMPL(__imp__sub_8227A990);
GTA4_NATIVE_HOOK_DECLARE(sub_8227A990);
PPC_FUNC(sub_8227A990)
{
    GTA4_NATIVE_DISPATCH(sub_8227A990);
    const uint32_t size = ShadowSize();
    if (size && ctx.r3.u32 == 256)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LOGF("shadow maps: size unit {} instead of 256 (sun {}x{})", size, size * 5, size * 5);
        }
        ctx.r3.u64 = size;
    }
    __imp__sub_8227A990(ctx, base);
}

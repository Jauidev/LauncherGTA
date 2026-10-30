// gta4port: draws the deferred G-buffer in fewer predicated tiles.
//
// The game renders its G-buffer (depth + 3 color targets) with 2x MSAA into EDRAM render targets
// one third of the screen tall, and draws the scene once per third with D3D's predicated tiling:
// sub_824F45C8 creates _DEFERRED_GBUFFER_{Z,0,1,2}_AA_ (height roundup32(height / 3), 2 samples,
// params + 4 of grcTextureFactoryXenon::CreateRenderTarget), sub_824F4F88 calls BeginTiling with 3
// rects, and sub_824F6988 resolves each target once per tile under SetPredication(3 << 2 * tile)
// before EndTiling. Every draw spanning more than one third is drawn again for each of them.
//
// Without MSAA (the Mali has no 2x, and Xenia dropped it anyway: GTA4_XENIA_NO_MSAA) the whole
// G-buffer fits the 10 MB of EDRAM in fewer tiles: one at 960x540, two at 1280x720. These hooks
// create the _AA_ targets with 1 sample and the new tile height, give BeginTiling that many tiles,
// and rewrite the resolve rects of the kept tiles. Resolves for tiles that no longer exist are
// dropped (the GPU would skip them anyway, being predicated to tiles that are never replayed).
//
// GTA4_GBUFFER_TILES=0 keeps the game's 3 tiles with 2x MSAA; =1..3 forces a tile count; unset
// picks the fewest that fit.
#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <os/logger.h>
#include <gta4_native.h>

namespace
{
    constexpr uint32_t kGBufferDepthAA = 0x82021BF0; // "_DEFERRED_GBUFFER_Z_AA_"
    constexpr uint32_t kGBufferColorAA[] = { 0x82021BC0, 0x82021B90, 0x82021B60 }; // _0_AA_, _1_AA_, _2_AA_
    constexpr uint32_t kGBufferHeight = 0x830169B0; // the G-buffer size used by sub_824F45C8
    constexpr uint32_t kGBufferWidth = 0x830169AC;
    constexpr uint32_t kEdramTiles = 2048; // 80x16 samples of 32 bits each

    // Requested tile count: -1 automatic, 0 = original game behaviour.
    int g_setting = -2;
    uint32_t g_tiles = 0; // chosen for the current G-buffer size, 0 = not changed
    uint32_t g_tileHeight = 0;
    uint32_t g_width = 0, g_height = 0;
    bool g_inTiling = false;
    uint32_t g_predication = 0;

    int Setting()
    {
        if (g_setting == -2)
        {
            const char* value = getenv("GTA4_GBUFFER_TILES");
            g_setting = value == nullptr || *value == '\0' ? -1 : std::clamp(atoi(value), 0, 3);
        }
        return g_setting;
    }

    uint32_t RoundUp32(uint32_t value) { return (value + 31) & ~31u; }

    // The game's own tile height for `count` tiles: height / count, rounded up to 32 rows.
    uint32_t TileHeight(uint32_t height, uint32_t count) { return RoundUp32(height / count); }

    // EDRAM tiles used by the 4 G-buffer surfaces (32 bits per sample) at 1x for a tile height.
    uint32_t EdramTilesUsed(uint32_t width, uint32_t tileHeight)
    {
        return 4 * ((width + 79) / 80) * ((tileHeight + 15) / 16);
    }

    void ChooseTiles(uint32_t width, uint32_t height)
    {
        g_width = width;
        g_height = height;
        g_tiles = 0;
        const int setting = Setting();
        if (setting == 0)
            return;
        uint32_t tiles = setting > 0 ? uint32_t(setting) : 3;
        if (setting < 0)
        {
            for (uint32_t count = 1; count <= 3; ++count)
            {
                if (EdramTilesUsed(width, TileHeight(height, count)) <= kEdramTiles)
                {
                    tiles = count;
                    break;
                }
            }
        }
        g_tiles = tiles;
        g_tileHeight = TileHeight(height, tiles);
        LOGFN("G-buffer {}x{}: {} tile(s) of {} rows without MSAA (the game uses 3 with 2x), {} of {} EDRAM tiles",
            width, height, tiles, g_tileHeight, EdramTilesUsed(width, g_tileHeight), kEdramTiles);
    }

    void WriteRect(uint8_t* base, uint32_t address, uint32_t tile)
    {
        PPC_STORE_U32(address + 0, 0);
        PPC_STORE_U32(address + 4, tile * g_tileHeight);
        PPC_STORE_U32(address + 8, g_width);
        PPC_STORE_U32(address + 12, std::min((tile + 1) * g_tileHeight, g_height));
    }
}

// grcTextureFactoryXenon::CreateRenderTarget(this, name, type, width, height, bpp, params)
// (vtable 0x82095FE4 + 56).
PPC_FUNC_IMPL(__imp__sub_828DC338);
GTA4_NATIVE_HOOK_DECLARE(sub_828DC338);
PPC_FUNC(sub_828DC338)
{
    GTA4_NATIVE_DISPATCH(sub_828DC338);
    const uint32_t name = ctx.r4.u32;
    if (name == kGBufferDepthAA)
        ChooseTiles(PPC_LOAD_U32(kGBufferWidth), PPC_LOAD_U32(kGBufferHeight));
    const bool gbuffer = name == kGBufferDepthAA || name == kGBufferColorAA[0] ||
        name == kGBufferColorAA[1] || name == kGBufferColorAA[2];
    if (gbuffer && g_tiles != 0 && ctx.r9.u32 != 0)
    {
        PPC_STORE_U32(ctx.r9.u32 + 4, 0); // multisample: none
        ctx.r7.u64 = g_tileHeight;
    }
    __imp__sub_828DC338(ctx, base);
}

// D3D BeginTiling(device, flags, count, rects, clear color, clear z, clear stencil): only called
// by sub_824F4F88 for the G-buffer.
PPC_FUNC_IMPL(__imp__sub_82A3E500);
GTA4_NATIVE_HOOK_DECLARE(sub_82A3E500);
PPC_FUNC(sub_82A3E500)
{
    GTA4_NATIVE_DISPATCH(sub_82A3E500);
    const uint32_t rects = ctx.r6.u32;
    if (g_tiles != 0 && ctx.r5.u32 == 3 && rects != 0 && PPC_LOAD_U32(rects + 8) == g_width &&
        PPC_LOAD_U32(rects + 32 + 12) == g_height)
    {
        for (uint32_t tile = 0; tile < g_tiles; ++tile)
            WriteRect(base, rects + 16 * tile, tile);
        ctx.r5.u64 = g_tiles;
        g_inTiling = true;
        g_predication = 0;
    }
    __imp__sub_82A3E500(ctx, base);
}

// D3D SetPredication(device, mask).
PPC_FUNC_IMPL(__imp__sub_82A3E390);
PPC_FUNC(sub_82A3E390)
{
    if (g_inTiling)
        g_predication = ctx.r4.u32;
    __imp__sub_82A3E390(ctx, base);
}

// D3D Resolve(device, flags, source rect, texture, dest point, ...): sub_824F6988 passes the same
// tile rect as source rect and dest point.
PPC_FUNC_IMPL(__imp__sub_82A3C858);
GTA4_NATIVE_HOOK_DECLARE(sub_82A3C858);
PPC_FUNC(sub_82A3C858)
{
    GTA4_NATIVE_DISPATCH(sub_82A3C858);
    if (g_inTiling && g_predication != 0 && ctx.r5.u32 != 0)
    {
        const uint32_t tile = uint32_t(std::countr_zero(g_predication)) / 2;
        if (tile >= g_tiles)
            return;
        WriteRect(base, ctx.r5.u32, tile);
        if (const uint32_t point = ctx.r7.u32; point != 0 && point != ctx.r5.u32)
        {
            PPC_STORE_U32(point + 0, 0);
            PPC_STORE_U32(point + 4, tile * g_tileHeight);
        }
    }
    __imp__sub_82A3C858(ctx, base);
}

// D3D EndTiling.
PPC_FUNC_IMPL(__imp__sub_82A3E998);
GTA4_NATIVE_HOOK_DECLARE(sub_82A3E998);
PPC_FUNC(sub_82A3E998)
{
    GTA4_NATIVE_DISPATCH(sub_82A3E998);
    g_inTiling = false;
    g_predication = 0;
    __imp__sub_82A3E998(ctx, base);
}

// gta4port: shader container dump for the native renderer work.
//
// sub_82A42798 / sub_82A428A8 are D3D CreatePixelShader / CreateVertexShader (TU8 0x82A42BA8 /
// 0x82A42CB8, matched by tools/match_functions.py). r3 is the shader container: a header whose
// words at +4 and +8 are its virtual and physical sizes. LibertyRecomp's native renderer keys its
// SPIR-V cache by XXH3_64bits over those virtual + physical bytes, so the same hash is used here.
//
// GTA4_SHADER_DUMP=<dir> writes each new container once as <dir>/<hash>.<vs|ps>.bin and appends
// "<hash> <vs|ps> <size>" to <dir>/shaders.txt.
#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <kernel/function.h>
#include <os/logger.h>
#include <gta4_native.h>
#include <unordered_set>

namespace
{
    const char* DumpDir()
    {
        static const char* dir = getenv("GTA4_SHADER_DUMP");
        return dir && *dir ? dir : nullptr;
    }

    void Dump(uint8_t* base, uint32_t container, const char* stage)
    {
        static std::mutex mutex;
        static std::unordered_set<uint64_t> seen;

        if (!container)
            return;
        const uint32_t virtualSize = ByteSwap(*reinterpret_cast<uint32_t*>(base + container + 4));
        const uint32_t physicalSize = ByteSwap(*reinterpret_cast<uint32_t*>(base + container + 8));
        const uint64_t total = uint64_t(virtualSize) + physicalSize;
        if (!total || total > 16 * 1024 * 1024)
            return;
        const uint64_t hash = XXH3_64bits(base + container, size_t(total));

        std::lock_guard lock(mutex);
        if (!seen.insert(hash).second)
            return;
        const std::string dir = DumpDir();
        char name[64];
        snprintf(name, sizeof(name), "/%016llX.%s.bin", static_cast<unsigned long long>(hash), stage);
        if (FILE* f = fopen((dir + name).c_str(), "wb"))
        {
            fwrite(base + container, 1, size_t(total), f);
            fclose(f);
        }
        if (FILE* f = fopen((dir + "/shaders.txt").c_str(), "a"))
        {
            fprintf(f, "%016llX %s %llu\n", static_cast<unsigned long long>(hash), stage,
                static_cast<unsigned long long>(total));
            fclose(f);
        }
        if (seen.size() % 100 == 1)
            LOGF("shader dump: {} containers", seen.size());
    }
}

PPC_FUNC_IMPL(__imp__sub_82A42798);
GTA4_NATIVE_HOOK_DECLARE(sub_82A42798);
PPC_FUNC(sub_82A42798)
{
    GTA4_NATIVE_DISPATCH(sub_82A42798);
    const uint32_t container = ctx.r3.u32;
    __imp__sub_82A42798(ctx, base);
    if (DumpDir() && ctx.r3.u32)
        Dump(base, container, "ps");
}

PPC_FUNC_IMPL(__imp__sub_82A428A8);
GTA4_NATIVE_HOOK_DECLARE(sub_82A428A8);
PPC_FUNC(sub_82A428A8)
{
    GTA4_NATIVE_DISPATCH(sub_82A428A8);
    const uint32_t container = ctx.r3.u32;
    __imp__sub_82A428A8(ctx, base);
    if (DumpDir() && ctx.r3.u32)
        Dump(base, container, "vs");
}

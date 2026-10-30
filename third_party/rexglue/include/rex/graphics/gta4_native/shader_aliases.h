#pragma once

#include <cstdint>
#include <utility>

// gta4port: the SPIR-V cache holds TU8's shaders by the hash of the whole container, and a container
// built from another source tree hashes differently although the program is the same. Without an
// alias its draws are dropped ("shader cache miss"): the cloud noise, every particle and the videos.
// The hash is replaced where the shader registers, so the hash-based families (kVideo, the TV trace)
// also see the cached one.
namespace rex::graphics::gta4_native {
inline constexpr std::pair<uint64_t, uint64_t> kShaderAliases[] = {
    // EFLC (xbox360_patch_e2). Checked against the cache's recovered source: same constant and
    // sampler registers and, for the vertex shader, the same literal constants (1, 2, -1).
    {0x88DC2287328E7C81ull, 0xA8FF8822E2585851ull},  // rage_perlinnoise vs_main
    {0xB0D0301E2408ECB3ull, 0x25DD95F97623A84Bull},  // rage_perlinnoise ps_main
    {0x8B22469C2A29096Bull, 0x5A2A2CE9A9ACE0B8ull},  // rage_perlinnoise ps_mainTweaked
    // The rest of EFLC (tools/iv_shaders/same_program.py): byte-identical to GTA IV's below but for the
    // 8-byte build signature that ends the microcode, same constant table, same registration order.
    {0x8ACE1C0E5BA7CC75ull, 0x0565A975401D2EFCull},  // rmptfx_default VS_Transform
    {0x84D7D379EDEC77C1ull, 0xD20B4303A9B07738ull},  // rmptfx_default PS_Draw
    {0x2E9785656DC3DB41ull, 0x20744CB73A0CC90Bull},  // rmptfx_default PS_DrawNonBlended
    {0xE7D73CD89CC1D59Aull, 0xB358BE68808024E2ull},  // rmptfx_litsprite VS_TransformFlat
    {0x0BF0B11007A7DCC6ull, 0x0BF799C249E138E5ull},  // rmptfx_litsprite VS_Transform
    {0xC9FE3F5D5FFB55D1ull, 0x1FED86FF7383F663ull},  // rmptfx_litsprite PS_DrawWaterDrop
    {0x1A6B7B379FFB3B09ull, 0xBDABFE60E122329Full},  // rmptfx_litsprite PS_DrawHeatHaze
    {0xB694070FE8C8FC38ull, 0x7DC718EBA3CEA0ACull},  // rmptfx_litsprite PS_Draw
    {0x242CE07C29F30260ull, 0x333FF1A52C92E8D6ull},  // gpuptfx_update VS_ragePassThroughNoXformTexOnly
    {0xE5CC339090D7A189ull, 0x95FB71598CFBE73Bull},  // gpuptfx_update ParticleUpdatePlanePS
    {0x6893D3BCF7D275C0ull, 0x00A6F26664F8B12Eull},  // gpuptfx_update ParticleUpdateTerrainPS
    {0x2A0EB9A545AE5E85ull, 0x7B29FD34FA01C2F4ull},  // gpuptfx_update ParticleUpdateTerrainGustPS
    {0x46A0265CFDE81ACEull, 0x18436E9729D41B4Aull},  // gpuptfx_update ParticleUpdatePlaneGustPS
    {0x9E8D53FD991D53FFull, 0x9826F389C213C00Eull},  // gpuptfx_update ParticleUpdatePlaneYUpPS
    {0x2318226AC4FCC00Bull, 0xC87CB7D70017230Full},  // gpuptfx_update ParticleUpdateTerrainYUpPS
    {0xB8F3F8B640CD5335ull, 0x2BDFD7849B4368F6ull},  // gpuptfx_update ParticleUpdateTerrainGustYUpPS
    {0xD180FC7218051E82ull, 0xEF6DDC2308B589B8ull},  // gpuptfx_update ParticleUpdatePlaneGustYUpPS
    {0xDCBAC507EBD89B24ull, 0x1E3874D56866D134ull},  // gpuptfx_update ptxgpuPackStateInitPS
    {0xEF544A243E8576E5ull, 0x156BAD4A9EE62726ull},  // rage_bink VS_Bink
    {0xB8F1889ED3CB6326ull, 0x9E76B68B60127349ull},  // rage_bink PS_Bink
    {0xA256E8E3FB2F8D5Aull, 0xA6C9E2B8B2A59D7Aull},  // rage_bink PS_BinkNoAlpha

    // GTA IV as on the disc, no title update (2026-10-30, tools/iv_shaders/pair.py). Its 24 misses
    // against the cache's 24 unused captured entries. Same stage and sampler mask, the cached
    // registers within the container's, the same literals; variants ordered by code size. Only
    // PS_Draw / PS_DrawNonBlended are told apart by size alone (identical registers).
    {0x0DEE31ED058CB291ull, 0xA8FF8822E2585851ull},  // rage_perlinnoise vs_main
    {0xB58D74F03C40DC17ull, 0x25DD95F97623A84Bull},  // rage_perlinnoise ps_main
    {0xDD1C337DBC4B4B1Aull, 0x5A2A2CE9A9ACE0B8ull},  // rage_perlinnoise ps_mainTweaked
    {0x6D57DD384ADF54B0ull, 0x0565A975401D2EFCull},  // rmptfx_default VS_Transform
    {0xC4CA7AC114D66FA6ull, 0xD20B4303A9B07738ull},  // rmptfx_default PS_Draw
    {0x2B31536ED2FE1C32ull, 0x20744CB73A0CC90Bull},  // rmptfx_default PS_DrawNonBlended
    {0xA49B63EE842244FEull, 0xB358BE68808024E2ull},  // rmptfx_litsprite VS_TransformFlat
    {0xBB16FBEED7B5B05Cull, 0x0BF799C249E138E5ull},  // rmptfx_litsprite VS_Transform
    {0x72B55A2DA0E74383ull, 0x1FED86FF7383F663ull},  // rmptfx_litsprite PS_DrawWaterDrop
    {0xED40FF7E9F8AAE36ull, 0xBDABFE60E122329Full},  // rmptfx_litsprite PS_DrawHeatHaze
    {0x2E3C3DB1FE03C714ull, 0x7DC718EBA3CEA0ACull},  // rmptfx_litsprite PS_Draw
    {0x088E43A43735745Bull, 0x333FF1A52C92E8D6ull},  // gpuptfx_update VS_ragePassThroughNoXformTexOnly
    {0x4784BA785BBB28E7ull, 0x95FB71598CFBE73Bull},  // gpuptfx_update ParticleUpdatePlanePS
    {0x13E7742FD8CA9A74ull, 0x18436E9729D41B4Aull},  // gpuptfx_update ParticleUpdatePlaneGustPS
    {0x0D6DE2A6CACF64EEull, 0x9826F389C213C00Eull},  // gpuptfx_update ParticleUpdatePlaneYUpPS
    {0xC86D2D4EF0A13F38ull, 0xEF6DDC2308B589B8ull},  // gpuptfx_update ParticleUpdatePlaneGustYUpPS
    {0xCAF1A68896F4434Dull, 0x00A6F26664F8B12Eull},  // gpuptfx_update ParticleUpdateTerrainPS
    {0xEB151A3DDEB89792ull, 0x7B29FD34FA01C2F4ull},  // gpuptfx_update ParticleUpdateTerrainGustPS
    {0x8F5AA1F9AD8D3E21ull, 0xC87CB7D70017230Full},  // gpuptfx_update ParticleUpdateTerrainYUpPS
    {0x30F457E639BC5DB8ull, 0x2BDFD7849B4368F6ull},  // gpuptfx_update ParticleUpdateTerrainGustYUpPS
    {0x6A82D8E1706AF5D8ull, 0x1E3874D56866D134ull},  // gpuptfx_update ptxgpuPackStateInitPS
    {0x6F6750E2EFD51CDBull, 0x156BAD4A9EE62726ull},  // rage_bink VS_Bink
    {0x9763494A2F038D96ull, 0x9E76B68B60127349ull},  // rage_bink PS_Bink
    {0x585ABFD4641D5BEEull, 0xA6C9E2B8B2A59D7Aull},  // rage_bink PS_BinkNoAlpha
};

constexpr uint64_t CanonicalShaderHash(uint64_t hash) {
  for (const auto& [from, to] : kShaderAliases) {
    if (hash == from) {
      return to;
    }
  }
  return hash;
}
}  // namespace rex::graphics::gta4_native

// gta4port: the sound thread's gain-ramped mix in native code (plan P6). sub_82199BA0 adds a
// 256-float block times a ramped gain into the mix buffer; the recompiled version was 16% of
// GtaSoundThread (offline audit 2026-10-27), with every lvx/stvx, vector copy and flush-mode check
// going through the context. LibertyRecomp hooks the same function (TU8 sub_82199BC8,
// rexglue_glue/eflc/gta4_audio_ramped_mix.cpp, not built here: it is instruction-identical to
// EFLC's) with gta4::audio::AccumulateRamped256, which keeps the guest's operation order and
// rounding: same additions, separate multiply and add, flush-to-zero.
//
// Off since 2026-10-27 (the user asked for the sound as before); files/audio_mix = 1 turns it on,
// live. GTA4_AUDIO_MIX_SELFTEST=1 runs the first 64 calls both ways and logs
// "gta4-audio-mix: native block differs" on any bit difference.
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gta4_audio_ramped_mix.h"
#include "gta4_init.h"

REXCVAR_DEFINE_BOOL(gta4_native_audio_mix, false, "GTA IV/Performance",
                    "Run the sound thread's gain-ramped block mix (sub_82199BA0) in native code");

namespace {

bool Contiguous(uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  return end <= 0x100000000ull && !(address < 0xE0000000u && end > 0xE0000000ull);
}

void NativeMix(PPCContext& __restrict ctx, uint8_t* base) {
  const uint64_t destination_register = ctx.r3.u64;
  const uint64_t source_register = ctx.r4.u64;
  const uint32_t destination = ctx.r3.u32 & ~15u;
  const uint32_t source = ctx.r4.u32 & ~15u;
  PPC_STORE_U64(ctx.r1.u32 - 8, ctx.r31.u64);
  ctx.fpscr.enableFlushMode();
  const auto result = gta4::audio::AccumulateRamped256(
      REX_RAW_ADDR(destination), REX_RAW_ADDR(source), REX_RAW_ADDR(ctx.r5.u32 & ~15u),
      REX_RAW_ADDR(ctx.r6.u32 & ~15u));
  // The recompiled leaf's scratch registers as it leaves them (its caller reloads them, but
  // nothing here relies on that).
  PPCVRegister* vectors[] = {&ctx.v0, &ctx.v1, &ctx.v2,  &ctx.v3,  &ctx.v4,  &ctx.v5,  &ctx.v6,
                             &ctx.v7, &ctx.v8, &ctx.v9, &ctx.v10, &ctx.v11, &ctx.v12, &ctx.v13};
  for (size_t i = 0; i < result.vectors.size(); ++i) {
    simde_mm_store_ps(vectors[i]->f32, result.vectors[i]);
  }
  ctx.r5.u64 = destination_register - source_register;
  ctx.r10.s64 = static_cast<int64_t>(destination_register) + 1072;
  ctx.r11.s64 = static_cast<int64_t>(source_register) + 1056;
  ctx.r8.s64 = static_cast<int64_t>(destination_register) + 960;
  ctx.r7.s64 = static_cast<int64_t>(destination_register) + 976;
  ctx.r6.u64 = destination_register + 992;
  ctx.r9.s64 = 0;
  ctx.r3.s64 = -32;
  ctx.r4.s64 = -16;
  ctx.r31.u64 = PPC_LOAD_U64(ctx.r1.u32 - 8);
}

bool SelfTestEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("GTA4_AUDIO_MIX_SELFTEST");
    return value && value[0] == '1';
  }();
  return enabled;
}

// Both versions from the same input; the guest's own result is the one kept.
void SelfTest(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint32_t> s_calls{0};
  static std::atomic<uint32_t> s_differences{0};
  const uint32_t call = s_calls.fetch_add(1, std::memory_order_relaxed);
  if (call >= 64) {
    __imp__sub_82199BA0(ctx, base);
    return;
  }
  uint8_t* destination = REX_RAW_ADDR(ctx.r3.u32 & ~15u);
  alignas(16) uint8_t before[1024];
  alignas(16) uint8_t native[1024];
  std::memcpy(before, destination, sizeof(before));
  PPCContext native_ctx = ctx;
  NativeMix(native_ctx, base);
  std::memcpy(native, destination, sizeof(native));
  std::memcpy(destination, before, sizeof(before));
  __imp__sub_82199BA0(ctx, base);
  const bool same_memory = std::memcmp(native, destination, sizeof(native)) == 0;
  bool same_registers = native_ctx.r3.u64 == ctx.r3.u64 && native_ctx.r4.u64 == ctx.r4.u64 &&
                        native_ctx.r5.u64 == ctx.r5.u64 && native_ctx.r6.u64 == ctx.r6.u64 &&
                        native_ctx.r7.u64 == ctx.r7.u64 && native_ctx.r8.u64 == ctx.r8.u64 &&
                        native_ctx.r9.u64 == ctx.r9.u64 && native_ctx.r10.u64 == ctx.r10.u64 &&
                        native_ctx.r11.u64 == ctx.r11.u64 && native_ctx.r31.u64 == ctx.r31.u64;
  const PPCVRegister* native_vectors = &native_ctx.v0;
  const PPCVRegister* guest_vectors = &ctx.v0;
  for (int i = 0; i < 14; ++i) {
    same_registers &= std::memcmp(native_vectors[i].u8, guest_vectors[i].u8, 16) == 0;
  }
  if (!same_memory || !same_registers) {
    s_differences.fetch_add(1, std::memory_order_relaxed);
    REXLOG_WARN("gta4-audio-mix: native block differs from the recompiled one (call {}, {})",
                call, same_memory ? "registers" : "samples");
  }
  if (call == 63) {
    REXLOG_INFO("gta4-audio-mix: self-test done, {} of 64 blocks differ",
                s_differences.load(std::memory_order_relaxed));
  }
}

}  // namespace

PPC_FUNC(sub_82199BA0) {
  if (!REXCVAR_GET(gta4_native_audio_mix) || !Contiguous(ctx.r3.u32 & ~15u, 1024) ||
      !Contiguous(ctx.r4.u32 & ~15u, 1024) || !Contiguous(ctx.r5.u32 & ~15u, 16) ||
      !Contiguous(ctx.r6.u32 & ~15u, 16)) {
    __imp__sub_82199BA0(ctx, base);
    return;
  }
  if (SelfTestEnabled()) [[unlikely]] {
    SelfTest(ctx, base);
    return;
  }
  NativeMix(ctx, base);
}

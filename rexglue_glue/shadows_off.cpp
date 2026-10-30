// gta4port: sun shadows off without the game preparing them.
//
// gta4_native_shadow_interval = 0 (Android's default since 2026-10-27) drops the sun-shadow draws
// in the recorder and leaves the map cleared, so nothing is shadowed. The game still built them:
// ~1,400 draws a frame went through its render thread and the capture only to be dropped. The
// cascades' reach is the range in GTA IV's shadow quality table (0x82C615E0, one context every
// 0x100 bytes, float at +0x14; written by ApplyShadowDistanceScale in eflc/gta4_native_hooks.cpp).
// While shadows are off this thread shrinks it to 2%, so the game's own culling finds next to no
// casters; the clears and resolves still run. It puts the range back when files/shadows turns them
// on, and takes any range the game or the port writes later as the new one to follow.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gta4_native.h"

REXCVAR_DECLARE(uint32_t, gta4_native_shadow_interval);

namespace gta4native {
namespace {

constexpr uint32_t kRanges[] = {0x82C615E0 + 0x14, 0x82C615E0 + 0x100 + 0x14};
constexpr float kOffFactor = 0.02f;

// Guest memory is big-endian; one aligned 32-bit access, as the game's own loads and stores.
uint32_t LoadGuestBits(uint8_t* base, uint32_t address) {
  return __builtin_bswap32(
      reinterpret_cast<std::atomic<uint32_t>*>(base + address)->load(std::memory_order_relaxed));
}

void StoreGuestBits(uint8_t* base, uint32_t address, uint32_t bits) {
  reinterpret_cast<std::atomic<uint32_t>*>(base + address)
      ->store(__builtin_bswap32(bits), std::memory_order_relaxed);
}

float FromBits(uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

uint32_t Bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

void ShadowRangeThread(uint8_t* base) {
  constexpr size_t kCount = sizeof(kRanges) / sizeof(kRanges[0]);
  uint32_t requested[kCount] = {};  // the range the game (or the port's scale) wants
  uint32_t written[kCount] = {};
  bool known[kCount] = {};
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const bool off = REXCVAR_GET(gta4_native_shadow_interval) == 0;
    for (size_t i = 0; i < kCount; ++i) {
      const uint32_t current = LoadGuestBits(base, kRanges[i]);
      if (!known[i] || current != written[i]) {
        // Not set up yet (the table fills when the shadow maps are created): wait.
        if (!std::isfinite(FromBits(current)) || FromBits(current) <= 0.0f) continue;
        requested[i] = current;
        known[i] = true;
      }
      const uint32_t wanted = off ? Bits(FromBits(requested[i]) * kOffFactor) : requested[i];
      if (wanted != current) {
        StoreGuestBits(base, kRanges[i], wanted);
        REXLOG_INFO("gta4-shadows: context {} range {} -> {} (sun shadows {})", i,
                    FromBits(current), FromBits(wanted), off ? "off" : "on");
      }
      written[i] = wanted;
    }
  }
}

}  // namespace

void StartShadowRangeWatch(uint8_t* guest_base) {
  std::thread(ShadowRangeThread, guest_base).detach();
}

}  // namespace gta4native

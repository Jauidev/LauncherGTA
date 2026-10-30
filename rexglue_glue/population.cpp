// gta4port: fewer cars and pedestrians to take simulation work off the game's main thread (AI,
// physics and streaming of every ambient vehicle and ped), which sets the frame rate on the phone.
//
// GTA IV keeps its population multipliers in five floats; the game and its scripts write them
// (startup resets, per-area setters, mission natives) and the ambient generators read them.
// LibertyRecomp scales them from hooks on the setters (gta4_population_hooks.cpp, not built here):
// the five setters with an EFLC address are hooked the same way below, and a thread every 250 ms
// catches the writes of the two that have none (sub_82598ED0/EE8 in TU8, the script natives for
// random and parked cars; rexglue_glue/eflc/translate.log). The globals' EFLC addresses are
// matched by 3-6 votes each (analysis/tu8_to_eflc_addresses.tsv). A value the game wrote since our
// last store is its new request.
//
// gta4_population_scale applies live: the switch thread can change it in game (files/density).
// Default 1 (as the game asks): on 2026-10-27 0.6 gave no measurable fps on the phone, uncapped,
// in the same session. 0 suppresses ambient population; non-positive requests (the game's own
// "none" during missions) are never touched.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "eflc/gta4_population_policy.h"
#include "gta4_init.h"

REXCVAR_DEFINE_DOUBLE(gta4_population_scale, 1.0, "GTA IV/Population",
                      "Scale of GTA IV's ambient traffic, parked cars and pedestrians (1 = as the "
                      "game asks)")
    .range(0.0, 4.0);

namespace gta4native {
namespace {

// EFLC addresses (TU8 0x82AA1888, 0x82AA188C, 0x82AA2940, 0x82AA2944, 0x82AA2948).
constexpr uint32_t kMultipliers[] = {
    0x82AA17C8,  // random (ambient) cars
    0x82AA17CC,  // parked cars
    0x82AA2880,  // ambient pedestrians
    0x82AA2884,  // scenario pedestrians, current
    0x82AA2888,  // scenario pedestrians, next
};
constexpr const char* kNames[] = {"traffic", "parked-cars", "peds", "scenario-peds",
                                  "scenario-peds-next"};

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

constexpr size_t kCount = sizeof(kMultipliers) / sizeof(kMultipliers[0]);
std::mutex g_mutex;
uint32_t g_requested[kCount] = {};  // bit patterns, so a NaN request still compares
uint32_t g_written[kCount] = {};
bool g_known[kCount] = {};
double g_scale = -1.0;  // the scale g_written was computed with

uint32_t Bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

// Under g_mutex. force: the game just wrote this multiplier (a hooked setter returned).
void Apply(uint8_t* base, size_t i, double scale, bool force) {
  const float current = LoadGuestFloat(base, kMultipliers[i]);
  const uint32_t current_bits = Bits(current);
  if (force || !g_known[i] || current_bits != g_written[i]) {
    g_requested[i] = current_bits;  // a write of the game's since our last store
    g_known[i] = true;
  } else if (scale == g_scale) {
    return;
  }
  float request;
  memcpy(&request, &g_requested[i], sizeof(request));
  const float effective = gta4::population::ScaleMultiplier(request, scale);
  if (Bits(effective) != current_bits) StoreGuestFloat(base, kMultipliers[i], effective);
  g_written[i] = Bits(effective);
  if (g_written[i] != g_requested[i]) {
    REXLOG_INFO("gta4-population: {} requested {} effective {}", kNames[i], request, effective);
  }
}

void ApplyWritten(uint8_t* base, std::initializer_list<size_t> indices) {
  std::lock_guard lock(g_mutex);
  const double scale = REXCVAR_GET(gta4_population_scale);
  for (const size_t i : indices) Apply(base, i, scale, true);
}

void PopulationThread(uint8_t* base) {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::lock_guard lock(g_mutex);
    const double scale = REXCVAR_GET(gta4_population_scale);
    for (size_t i = 0; i < kCount; ++i) Apply(base, i, scale, false);
    if (scale != g_scale) {
      REXLOG_INFO("gta4-population: scale {} -> {}", g_scale < 0 ? 1.0 : g_scale, scale);
      g_scale = scale;
    }
  }
}

}  // namespace

void StartPopulationScaling(uint8_t* guest_base) {
  std::thread(PopulationThread, guest_base).detach();
}

}  // namespace gta4native

// The setters (EFLC addresses of LibertyRecomp's hooks): the game's write, then ours.
// Script/native handler: random and parked vehicle multipliers.
PPC_FUNC(sub_825C0898) {
  __imp__sub_825C0898(ctx, base);
  gta4native::ApplyWritten(base, {0, 1});
}
// Canonical ambient pedestrian multiplier setter.
PPC_FUNC(sub_823F2A28) {
  __imp__sub_823F2A28(ctx, base);
  gta4native::ApplyWritten(base, {2});
}
// Canonical current/next scenario pedestrian multiplier setter.
PPC_FUNC(sub_823F2A38) {
  __imp__sub_823F2A38(ctx, base);
  gta4native::ApplyWritten(base, {3, 4});
}
// Vehicle population startup reset.
PPC_FUNC(sub_823A51E8) {
  __imp__sub_823A51E8(ctx, base);
  gta4native::ApplyWritten(base, {0, 1});
}
// Pedestrian population startup reset.
PPC_FUNC(sub_823F47C8) {
  __imp__sub_823F47C8(ctx, base);
  gta4native::ApplyWritten(base, {2, 3, 4});
}

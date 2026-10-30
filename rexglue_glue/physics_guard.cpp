// gta4port: guards on the physics collision task. The RAGE task workers (sub_8285CD38) run
// sub_829593E8, which calls sub_82959000 for each 48-byte pair of instances of an array. Twice a
// worker found an instance half torn down:
// - 2026-10-28 (0.5.0D, swimming into the sea while a police helicopter shot at the player): with
//   both members of the pair the same (r4 == r5) sub_82959000 takes an object from a table,
//   *(*(r3 + 4) + 128), at the u16 index the member keeps at +8, and calls slot 7 of its vtable.
//   That entry was garbage (0x026A07DE), its vtable read 0 and the call faulted at 0x1C.
// - 2026-10-29 (0.6.3D, ~3 min into play, driving into fences that break): sub_8296DE58 asks for
//   the world matrices of a contact's two instances, sub_8296DB78 for the one at +52 (part index at
//   +104) and sub_8296DCE8 for the one at +56 (part at +112). Each copies the instance's matrix
//   (64 bytes at +16) and, for a composite bound (type 12), applies the part's matrix. The
//   instance's archetype read 0 and the bound load faulted at 0xC.
// Why the task sees such instances is not known yet (breakable objects being swapped for their
// pieces is the lead). Each guard below gives what the game's own code gives when there is no
// instance, and logs it. The checks and the original code are not atomic: an instance torn down
// between them can still crash.
//
// Layout, as the code reads it:
// - instance (phInst): +0 vtable, +4 archetype, +8 u16 level index (0xFFFF = not in the level),
//   +16 matrix.
// - archetype: +12 bound, +16 type flags, +20 include flags. bound: +0 vtable, +4 u8 type.
// - contact point: +160 manifold. manifold: +52/+56 instances, +104/+112 their part indices.
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/logging.h>

#include "gta4_init.h"

PPC_FUNC_IMPL(__imp__sub_82959000);
PPC_FUNC_IMPL(__imp__sub_8296B5C8);
PPC_FUNC_IMPL(__imp__sub_8296B8D0);
PPC_FUNC_IMPL(__imp__sub_8296DB78);
PPC_FUNC_IMPL(__imp__sub_8296DCE8);
PPC_FUNC_IMPL(__imp__sub_829771F8);

namespace {

enum Guard { kPairTable, kPair, kMatrix, kMaterial, kHidden, kGuardCount };
std::atomic<uint32_t> g_counts[kGuardCount];

// The two instances of the pair this worker is in (0 outside sub_82959000), for the log.
thread_local uint32_t t_pair[2];

// Heap memory: the virtual heap or the physical views (the game's objects are at 0xA...).
bool IsHeap(uint32_t address) {
  return (address & 3) == 0 &&
         ((address >= 0x40000000 && address < 0x80000000) || address >= 0xA0000000);
}

// The XEX image: vtables and code.
bool InImage(uint32_t address) {
  return (address & 3) == 0 && address >= 0x82000000 && address < 0x84000000;
}

// Memory an object can be in: the heaps (and stacks) or the image's data.
bool IsObject(uint32_t address) { return IsHeap(address) || InImage(address); }

// Everything the collision code reads from an instance without checking: its vtable, archetype
// and bound, and the bound's vtable.
bool InstanceUsable(uint8_t* base, uint32_t instance) {
  if (!IsObject(instance) || !InImage(PPC_LOAD_U32(instance))) return false;
  const uint32_t archetype = PPC_LOAD_U32(instance + 4);
  if (!IsObject(archetype)) return false;
  const uint32_t bound = PPC_LOAD_U32(archetype + 12);
  return IsObject(bound) && InImage(PPC_LOAD_U32(bound));
}

// Counts an event of a guard; true for the ones worth a log line (the first 10, then every 100th).
bool Count(Guard guard, uint32_t& count) {
  count = g_counts[guard].fetch_add(1, std::memory_order_relaxed) + 1;
  return count <= 10 || count % 100 == 0;
}

// Whose instance it was: one of the pair being processed, or one a manifold kept from before.
const char* Whose(uint32_t instance) {
  if (!t_pair[0]) return "outside a pair";
  return instance == t_pair[0] || instance == t_pair[1] ? "of this pair" : "kept by the manifold";
}

uint32_t Manifold(uint8_t* base, uint32_t contact) {
  return IsObject(contact) ? PPC_LOAD_U32(contact + 160) : 0;
}

// Manifold instances set to 0 (no instance, which the code checks for) while a function runs, and
// put back after it unless the game changed the field meanwhile.
class HiddenInstances {
 public:
  explicit HiddenInstances(uint8_t* base) : base_(base) {}
  HiddenInstances(const HiddenInstances&) = delete;
  HiddenInstances& operator=(const HiddenInstances&) = delete;
  ~HiddenInstances() {
    for (uint32_t i = 0; i < count_; ++i) {
      Exchange(address_[i], 0, __builtin_bswap32(instance_[i]));
    }
  }

  void HideTornDown(uint32_t manifold) {
    if (!IsObject(manifold)) return;
    for (const uint32_t offset : {52u, 56u}) {
      uint8_t* base = base_;
      const uint32_t instance = PPC_LOAD_U32(manifold + offset);
      if (instance == 0 || InstanceUsable(base, instance) || count_ == kCapacity) continue;
      if (!Exchange(manifold + offset, __builtin_bswap32(instance), 0)) continue;
      address_[count_] = manifold + offset;
      instance_[count_++] = instance;
      uint32_t count;
      if (Count(kHidden, count)) {
        REXLOG_WARN("gta4-physics-guard: manifold 0x{:08X} instance 0x{:08X} ({}) torn down: "
                    "left out ({} so far)",
                    manifold, instance, Whose(instance), count);
      }
    }
  }

 private:
  static constexpr uint32_t kCapacity = 32;

  // Compare-and-swap of a guest word (values as stored, big-endian).
  bool Exchange(uint32_t address, uint32_t expected, uint32_t desired) const {
    return __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(base_ + address), &expected,
                                       desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  }

  uint8_t* base_;
  uint32_t count_ = 0;
  uint32_t address_[kCapacity];
  uint32_t instance_[kCapacity];
};

// For sub_8296DB78 / sub_8296DCE8: false when the instance at contact + instance_offset is there but
// not usable; the matrix at r4 is then written here: the instance's own (what a bound that is not
// composite gets), or identity (the function's path for no instance) if even that is not readable.
bool InstanceMatrixReadable(PPCContext& ctx, uint8_t* base, uint32_t instance_offset) {
  const uint32_t contact = ctx.r3.u32;
  const uint32_t instance = IsObject(contact) ? PPC_LOAD_U32(contact + instance_offset) : 1;
  if (instance == 0 || InstanceUsable(base, instance)) return true;

  const uint32_t matrix = ctx.r4.u32 & ~0xFu;
  if (IsObject(instance)) {
    std::memcpy(base + matrix, base + ((instance + 16) & ~0xFu), 64);
  } else {
    // Identity, as the function writes it (the fourth column is left alone).
    for (const uint32_t offset : {4u, 8u, 16u, 24u, 32u, 36u}) PPC_STORE_U32(matrix + offset, 0);
    for (const uint32_t offset : {0u, 20u, 40u}) PPC_STORE_U32(matrix + offset, 0x3F800000);
    std::memset(base + matrix + 48, 0, 16);
  }
  uint32_t count;
  if (Count(kMatrix, count)) {
    REXLOG_WARN("gta4-physics-guard: contact 0x{:08X} instance 0x{:08X} ({}) torn down: {} matrix "
                "({} so far)",
                contact, instance, Whose(instance), IsObject(instance) ? "instance" : "identity",
                count);
  }
  return false;
}

}  // namespace

PPC_FUNC(sub_82959000) {
  const uint32_t a = ctx.r4.u32, b = ctx.r5.u32;
  if (a == b) {
    const uint32_t level = IsHeap(ctx.r3.u32) ? PPC_LOAD_U32(ctx.r3.u32 + 4) : 0;
    const uint32_t table = IsHeap(level) ? PPC_LOAD_U32(level + 128) : 0;
    const uint32_t index = PPC_LOAD_U16(a + 8);
    const uint32_t object = IsHeap(table) ? PPC_LOAD_U32(table + index * 4) : 0;
    const uint32_t vtable = IsHeap(object) ? PPC_LOAD_U32(object) : 0;
    const uint32_t method = InImage(vtable) ? PPC_LOAD_U32(vtable + 28) : 0;
    if (!InImage(method)) {
      uint32_t count;
      if (Count(kPairTable, count)) {
        REXLOG_WARN(
            "gta4-physics-guard: pair 0x{:08X} index {} -> object 0x{:08X} vtable 0x{:08X}: "
            "skipped ({} so far)",
            a, index, object, vtable, count);
      }
      return;
    }
  } else if (IsObject(a) && IsObject(b)) {
    // The handler only goes on (and reads both archetypes) when both level indices are below the
    // level's object count (u16 at +38 of the level at 0x831CC78C).
    const uint32_t level = PPC_LOAD_U32(0x831CC78C);
    const uint32_t objects = IsObject(level) ? PPC_LOAD_U16(level + 38) : 0;
    if (PPC_LOAD_U16(a + 8) < objects && PPC_LOAD_U16(b + 8) < objects &&
        (!InstanceUsable(base, a) || !InstanceUsable(base, b))) {
      uint32_t count;
      if (Count(kPair, count)) {
        REXLOG_WARN("gta4-physics-guard: pair 0x{:08X} / 0x{:08X} torn down: skipped ({} so far)",
                    a, b, count);
      }
      return;
    }
  }
  t_pair[0] = a;
  t_pair[1] = b;
  __imp__sub_82959000(ctx, base);
  t_pair[0] = t_pair[1] = 0;
}

PPC_FUNC(sub_8296DB78) {
  if (InstanceMatrixReadable(ctx, base, 52)) __imp__sub_8296DB78(ctx, base);
}

PPC_FUNC(sub_8296DCE8) {
  if (InstanceMatrixReadable(ctx, base, 56)) __imp__sub_8296DCE8(ctx, base);
}

// The material id of one side of a contact point (r4 != 0: the instance at +52, else +56), from
// its bound. 0 (what the function gives for no instance, or one not in the level) for a torn-down
// instance.
PPC_FUNC(sub_8296B5C8) {
  const uint32_t manifold = Manifold(base, ctx.r3.u32);
  const uint32_t instance =
      IsObject(manifold) ? PPC_LOAD_U32(manifold + ((ctx.r4.u32 & 0xFF) ? 52 : 56)) : 1;
  if (instance != 0 && !InstanceUsable(base, instance)) {
    const bool in_level = IsObject(instance) && PPC_LOAD_U16(instance + 8) != 0xFFFF;
    uint32_t count;
    if (in_level && Count(kMaterial, count)) {
      REXLOG_WARN("gta4-physics-guard: contact 0x{:08X} instance 0x{:08X} ({}) torn down: "
                  "material 0 ({} so far)",
                  ctx.r3.u32, instance, Whose(instance), count);
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8296B5C8(ctx, base);
}

// A contact point's friction and elasticity, which also calls a vtable method on each instance.
PPC_FUNC(sub_8296B8D0) {
  HiddenInstances hidden(base);
  hidden.HideTornDown(Manifold(base, ctx.r3.u32));
  __imp__sub_8296B8D0(ctx, base);
}

// Goes through a list of contact points (r4: +0 array, +44 count) and calls a vtable method on the
// instances of each one's manifold.
PPC_FUNC(sub_829771F8) {
  HiddenInstances hidden(base);
  const uint32_t list = ctx.r4.u32;
  const uint32_t entries = IsObject(list) ? PPC_LOAD_U32(list) : 0;
  const int32_t points = IsObject(entries) ? int32_t(PPC_LOAD_U32(list + 44)) : 0;
  for (int32_t i = 0; i < points && i < 4096; ++i) {
    hidden.HideTornDown(Manifold(base, PPC_LOAD_U32(entries + i * 4)));
  }
  __imp__sub_829771F8(ctx, base);
}

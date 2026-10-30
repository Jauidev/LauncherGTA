// gta4port: the RAGE task pool (256 tasks of 160 bytes at 0x831AB858; a task is free while its
// function, +4, is 0). sub_8285D3A8 takes a free task and queues it, or returns 0 when none is free;
// its caller later waits on the task's event (+156) in sub_8285CB40/sub_8285CC68, which also frees
// it. Twice the pool ran out ~200 s into a session: a waiter then read the event of a null task
// (handle 0), the wait failed at once, the waiter freed tasks the workers had not run yet, and the
// workers crashed calling function 0 (the Xbox never ran out: there a null task crashes at once).
//
// The tasks were the world visibility scan's: sub_821F2560 queues one task per run of 10-16 grid
// cells in each row the camera sees and appends it to a list of 200 entries (0x82B8F210, count at
// 0x82BA90C4, waited on by sub_821F5EC8), without checking either limit. With an open view on the
// wide screen (the horizontal field of view grows with the aspect ratio) a frame asked for 220+
// tasks: the pool ran out and the list overwrote the data after it. Past kVisibilityTaskLimit the
// cells are now scanned on the calling thread, as the task would on a worker.
//
// This also logs who holds the pool's tasks when it is half used or full, and makes a wait with
// null tasks wait for the real ones before freeing them.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

#include <rex/hook.h>
#include <rex/logging.h>

#include "gta4_init.h"

PPC_FUNC_IMPL(__imp__sub_8285D3A8);
PPC_FUNC_IMPL(__imp__sub_8285CB40);
PPC_FUNC_IMPL(__imp__sub_821F2560);

namespace gta4::task_pool {
namespace {

constexpr uint32_t kPoolLock = 0x831AB830;
// Queue pointers (sub_8285D318 makes them), their count at +32; a queue's ring holds its queued
// tasks, their number at ring +1064.
constexpr uint32_t kQueues = 0x831AB834;
constexpr uint32_t kRingOffset = 8;
constexpr uint32_t kRingCountOffset = 1064;
constexpr uint32_t kPoolBase = 0x831AB858;
constexpr uint32_t kTaskSize = 160;
constexpr uint32_t kTaskCount = 256;
constexpr uint32_t kFunctionOffset = 4;
constexpr uint32_t kEventOffset = 156;
// sub_8285CB40's frame: 1184 bytes, room for 276 handles from +80; it waits on 64 at a time.
constexpr uint32_t kWaitFrameSize = 1184;
constexpr uint32_t kWaitHandlesOffset = 80;
constexpr uint32_t kMaxWaitTasks = (kWaitFrameSize - kWaitHandlesOffset) / 4;
constexpr uint32_t kHandlesPerWait = 64;
// The visibility scan's task list holds 200 entries; the margin keeps it below that.
constexpr uint32_t kVisibilityTaskCount = 0x82BA90C4;
constexpr uint32_t kVisibilityTaskLimit = 192;
// Set to 1 by the visibility task (sub_821F2448) in its thread's TLS block.
constexpr uint32_t kVisibilityThreadFlag = 1600;

std::atomic<uint32_t> g_owner[kTaskCount];
std::atomic<int64_t> g_last_report_ms{0};
std::atomic<uint32_t> g_failures{0};
std::atomic<int64_t> g_last_inline_report_ms{0};
std::atomic<uint32_t> g_inline_runs{0};

bool IsPoolTask(uint32_t task) {
  return task >= kPoolBase && task < kPoolBase + kTaskCount * kTaskSize &&
         (task - kPoolBase) % kTaskSize == 0;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Logs when the pool is at least half used, or always when an allocation failed. Queued tasks
// have not run yet; the rest ran (or run now) and wait for their caller to free them.
void Report(uint8_t* base, const char* reason, bool always) {
  std::map<uint32_t, uint32_t> owners;  // caller -> tasks in use
  uint32_t used = 0;
  for (uint32_t slot = 0; slot < kTaskCount; ++slot) {
    const uint32_t task = kPoolBase + slot * kTaskSize;
    if (PPC_LOAD_U32(task + kFunctionOffset) != 0) {
      ++used;
      ++owners[g_owner[slot].load(std::memory_order_relaxed)];
    }
  }
  if (!always && used < kTaskCount / 2) return;
  uint32_t queued = 0;
  const uint32_t queues = std::min<uint32_t>(PPC_LOAD_U32(kQueues + 32), 8);
  for (uint32_t i = 0; i < queues; ++i) {
    const uint32_t queue = PPC_LOAD_U32(kQueues + i * 4);
    if (queue) queued += PPC_LOAD_U32(queue + kRingOffset + kRingCountOffset);
  }
  std::string text;
  for (const auto& [caller, count] : owners) {
    char entry[32];
    std::snprintf(entry, sizeof(entry), " %08X x%u", caller, count);
    text += entry;
  }
  REXLOG_INFO("gta4-task-pool: {} {} of {} tasks in use ({} queued), by caller:{} "
              "(failed allocations {})",
              reason, used, kTaskCount, queued, text, g_failures.load(std::memory_order_relaxed));
}

// sub_8285CB40 with wait-all for a list holding entries that are not pool tasks (null ones from a
// failed allocation, or anything else an overflowing list picked up): waits for the events of the
// real tasks only, then frees them under the pool lock as sub_8285CB40 does. The null tasks' work
// never ran.
void WaitForRealTasks(PPCContext& ctx, uint8_t* base, uint32_t count, uint32_t tasks) {
  rex::CallFrame call(ctx);
  const uint32_t frame = ctx.r1.u32 - kWaitFrameSize;
  PPC_STORE_U32(frame, ctx.r1.u32);
  const uint32_t handles = frame + kWaitHandlesOffset;
  uint32_t waits = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t task = PPC_LOAD_U32(tasks + i * 4);
    if (IsPoolTask(task)) PPC_STORE_U32(handles + waits++ * 4, PPC_LOAD_U32(task + kEventOffset));
  }
  for (uint32_t done = 0; done < waits; done += kHandlesPerWait) {
    call.ctx.r1.u64 = frame;
    call.ctx.r3.u64 = std::min(waits - done, kHandlesPerWait);
    call.ctx.r4.u64 = handles + done * 4;
    call.ctx.r5.u64 = 1;
    call.ctx.r6.s64 = -1;
    sub_82A12C38(call.ctx, base);
  }
  call.ctx.r1.u64 = frame;
  call.ctx.r3.u64 = kPoolLock;
  sub_821EC1C0(call.ctx, base);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t task = PPC_LOAD_U32(tasks + i * 4);
    if (IsPoolTask(task)) PPC_STORE_U32(task + kFunctionOffset, 0);
  }
  std::atomic_thread_fence(std::memory_order_acq_rel);
  PPC_STORE_U32(kPoolLock, 0);
  ctx.r3.u64 = 0;
}

// What the visibility task sub_821F2448 does on a worker for the cells first..last of a row:
// scans each cell with sub_821F0B08 (flag set) or sub_821F0888, its thread flag set meanwhile.
void ScanVisibilityCells(PPCContext& ctx, uint8_t* base, int32_t first, int32_t last, int32_t row,
                         bool flag) {
  if (last < first) std::swap(first, last);
  const uint32_t tls = PPC_LOAD_U32(ctx.r13.u32 + 0);
  const uint32_t saved_flag = PPC_LOAD_U32(tls + kVisibilityThreadFlag);
  PPC_STORE_U32(tls + kVisibilityThreadFlag, 1);
  rex::CallFrame call(ctx);
  for (int32_t cell = first; cell <= last; ++cell) {
    call.ctx.r1.u64 = ctx.r1.u64;
    call.ctx.r3.s64 = cell;
    call.ctx.r4.s64 = row;
    call.ctx.r5.s64 = 0;
    if (flag) {
      sub_821F0B08(call.ctx, base);
    } else {
      sub_821F0888(call.ctx, base);
    }
  }
  PPC_STORE_U32(tls + kVisibilityThreadFlag, saved_flag);
}

}  // namespace
}  // namespace gta4::task_pool

PPC_FUNC(sub_8285D3A8) {
  using namespace gta4::task_pool;
  // Test only: GTA4_TASK_POOL_FAIL_EVERY=N fails every Nth allocation as a full pool does.
  static const uint32_t fail_every = [] {
    const char* value = std::getenv("GTA4_TASK_POOL_FAIL_EVERY");
    return value ? uint32_t(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  static std::atomic<uint32_t> allocations{0};
  const uint32_t caller = uint32_t(ctx.lr);
  if (fail_every &&
      allocations.fetch_add(1, std::memory_order_relaxed) % fail_every == fail_every - 1) {
    ctx.r3.u64 = 0;
  } else {
    __imp__sub_8285D3A8(ctx, base);
  }
  const uint32_t task = ctx.r3.u32;
  const int64_t now = NowMs();
  if (task >= kPoolBase && task < kPoolBase + kTaskCount * kTaskSize) {
    g_owner[(task - kPoolBase) / kTaskSize].store(caller, std::memory_order_relaxed);
  } else {
    g_failures.fetch_add(1, std::memory_order_relaxed);
  }
  int64_t last = g_last_report_ms.load(std::memory_order_relaxed);
  const bool full = task == 0;
  if ((now - last >= (full ? 1000 : 10000)) &&
      g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    Report(base, full ? "FULL (allocation failed):" : "over half used:", full);
  }
}

PPC_FUNC(sub_8285CB40) {
  // r3 tasks at r4, r5 wait for all (every caller) or any. A null task's event is handle 0 (and
  // a stray entry's is anything), which makes the whole wait fail at once and free tasks still
  // queued.
  using namespace gta4::task_pool;
  const uint32_t count = ctx.r3.u32;
  const uint32_t tasks = ctx.r4.u32;
  uint32_t nulls = 0;
  uint32_t strays = 0;
  for (uint32_t i = 0; i < count && i < kMaxWaitTasks; ++i) {
    const uint32_t task = PPC_LOAD_U32(tasks + i * 4);
    nulls += task == 0;
    strays += task != 0 && !IsPoolTask(task);
  }
  if (!nulls && !strays) {
    __imp__sub_8285CB40(ctx, base);
    return;
  }
  const bool handled = (ctx.r5.u32 & 0xFF) != 0 && int32_t(count) > 0 && count <= kMaxWaitTasks;
  REXLOG_WARN("gta4-task-pool: caller {:08X} waits on {} tasks, {} null and {} not tasks{}",
              uint32_t(ctx.lr), count, nulls, strays, handled ? "; waiting for the others" : "");
  if (handled) {
    WaitForRealTasks(ctx, base, count, tasks);
  } else {
    __imp__sub_8285CB40(ctx, base);
  }
}

PPC_FUNC(sub_821F2560) {
  // Queues the visibility scan of cells r4..r6 of row r7 (r8: which scan) and appends the task to
  // the list; past the limit the cells are scanned here instead.
  using namespace gta4::task_pool;
  if (PPC_LOAD_U32(kVisibilityTaskCount) < kVisibilityTaskLimit) {
    __imp__sub_821F2560(ctx, base);
    return;
  }
  ScanVisibilityCells(ctx, base, ctx.r4.s32, ctx.r6.s32, ctx.r7.s32, ctx.r8.u32 != 0);
  const uint32_t runs = g_inline_runs.fetch_add(1, std::memory_order_relaxed) + 1;
  const int64_t now = NowMs();
  int64_t last = g_last_inline_report_ms.load(std::memory_order_relaxed);
  if (now - last >= 1000 &&
      g_last_inline_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    REXLOG_INFO("gta4-task-pool: visibility scan over {} tasks; {} cell runs scanned in place so far",
                kVisibilityTaskLimit, runs);
  }
}

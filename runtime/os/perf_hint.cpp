// gta4port: ADPF hint session and thermal headroom (see perf_hint.h). The functions come from
// libandroid.so through dlsym: the app's minimum API level (29) predates them, and a phone without
// them just has no session.
#include <stdafx.h>
#include <os/logger.h>
#include "perf_hint.h"
#include <atomic>
#include <chrono>
#include <cmath>

// The frame counter: written by the recorder thread, read and reset by the UI thread.
namespace
{
    std::atomic<uint64_t> g_counterFrames{ 0 };
    std::atomic<int64_t> g_counterWorstNs{ 0 };
}

void FrameCounterRead(float& fps, float& worst_ms)
{
    static auto s_previous = std::chrono::steady_clock::now();
    static uint64_t s_previousFrames = 0;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t frames = g_counterFrames.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(now - s_previous).count();
    fps = seconds > 0 ? float(double(frames - s_previousFrames) / seconds) : 0.0f;
    worst_ms = float(double(g_counterWorstNs.exchange(0, std::memory_order_relaxed)) / 1e6);
    s_previous = now;
    s_previousFrames = frames;
}

static void FrameCounterAdd(int64_t interval_ns)
{
    g_counterFrames.fetch_add(1, std::memory_order_relaxed);
    int64_t worst = g_counterWorstNs.load(std::memory_order_relaxed);
    while (interval_ns > worst &&
        !g_counterWorstNs.compare_exchange_weak(worst, interval_ns, std::memory_order_relaxed))
    {
    }
}
#ifdef __ANDROID__
#include <dlfcn.h>
#endif

#ifdef __ANDROID__
namespace
{
    struct Api
    {
        void* (*getManager)() = nullptr;
        void* (*createSession)(void* manager, const int32_t* tids, size_t count, int64_t targetNs) = nullptr;
        int (*reportActual)(void* session, int64_t actualNs) = nullptr;
        void (*closeSession)(void* session) = nullptr;
        int (*setThreads)(void* session, const int32_t* tids, size_t count) = nullptr; // API 34
        void* (*acquireThermal)() = nullptr;                                            // API 31
        float (*thermalHeadroom)(void* manager, int forecastSeconds) = nullptr;          // API 31
    };

    const Api& GetApi()
    {
        static const Api s_api = []
        {
            Api api;
            void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
            if (!library)
                return api;
            api.getManager = reinterpret_cast<decltype(api.getManager)>(dlsym(library, "APerformanceHint_getManager"));
            api.createSession = reinterpret_cast<decltype(api.createSession)>(dlsym(library, "APerformanceHint_createSession"));
            api.reportActual = reinterpret_cast<decltype(api.reportActual)>(dlsym(library, "APerformanceHint_reportActualWorkDuration"));
            api.closeSession = reinterpret_cast<decltype(api.closeSession)>(dlsym(library, "APerformanceHint_closeSession"));
            api.setThreads = reinterpret_cast<decltype(api.setThreads)>(dlsym(library, "APerformanceHint_setThreads"));
            api.acquireThermal = reinterpret_cast<decltype(api.acquireThermal)>(dlsym(library, "AThermal_acquireManager"));
            api.thermalHeadroom = reinterpret_cast<decltype(api.thermalHeadroom)>(dlsym(library, "AThermal_getThermalHeadroom"));
            return api;
        }();
        return s_api;
    }

    // gta4port: the session gets the recorder's own work per frame (gta4_perf_hint_report_work),
    // so the default target is 28 ms, 85% of the 30 fps period: room for the other stages.
    int64_t TargetNs()
    {
        static const int64_t s_target = []
        {
            const char* value = getenv("GTA4_ADPF_TARGET_MS");
            const double ms = value ? atof(value) : 0.0;
            return int64_t((ms >= 8.0 && ms <= 100.0 ? ms : 28.0) * 1e6);
        }();
        return s_target;
    }

    // Gaps longer than this (loading, a stall) are not frames to plan clocks for.
    constexpr int64_t MAX_REPORTED_NS = 250'000'000;

    std::mutex g_mutex; // the session is used from the recorder and the placement thread
    void* g_session = nullptr;
    bool g_enabled = false;
    bool g_unsupported = false; // createSession failed: not retried every sweep
    std::vector<int32_t> g_tids;
    uint64_t g_reports = 0;
    int64_t g_reportedNs = 0;
    int64_t g_maxNs = 0;
    std::atomic<float> g_thermalCeiling{ 1.0f }; // draw distance ceiling, 1 = none

    // gta4port: levels from the 10 s headroom forecast (1.0 = severe throttling): step down before
    // the governor lowers the clocks, back up 0.07-0.10 lower (hysteresis).
    void UpdateThermalCeiling(float forecast)
    {
        static int s_level = 0;
        if (std::isnan(forecast))
            return;
        if (forecast >= 0.95f)
            s_level = 2;
        else if (forecast >= 0.85f)
            s_level = std::max(s_level, 1);
        if (s_level == 2 && forecast < 0.88f)
            s_level = 1;
        if (s_level == 1 && forecast < 0.75f)
            s_level = 0;
        static constexpr float kCeiling[] = { 1.0f, 0.85f, 0.7f };
        if (g_thermalCeiling.exchange(kCeiling[s_level], std::memory_order_relaxed) != kCeiling[s_level])
            LOGFN("adpf: thermal forecast {:.2f}, draw distance ceiling {:.2f}", forecast, kCeiling[s_level]);
    }

    void CloseSession()
    {
        if (g_session)
            GetApi().closeSession(g_session);
        g_session = nullptr;
    }

    // With g_mutex held.
    void UpdateSession()
    {
        const Api& api = GetApi();
        if (!g_enabled || g_tids.empty())
        {
            CloseSession();
            return;
        }
        if (g_session && api.setThreads && api.setThreads(g_session, g_tids.data(), g_tids.size()) == 0)
            return;
        CloseSession();
        if (g_unsupported || !api.getManager || !api.createSession || !api.reportActual || !api.closeSession)
            return;
        void* manager = api.getManager();
        g_session = manager ? api.createSession(manager, g_tids.data(), g_tids.size(), TargetNs()) : nullptr;
        if (!g_session)
        {
            g_unsupported = true;
            LOGN_WARNING("adpf: the system declined a performance hint session; no session this run");
            return;
        }
        LOGFN("adpf: hint session for {} threads, target {:.1f} ms per frame", g_tids.size(), TargetNs() / 1e6);
    }
}

void PerfHintSetThreads(std::vector<int32_t> tids)
{
    std::sort(tids.begin(), tids.end());
    tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
    std::lock_guard guard{ g_mutex };
    if (tids == g_tids)
        return;
    g_tids = std::move(tids);
    UpdateSession();
}

void PerfHintSetEnabled(bool enabled)
{
    std::lock_guard guard{ g_mutex };
    if (enabled == g_enabled)
        return;
    g_enabled = enabled;
    LOGFN("adpf: hint session {}", enabled ? "on" : "off");
    UpdateSession();
}

bool PerfHintDefaultEnabled()
{
    const char* value = getenv("GTA4_ADPF");
    return !(value && value[0] == '0');
}

void PerfHintTick()
{
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    // The thermal service allows about one headroom query per second. gta4port: the 10 s
    // forecast every 2 s steers the draw distance ceiling (gta4_perf_hint_thermal_ceiling); the
    // 10 s log reuses it.
    const Api& api = GetApi();
    static void* s_thermal = api.acquireThermal ? api.acquireThermal() : nullptr;
    static clock::time_point s_nextThermal = now;
    static float s_forecast = NAN;
    if (now >= s_nextThermal && s_thermal && api.thermalHeadroom)
    {
        s_nextThermal = now + std::chrono::seconds(2);
        s_forecast = api.thermalHeadroom(s_thermal, 10);
        UpdateThermalCeiling(s_forecast);
    }

    static clock::time_point s_next = now + std::chrono::seconds(10);
    if (now < s_next)
        return;
    s_next = now + std::chrono::seconds(10);

    std::lock_guard guard{ g_mutex };
    if (g_session && g_reports != 0)
    {
        LOGFN("adpf: {} frames in 10 s, recorder work mean {:.1f} ms, max {:.1f} ms (target {:.1f}); thermal forecast {:.2f}",
            g_reports, g_reportedNs / 1e6 / double(g_reports), g_maxNs / 1e6, TargetNs() / 1e6, s_forecast);
    }
    else
    {
        LOGFN("adpf: thermal forecast {:.2f} (1.0 = severe throttling)", s_forecast);
    }
    g_reports = 0;
    g_reportedNs = 0;
    g_maxNs = 0;
}

// The time between two recorder starts: the fps counter only (it includes the waits for the game
// and for the 30 fps publication gate, which faster clocks do not shorten).
extern "C" void gta4_perf_hint_report_frame(int64_t interval_ns)
{
    FrameCounterAdd(interval_ns);
}

// gta4port: the recorder's work for one frame, start to vkQueueSubmit (graphics_system.cpp,
// FrameRecorderMain): what the session plans clocks for.
extern "C" void gta4_perf_hint_report_work(int64_t work_ns)
{
    if (work_ns <= 0 || work_ns > MAX_REPORTED_NS)
        return;
    std::lock_guard guard{ g_mutex };
    if (!g_session)
        return;
    GetApi().reportActual(g_session, work_ns);
    g_reports++;
    g_reportedNs += work_ns;
    g_maxNs = std::max(g_maxNs, work_ns);
}

extern "C" float gta4_perf_hint_thermal_ceiling()
{
    return g_thermalCeiling.load(std::memory_order_relaxed);
}
#else
void PerfHintSetThreads(std::vector<int32_t>) {}
void PerfHintSetEnabled(bool) {}
bool PerfHintDefaultEnabled() { return false; }
void PerfHintTick() {}
extern "C" void gta4_perf_hint_report_frame(int64_t interval_ns) { FrameCounterAdd(interval_ns); }
extern "C" void gta4_perf_hint_report_work(int64_t) {}
extern "C" float gta4_perf_hint_thermal_ceiling() { return 1.0f; }
#endif

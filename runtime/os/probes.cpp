// gta4port: call counters on recompiled game functions, for headless debugging. Each probe wraps
// a (weak) recompiled sub_XXXXXXXX, logs its first call and, with GTA4_PROBES=1, prints every
// probe's call count every 2 seconds. The current set covers the handlers of the main loop's
// state machine (sub_82142260: switch on r29, states 0-6).
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <os/logger.h>

namespace
{
    struct Probe
    {
        uint32_t address;
        const char* name;
        std::atomic<uint64_t> calls;
        std::atomic<int64_t> lastCall; // steady_clock ticks
    };

    Probe g_probes[] = {
        { 0x821D17B0, "state 0" },
        { 0x821CE070, "state 1" },
        { 0x821CE1B0, "state 2" },
        { 0x82241C58, "state 3" },
        { 0x821D43C0, "state 4" },
        { 0x821D25A8, "state 5" },
    };

    void Count(size_t index)
    {
        g_probes[index].lastCall = std::chrono::steady_clock::now().time_since_epoch().count();
        if (g_probes[index].calls++ == 0)
            LOGF("first call: {} (0x{:08X})", g_probes[index].name, g_probes[index].address);

        static std::once_flag s_reporter;
        std::call_once(s_reporter, []
        {
            if (getenv("GTA4_PROBES") == nullptr)
                return;

            std::thread([]
            {
                // GTA4_WATCH=82B5E54C,82B5E550: guest words printed with the counts.
                std::vector<uint32_t> watches;
                if (const char* list = getenv("GTA4_WATCH"))
                {
                    for (const char* p = list; *p; )
                    {
                        char* end;
                        watches.push_back(uint32_t(strtoul(p, &end, 16)));
                        p = *end ? end + 1 : end;
                    }
                }

                while (true)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    std::string line;
                    for (const Probe& probe : g_probes)
                        line += fmt::format("{}={} ", probe.name, probe.calls.load());
                    for (uint32_t address : watches)
                        line += fmt::format("[{:08X}]={:08X} ", address, ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + address)));
                    LOG(line);
                }
            }).detach();
        });
    }
}

// Main loop states whose handler ran during the last second, e.g. "3, 4".
std::string GetActiveMainLoopStates()
{
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    const int64_t second = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(1)).count();
    std::string states;
    for (size_t i = 0; i < std::size(g_probes); i++)
    {
        if (g_probes[i].calls != 0 && now - g_probes[i].lastCall < second)
            states += fmt::format("{}{}", states.empty() ? "" : ", ", i);
    }
    return states.empty() ? "-" : states;
}

#define GTA4_PROBE(index, address)                    \
    PPC_FUNC_IMPL(__imp__sub_##address);               \
    PPC_FUNC(sub_##address)                            \
    {                                                  \
        Count(index);                                  \
        __imp__sub_##address(ctx, base);               \
    }

// The handlers are EFLC's (the macro pastes their numbers, which tools/gen_iv_sources.py leaves alone).
#ifndef GTA4_IV
GTA4_PROBE(0, 821D17B0)
GTA4_PROBE(1, 821CE070)
GTA4_PROBE(2, 821CE1B0)
GTA4_PROBE(3, 82241C58)
GTA4_PROBE(4, 821D43C0)
GTA4_PROBE(5, 821D25A8)
#endif

#pragma once
// gta4port: Android's performance hint API (ADPF, API 33+) for the threads that make each frame.
// The app may not raise its threads' uclamp.min itself (EPERM, 2026-10-26); a hint session is the
// supported way: the game reports how long each frame took against a target, and the system (on
// MediaTek, its frame-aware scheduler) sets those threads' clocks to meet it instead of guessing
// from load. On by default since 2026-10-26; files/adpf_mode 'B' or GTA4_ADPF=0 turns it off. Also logs the thermal headroom every 10 seconds (1.0 = the device starts
// severe throttling), so a slow session can be told apart from a hot phone.

#include <cstdint>
#include <vector>

// The threads of the session, from the placement thread at every sweep (only a change does work).
void PerfHintSetThreads(std::vector<int32_t> tids);

// files/adpf_mode, read by the placement thread: 'A' on, 'B' off, absent = GTA4_ADPF (default on).
void PerfHintSetEnabled(bool enabled);
bool PerfHintDefaultEnabled();

// From the placement thread at every sweep: the periodic log line.
void PerfHintTick();

// From the renderer's frame recorder, at the start of each frame: the time since the previous one.
// Also feeds the on-screen frame counter, with or without a session.
extern "C" void gta4_perf_hint_report_frame(int64_t interval_ns);

// The on-screen frame counter (MainActivity, twice a second): frames per second since the previous
// call and the longest frame among them in ms (a stutter shows there before it moves the average).
void FrameCounterRead(float& fps, float& worst_ms);

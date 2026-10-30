// gta4port: one text block describing what the headless game is doing, for the Android status
// screen (android/src/.../MainActivity.java polls it twice a second through JNI).
#include <stdafx.h>
#include <kernel/memory.h>
#include "status.h"
#include "thread_dump.h"

uint32_t GetGpuSwapCount();
std::string GetLastOpenedFile();
std::string GetActiveMainLoopStates();
extern std::atomic<uint64_t> g_fileReads;
extern std::atomic<uint64_t> g_fileBytesRead;

namespace os::status
{
    static const auto g_start = std::chrono::steady_clock::now();

    std::string Describe()
    {
        using namespace std::chrono;

        // Frame rate over the time since the previous call.
        static std::mutex s_mutex;
        static auto s_lastTime = steady_clock::now();
        static uint32_t s_lastSwaps;
        static double s_fps;

        const auto now = steady_clock::now();
        const uint32_t swaps = GetGpuSwapCount();
        {
            std::lock_guard guard{ s_mutex };
            const double elapsed = duration<double>(now - s_lastTime).count();
            if (elapsed >= 0.5)
            {
                s_fps = (swaps - s_lastSwaps) / elapsed;
                s_lastTime = now;
                s_lastSwaps = swaps;
            }
        }

        // Sub-state of the main loop's state 4 (the save scan / episode start), see README.
        const uint32_t subState = g_memory.base ? ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + 0x82B5E54C)) : 0;

        const char* gpu = getenv("GTA4_GPU");
        const bool xenia = gpu && strcmp(gpu, "xenia") == 0;
        std::string text = fmt::format(
            "GTA IV: Episodes from Liberty City — port nativo ARM64\n"
            "{}\n"
            "\n"
            "Tiempo           {} s\n"
            "Fotogramas       {} · {:.0f} fps\n"
            "Hilos del juego  {}\n"
            "Lecturas         {} · {} MB\n"
            "Último fichero   {}\n"
            "Bucle principal  estados {} · subestado {}\n"
            "\n"
            "Log:\n",
            xenia ? "Imagen: GPU de Xenia sobre Vulkan (fase 4). Toca la pantalla para ocultar este texto."
                  : "Sin imagen: GPU falsa (GTA4_GPU=xenia para la imagen)",
            duration_cast<seconds>(now - g_start).count(),
            swaps, s_fps,
            GetGuestThreadCount(),
            g_fileReads.load(), g_fileBytesRead.load() / (1024 * 1024),
            GetLastOpenedFile().empty() ? "-" : GetLastOpenedFile(),
            GetActiveMainLoopStates(), subState);

        for (const std::string& line : os::logger::GetRecentLines(14))
            text += line.size() > 110 ? line.substr(0, 110) + "…\n" : line + "\n";

        return text;
    }
}

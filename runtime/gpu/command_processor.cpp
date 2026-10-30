// gta4port: fake GPU for the headless phases. The game's statically linked D3D writes PM4 packets
// into a ring buffer, kicks the GPU through CP_RB_WPTR and then waits for the GPU's answers:
// the ring read pointer written back to memory, fences written by EVENT_WRITE_SHD / MEM_WRITE,
// scratch registers, and interrupts (source 1 from PM4_INTERRUPT, source 0 at every vblank) that
// make D3D run its swap-completion callbacks. This command processor gives exactly those answers
// and skips everything that would draw. Packet semantics follow Xenia's CommandProcessor (BSD).
//
// Registers live where the CPU reads them: the MMIO window at 0x7FC80000 + index * 4 (big-endian),
// which in this runtime is plain guest memory. Indices of 0x4000 and up (shader constants) are
// not CPU-visible and stay in a host array.
#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <cpu/guest_thread.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <kernel/xdm.h>
#include <os/background.h>
#include <os/logger.h>
#include <os/thread_policy.h>
#include <os/thread_dump.h>
#include <gta4_gpu.h>
#include <gta4_native.h>

std::string VmemPhysicalStats(); // kernel/vmem.cpp

extern std::atomic<uint64_t> g_fileReads;
extern std::atomic<uint64_t> g_fileBytesRead;

struct ANativeWindow;

namespace
{
    void XeniaInterrupt(uint32_t source, uint32_t cpu);

    constexpr uint32_t MMIO_BASE = 0x7FC80000;
    constexpr uint32_t PHYSICAL_BASE = 0xA0000000; // where runtime/kernel/vmem.cpp maps physical RAM
    constexpr uint32_t TITLE_ID = 0x545407F2;      // Episodes from Liberty City (analysis/xexinfo.txt)

    constexpr uint32_t REG_CP_RB_RPTR = 0x01C4;
    constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
    constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
    constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
    constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A2F;
    constexpr uint32_t REG_VGT_EVENT_INITIATOR = 0x21F9;

    enum : uint32_t
    {
        PM4_NOP = 0x10,
        PM4_REG_RMW = 0x21,
        PM4_DRAW_INDX = 0x22,
        PM4_SET_CONSTANT = 0x2D,
        PM4_LOAD_ALU_CONSTANT = 0x2F,
        PM4_DRAW_INDX_2 = 0x36,
        PM4_INDIRECT_BUFFER_PFD = 0x37,
        PM4_WAIT_REG_MEM = 0x3C,
        PM4_MEM_WRITE = 0x3D,
        PM4_REG_TO_MEM = 0x3E,
        PM4_INDIRECT_BUFFER = 0x3F,
        PM4_COND_WRITE = 0x45,
        PM4_EVENT_WRITE = 0x46,
        PM4_ME_INIT = 0x48,
        PM4_INTERRUPT = 0x54,
        PM4_SET_CONSTANT2 = 0x55,
        PM4_SET_SHADER_CONSTANTS = 0x56,
        PM4_EVENT_WRITE_SHD = 0x58,
        PM4_EVENT_WRITE_EXT = 0x59,
        PM4_EVENT_WRITE_ZPD = 0x5B,
        PM4_SET_BIN_MASK = 0x50,
        PM4_SET_BIN_SELECT = 0x51,
        PM4_SET_BIN_MASK_LO = 0x60,
        PM4_SET_BIN_MASK_HI = 0x61,
        PM4_SET_BIN_SELECT_LO = 0x62,
        PM4_SET_BIN_SELECT_HI = 0x63,
    };

    // VdSwap reserves 64 dwords in the ring; it fills them with a NOP whose first word is this.
    constexpr uint32_t SWAP_MARKER = 0x53574150; // 'SWAP'

    uint8_t* Physical(uint32_t address)
    {
        return g_memory.base + PHYSICAL_BASE + (address & 0x1FFFFFFF);
    }

    // The low two bits of a GPU memory address select a byte order (xenos::Endian).
    uint32_t GpuSwap(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF); // 8 in 16
        case 2: return ByteSwap(value);                                           // 8 in 32
        case 3: return (value << 16) | (value >> 16);                             // 16 in 32
        default: return value;
        }
    }

    // value = host order; memory gets it in the order the address asks for.
    void WriteMemory(uint32_t address, uint32_t value)
    {
        *reinterpret_cast<uint32_t*>(Physical(address & ~3u)) = GpuSwap(value, address & 3);
    }

    uint32_t ReadMemory(uint32_t address)
    {
        return GpuSwap(*reinterpret_cast<uint32_t*>(Physical(address & ~3u)), address & 3);
    }

    bool Compare(uint32_t function, uint32_t value, uint32_t reference)
    {
        switch (function & 7)
        {
        case 0: return false;
        case 1: return value < reference;
        case 2: return value <= reference;
        case 3: return value == reference;
        case 4: return value != reference;
        case 5: return value >= reference;
        case 6: return value > reference;
        default: return true;
        }
    }

    // Reads big-endian dwords from a ring (wrapping) or from a linear indirect buffer.
    struct PacketReader
    {
        uint32_t base;      // physical address
        uint32_t size;      // ring size in dwords, 0 for a linear buffer
        uint32_t index;
        uint32_t end;

        bool AtEnd() const
        {
            return index == end;
        }

        uint32_t Read()
        {
            const uint32_t value = ByteSwap(*reinterpret_cast<uint32_t*>(Physical(base + index * 4)));
            index = size ? (index + 1) % size : index + 1;
            return value;
        }

        void Skip(uint32_t count)
        {
            index = size ? (index + count) % size : index + count;
        }
    };

    class CommandProcessor
    {
    public:
        void InitializeRingBuffer(uint32_t address, uint32_t sizeLog2)
        {
            std::lock_guard guard{ m_mutex };
            m_ringBase = address & 0x1FFFFFFF;
            m_ringSize = (1u << (sizeLog2 + 3)) / 4;
            m_readIndex = 0;
            LOGF("ring buffer at 0x{:08X}, {} dwords", m_ringBase, m_ringSize);
            // GTA4_GPU=xenia: Xenia's GPU (xenia_glue/) consumes the ring and draws with Vulkan.
            const char* gpu = getenv("GTA4_GPU");
            if (gpu && strcmp(gpu, "xenia") == 0 && g_memory.physical)
            {
                m_xenia = gta4gpu::Initialize(g_memory.base, g_memory.physical, TITLE_ID,
                    XeniaInterrupt);
                LOGF("GPU: {}", m_xenia ? "Xenia (Vulkan)" : "Xenia failed to start, fake GPU instead");
                if (m_xenia)
                {
                    gta4gpu::InitializeRingBuffer(address, sizeLog2);
                    ApplyPendingSurface();
                }
            }
            // GTA4_GPU=native: LibertyRecomp's native renderer (rexglue_glue/) draws from the
            // game's D3D calls; the fake GPU still consumes the ring (fences, interrupts).
            if (gpu && strcmp(gpu, "native") == 0 && g_memory.physical)
            {
                m_native = gta4native::Initialize(g_memory.base, g_memory.physical);
                LOGF("GPU: {}", m_native ? "native renderer (Vulkan) + fake GPU"
                                         : "native renderer failed to start, fake GPU only");
                if (m_native)
                    ApplyPendingSurface();
            }
            Start();
        }

        void EnableReadPointerWriteBack(uint32_t address, uint32_t blockLog2)
        {
            std::lock_guard guard{ m_mutex };
            m_writeBackAddress = address;
            LOGF("read pointer write-back to 0x{:08X} (block log2 {})", address, blockLog2);
            if (m_xenia)
                gta4gpu::EnableReadPointerWriteBack(address, blockLog2);
        }

        bool UsesXenia() const
        {
            return m_xenia;
        }

        // Interrupts delivered by Xenia's command processor run here, as the fake GPU's do.
        void RunInterrupt(uint32_t source, uint32_t cpu)
        {
            DispatchInterrupt(source, cpu);
        }

        // The activity's surface may come before or after the GPU starts.
        void SetSurface(ANativeWindow* window)
        {
            std::lock_guard guard{ m_surfaceMutex };
            m_surface = window;
            if (m_xenia)
                gta4gpu::SetSurface(window);
            if (m_native)
                gta4native::SetSurface(window);
        }

        void CountSwap()
        {
            ++m_swapCounter;
        }

        void SetInterruptCallback(uint32_t callback, uint32_t userData)
        {
            std::lock_guard guard{ m_interruptMutex };
            m_interruptCallback = callback;
            m_interruptUserData = userData;
            LOGF("interrupt callback 0x{:08X}, data 0x{:08X}", callback, userData);
        }

        uint32_t GetSwapCount() const
        {
            return m_swapCounter;
        }

        void Kick(uint32_t device)
        {
            m_device = device;
            ++m_kicks;
            if (m_xenia)
            {
                gta4gpu::UpdateWritePointer(ReadRegister(REG_CP_RB_WPTR));
                return;
            }
            {
                std::lock_guard guard{ m_kickMutex };
                m_kicked = true;
            }
            m_kickCondition.notify_one();
        }

    private:
        void Start()
        {
            if (m_started)
                return;

            m_started = true;

            // Values Xenia's GraphicsSystem::ReadRegister returns for display registers the CPU polls.
            WriteMmio(0x3C00 / 4, 0x08100748);
            WriteMmio(0x3C04 / 4, 0x0000200E);
            WriteMmio(0x6530 / 4, 0x000002D0); // scanline
            WriteMmio(0x6544 / 4, 0x00000001); // D1MODE_VBLANK_STATUS
            WriteMmio(0x6584 / 4, 0x050002D0); // 1280x720

            if (!m_xenia)
                std::thread([this] { CommandThread(); }).detach();
            std::thread([this] { VblankThread(); }).detach();
        }

        void ApplyPendingSurface()
        {
            std::lock_guard guard{ m_surfaceMutex };
            if (m_surface && m_xenia)
                gta4gpu::SetSurface(m_surface);
            if (m_surface && m_native)
                gta4native::SetSurface(m_surface);
        }

        void WriteMmio(uint32_t index, uint32_t value)
        {
            *reinterpret_cast<uint32_t*>(g_memory.base + MMIO_BASE + index * 4) = ByteSwap(value);
        }

        uint32_t ReadRegister(uint32_t index)
        {
            index &= 0x7FFF;
            if (index < 0x4000)
                return ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + MMIO_BASE + index * 4));
            return m_constants[index - 0x4000];
        }

        void WriteRegister(uint32_t index, uint32_t value)
        {
            index &= 0x7FFF;
            if (index >= 0x4000)
            {
                if (index - 0x4000 < std::size(m_constants))
                    m_constants[index - 0x4000] = value;
                return;
            }

            // Coherency requests complete at once: nothing is cached on this side.
            if (index == REG_COHER_STATUS_HOST)
                value &= ~0x80000000u;

            WriteMmio(index, value);

            // Scratch registers are also written to memory when their bit is set in SCRATCH_UMSK.
            if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
            {
                const uint32_t scratch = index - REG_SCRATCH_REG0;
                if (ReadRegister(REG_SCRATCH_UMSK) & (1u << scratch))
                    *reinterpret_cast<uint32_t*>(Physical((ReadRegister(REG_SCRATCH_ADDR) & ~3u) + scratch * 4)) = ByteSwap(value);
            }
        }

        // Runs the guest interrupt callback on the calling thread, as Xenia's EmulateCPInterruptDPC does.
        void DispatchInterrupt(uint32_t source, uint32_t cpu)
        {
            std::lock_guard guard{ m_interruptMutex };
            if (m_interruptCallback == 0)
                return;

            thread_local std::unique_ptr<GuestThreadContext> s_context;
            if (!s_context)
                s_context = std::make_unique<GuestThreadContext>(cpu);

            PPCContext& ctx = s_context->ppcContext;
            s_context->thread[0x10C] = uint8_t(cpu); // PCR: current processor
            ctx.r3.u64 = source;
            ctx.r4.u64 = m_interruptUserData;
            g_memory.FindFunction(m_interruptCallback)(ctx, g_memory.base);
            ++m_interrupts;
        }

        void CommandThread()
        {
            NameCurrentThread("gta4-gpu");
            while (true)
            {
                {
                    // Kicks come from the hooked D3D kickoff; the timeout is a fallback poll.
                    std::unique_lock lock{ m_kickMutex };
                    m_kickCondition.wait_for(lock, std::chrono::milliseconds(2), [this] { return m_kicked; });
                    m_kicked = false;
                }

                uint32_t ringBase, ringSize;
                {
                    std::lock_guard guard{ m_mutex };
                    ringBase = m_ringBase;
                    ringSize = m_ringSize;
                }
                if (ringSize == 0)
                    continue;

                const uint32_t writeIndex = ReadRegister(REG_CP_RB_WPTR) % ringSize;
                if (writeIndex == m_readIndex)
                    continue;

                PacketReader reader{ ringBase, ringSize, m_readIndex, writeIndex };
                while (!reader.AtEnd())
                    ExecutePacket(reader);

                m_readIndex = writeIndex;
                WriteRegister(REG_CP_RB_RPTR, m_readIndex);
                if (m_writeBackAddress != 0)
                    *reinterpret_cast<uint32_t*>(Physical(m_writeBackAddress)) = ByteSwap(m_readIndex.load());
            }
        }

        void ExecuteIndirectBuffer(uint32_t address, uint32_t length)
        {
            PacketReader reader{ address & 0x1FFFFFFF, 0, 0, length };
            while (!reader.AtEnd())
                ExecutePacket(reader);
        }

        void ExecutePacket(PacketReader& reader)
        {
            const uint32_t packet = reader.Read();
            switch (packet >> 30)
            {
            case 0:
            {
                // Type 0: consecutive (or, with bit 15, repeated) register writes.
                const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
                const uint32_t base = packet & 0x7FFF;
                const bool oneRegister = (packet >> 15) & 1;
                for (uint32_t i = 0; i < count; i++)
                    WriteRegister(oneRegister ? base : base + i, reader.Read());
                break;
            }
            case 1:
            {
                // Type 1: two registers.
                const uint32_t first = reader.Read();
                const uint32_t second = reader.Read();
                WriteRegister(packet & 0x7FF, first);
                WriteRegister((packet >> 11) & 0x7FF, second);
                break;
            }
            case 2:
                break; // Type 2: padding.
            case 3:
                ExecutePacketType3(reader, packet);
                break;
            }
        }

        void ExecutePacketType3(PacketReader& reader, uint32_t packet)
        {
            const uint32_t opcode = (packet >> 8) & 0x7F;
            const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
            const uint32_t start = reader.index;
            uint32_t consumed = 0;
            auto read = [&] { consumed++; return reader.Read(); };

            // Predicated tiling: D3D runs one command buffer once per screen tile, and packets
            // with bit 0 set only run in the passes the bin select/mask let through. Without
            // this, interrupts and swaps run once per tile and D3D's per-CPU list threads replay
            // lists without end.
            if ((packet & 1) && (m_binSelect & m_binMask) == 0)
            {
                reader.Skip(count);
                return;
            }

            switch (opcode)
            {
            case PM4_SET_BIN_MASK_LO:
                m_binMask = (m_binMask & 0xFFFFFFFF00000000ull) | read();
                break;
            case PM4_SET_BIN_MASK_HI:
                m_binMask = (m_binMask & 0xFFFFFFFFull) | (uint64_t(read()) << 32);
                break;
            case PM4_SET_BIN_SELECT_LO:
                m_binSelect = (m_binSelect & 0xFFFFFFFF00000000ull) | read();
                break;
            case PM4_SET_BIN_SELECT_HI:
                m_binSelect = (m_binSelect & 0xFFFFFFFFull) | (uint64_t(read()) << 32);
                break;
            case PM4_SET_BIN_MASK:
            {
                const uint64_t high = read();
                m_binMask = (high << 32) | read();
                break;
            }
            case PM4_SET_BIN_SELECT:
            {
                const uint64_t high = read();
                m_binSelect = (high << 32) | read();
                break;
            }

            case PM4_NOP:
                if (count >= 1 && read() == SWAP_MARKER)
                    OnSwap();
                break;

            case PM4_INTERRUPT:
            {
                const uint32_t cpuMask = read();
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                {
                    if (cpuMask & (1u << cpu))
                        DispatchInterrupt(1, cpu);
                }
                break;
            }

            case PM4_INDIRECT_BUFFER:
            case PM4_INDIRECT_BUFFER_PFD:
            {
                const uint32_t address = read();
                const uint32_t length = read() & 0xFFFFF;
                ExecuteIndirectBuffer(address, length);
                break;
            }

            case PM4_WAIT_REG_MEM:
            {
                const uint32_t waitInfo = read();
                const uint32_t pollAddress = read();
                const uint32_t reference = read();
                const uint32_t mask = read();
                const uint32_t wait = read();
                WaitRegisterMemory(waitInfo, pollAddress, reference, mask, wait);
                break;
            }

            case PM4_REG_RMW:
            {
                const uint32_t info = read();
                const uint32_t andMask = read();
                const uint32_t orMask = read();
                uint32_t value = ReadRegister(info & 0x1FFF);
                value &= (info >> 31) & 1 ? ReadRegister(andMask & 0x1FFF) : andMask;
                value |= (info >> 30) & 1 ? ReadRegister(orMask & 0x1FFF) : orMask;
                WriteRegister(info & 0x1FFF, value);
                break;
            }

            case PM4_REG_TO_MEM:
            {
                const uint32_t index = read();
                const uint32_t address = read();
                WriteMemory(address, ReadRegister(index));
                break;
            }

            case PM4_MEM_WRITE:
            {
                uint32_t address = read();
                while (consumed < count)
                {
                    WriteMemory(address, read());
                    address += 4;
                }
                break;
            }

            case PM4_COND_WRITE:
            {
                const uint32_t waitInfo = read();
                const uint32_t pollAddress = read();
                const uint32_t reference = read();
                const uint32_t mask = read();
                const uint32_t writeAddress = read();
                const uint32_t data = read();
                const uint32_t value = (waitInfo & 0x10) ? ReadMemory(pollAddress) : ReadRegister(pollAddress);
                if (Compare(waitInfo, value & mask, reference))
                {
                    if (waitInfo & 0x100)
                        WriteMemory(writeAddress, data);
                    else
                        WriteRegister(writeAddress, data);
                }
                break;
            }

            case PM4_EVENT_WRITE_SHD:
            {
                const uint32_t initiator = read();
                const uint32_t address = read();
                const uint32_t value = read();
                WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
                // Bit 31: write the swap counter instead of the value.
                WriteMemory(address, (initiator >> 31) & 1 ? m_swapCounter.load() : value);
                break;
            }

            case PM4_EVENT_WRITE_EXT:
            {
                // Screen extents of what was drawn; report the whole 1280x720 target.
                const uint32_t initiator = read();
                const uint32_t address = read();
                WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
                const uint16_t extents[] = { 0, 1280 >> 3, 0, 720 >> 3, 0, 1 };
                auto* out = reinterpret_cast<uint16_t*>(Physical(address & ~3u));
                for (size_t i = 0; i < std::size(extents); i++)
                    out[i] = (address & 3) ? ByteSwap(extents[i]) : extents[i];
                break;
            }

            case PM4_SET_CONSTANT:
            {
                const uint32_t offsetType = read();
                uint32_t index = offsetType & 0x7FF;
                static constexpr uint32_t s_bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                if (type >= std::size(s_bases))
                    break;
                index += s_bases[type];
                while (consumed < count)
                    WriteRegister(index++, read());
                break;
            }

            case PM4_SET_CONSTANT2:
            case PM4_SET_SHADER_CONSTANTS:
            {
                uint32_t index = read() & 0xFFFF;
                while (consumed < count)
                    WriteRegister(index++, read());
                break;
            }

            case PM4_LOAD_ALU_CONSTANT:
            {
                const uint32_t address = read() & 0x3FFFFFFF;
                const uint32_t offsetType = read();
                const uint32_t size = read() & 0xFFF;
                static constexpr uint32_t s_bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                if (type >= std::size(s_bases))
                    break;
                const uint32_t index = (offsetType & 0x7FF) + s_bases[type];
                for (uint32_t i = 0; i < size; i++)
                    WriteRegister(index + i, ByteSwap(*reinterpret_cast<uint32_t*>(Physical(address + i * 4))));
                break;
            }

            case PM4_ME_INIT:
            case PM4_EVENT_WRITE:
            case PM4_EVENT_WRITE_ZPD:
            case PM4_DRAW_INDX:
            case PM4_DRAW_INDX_2:
                break; // nothing the CPU waits on

            default:
                if (!m_reportedOpcodes[opcode])
                {
                    m_reportedOpcodes[opcode] = true;
                    LOGF("skipping PM4 opcode 0x{:02X} ({} dwords)", opcode, count);
                }
                break;
            }

            // Every packet ends where its header says, whatever the handler consumed.
            reader.index = start;
            reader.Skip(count);
        }

        void WaitRegisterMemory(uint32_t waitInfo, uint32_t pollAddress, uint32_t reference, uint32_t mask, uint32_t wait)
        {
            const auto begin = std::chrono::steady_clock::now();
            bool reported = false;
            while (true)
            {
                const uint32_t value = (waitInfo & 0x10) ? ReadMemory(pollAddress) : ReadRegister(pollAddress);
                if (Compare(waitInfo, value & mask, reference))
                    return;

                if (!reported && std::chrono::steady_clock::now() - begin > std::chrono::seconds(2))
                {
                    reported = true;
                    LOGF_WARNING("GPU stuck in WAIT_REG_MEM: {} 0x{:08X} = 0x{:08X}, function {}, reference 0x{:08X}, mask 0x{:08X}",
                        (waitInfo & 0x10) ? "memory" : "register", pollAddress, value, waitInfo & 7, reference, mask);
                }

                if (wait >= 0x100)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                else
                    std::this_thread::yield();
            }
        }

        void OnSwap()
        {
            ++m_swapCounter;
        }

        void VblankThread()
        {
            NameCurrentThread("gta4-vblank");
            using namespace std::chrono;
            constexpr auto period = duration_cast<steady_clock::duration>(duration<double>(1.0 / 60.0));
            auto next = steady_clock::now() + period;
            auto lastReport = steady_clock::now();
            uint32_t lastSwaps = 0;

            while (true)
            {
                // No interrupts while the app is in the background (os/background.h); the
                // 60 Hz timing starts over afterwards instead of catching up.
                if (WaitWhileBackgroundHold("vblank"))
                    next = steady_clock::now() + period;
                std::this_thread::sleep_until(next);
                next += period;
                if (m_xenia)
                    gta4gpu::MarkVblank();
                else
                    DispatchInterrupt(0, 2);

                const auto now = steady_clock::now();
                if (now - lastReport >= seconds(2))
                {
                    const uint32_t swaps = m_swapCounter;
                    const double elapsed = duration<double>(now - lastReport).count();
                    LOGF("{} swaps ({:.1f} fps), {} interrupts, ring read {} / write {}, file reads {} ({} KB)",
                        swaps, (swaps - lastSwaps) / elapsed, m_interrupts.load(), m_readIndex.load(), ReadRegister(REG_CP_RB_WPTR),
                        g_fileReads.load(), g_fileBytesRead.load() / 1024);
                    if (const uint32_t device = m_device)
                    {
                        // D3D device fields (XDK 6274 layout, see README): fence the CPU has
                        // inserted (+0x2A9C) and the one the GPU wrote (*+0x2A90); swaps
                        // presented (+0x40A0) and completed (+0x40A8).
                        auto word = [](uint32_t address) { return ByteSwap(*reinterpret_cast<uint32_t*>(g_memory.base + address)); };
                        LOGF("d3d: {} kicks, fence cpu 0x{:X} gpu 0x{:X}, swaps presented {} completed {}",
                            m_kicks.load(), word(device + 0x2A9C), word(word(device + 0x2A90)), word(device + 0x40A0),
                            word(device + 0x40A8));
                    }
                    if (gta4gpu::IsActive())
                    {
                        LOGF("{}", gta4gpu::DebugStatus());
                    }
                    LOGF("{}", VmemPhysicalStats());
                    lastReport = now;
                    lastSwaps = swaps;
                }
            }
        }

        std::mutex m_mutex;
        uint32_t m_ringBase = 0;
        uint32_t m_ringSize = 0;
        uint32_t m_writeBackAddress = 0;
        std::atomic<uint32_t> m_readIndex = 0;
        bool m_started = false;
        bool m_xenia = false; // GTA4_GPU=xenia and Xenia started
        bool m_native = false; // GTA4_GPU=native and the native renderer started
        std::mutex m_surfaceMutex;
        ANativeWindow* m_surface = nullptr;

        std::mutex m_kickMutex;
        std::condition_variable m_kickCondition;
        bool m_kicked = false;
        std::atomic<uint32_t> m_device = 0;
        std::atomic<uint64_t> m_kicks = 0;

        std::recursive_mutex m_interruptMutex;
        uint32_t m_interruptCallback = 0;
        uint32_t m_interruptUserData = 0;
        std::atomic<uint64_t> m_interrupts = 0;

        std::atomic<uint32_t> m_swapCounter = 0;
        uint32_t m_constants[0x2000]{};
        bool m_reportedOpcodes[0x80]{};
        uint64_t m_binMask = ~0ull;   // command thread only
        uint64_t m_binSelect = ~0ull;
    };

    CommandProcessor g_commandProcessor;

    void XeniaInterrupt(uint32_t source, uint32_t cpu)
    {
        g_commandProcessor.RunInterrupt(source, cpu);
    }
}

// Frames the game has presented so far (shown by the Android status screen).
// The activity's SurfaceView (runtime/os/android_app.cpp), nullptr when it goes away.
void SetGpuSurface(ANativeWindow* window)
{
    g_commandProcessor.SetSurface(window);
}

uint32_t GetGpuSwapCount()
{
    return g_commandProcessor.GetSwapCount();
}

void VdInitializeRingBuffer(uint32_t address, uint32_t sizeLog2)
{
    g_commandProcessor.InitializeRingBuffer(address, sizeLog2);
}

void VdEnableRingBufferRPtrWriteBack(uint32_t address, uint32_t blockLog2)
{
    g_commandProcessor.EnableReadPointerWriteBack(address, blockLog2);
}

void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t userData)
{
    g_commandProcessor.SetInterruptCallback(callback, userData);
}

// D3D reserves 64 dwords in the ring for the kernel's swap commands. A NOP carrying the swap
// marker and the front buffer address keeps the command processor in step.
void VdSwap(be<uint32_t>* buffer, uint32_t fetch, uint32_t unk2, uint32_t unk3, uint32_t unk4, be<uint32_t>* frontBuffer)
{
    if (g_commandProcessor.UsesXenia())
    {
        gta4gpu::WriteSwapPacket(g_memory.MapVirtual(buffer), fetch);
        g_commandProcessor.CountSwap();
        return;
    }
    buffer[0] = 0xC0000000 | (62u << 16) | (PM4_NOP << 8);
    buffer[1] = SWAP_MARKER;
    buffer[2] = frontBuffer ? uint32_t(*frontBuffer) & 0x1FFFFFFF : 0;
    for (uint32_t i = 3; i < 64; i++)
        buffer[i] = 0;
}

void VdGetSystemCommandBuffer(be<uint32_t>* buffer, be<uint32_t>* value)
{
    memset(buffer, 0, 0x94);
    buffer[0] = 0xBEEF0000;
    *value = 0xBEEF0001;
}

uint32_t VdInitializeEngines()
{
    return 1;
}

uint32_t VdIsHSIOTrainingSucceeded()
{
    return 1;
}

// Widescreen, at least 1024 pixels wide (Xenia's flags for 1280x720).
uint32_t VdQueryVideoFlags()
{
    return 3;
}

void VdGetCurrentDisplayGamma(be<uint32_t>* type, be<float>* power)
{
    *type = 2;
    *power = 2.22222233f;
}

GUEST_FUNCTION_HOOK(__imp__VdInitializeRingBuffer, VdInitializeRingBuffer);
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack);
GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback);
GUEST_FUNCTION_HOOK(__imp__VdSwap, VdSwap);
GUEST_FUNCTION_HOOK(__imp__VdGetSystemCommandBuffer, VdGetSystemCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdInitializeEngines, VdInitializeEngines);
GUEST_FUNCTION_HOOK(__imp__VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded);
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoFlags, VdQueryVideoFlags);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma);

// D3D's kickoff (writes CP_RB_WPTR): run the original, then wake the command processor at once
// instead of waiting for its fallback poll. The recompiled function is weak, so this replaces it.
PPC_FUNC_IMPL(__imp__sub_82A48868);
PPC_FUNC(sub_82A48868)
{
    const uint32_t device = ctx.r3.u32;
    __imp__sub_82A48868(ctx, base);
    g_commandProcessor.Kick(device);
}

// Diagnostics: D3D's GPU hang handler (called when a fence wait sees no progress for 5000 units
// of the clock at [[r13+0x100]+0x58]).
PPC_FUNC_IMPL(__imp__sub_82A4FC60);
PPC_FUNC(sub_82A4FC60)
{
    auto word = [base](uint32_t address) { return ByteSwap(*reinterpret_cast<uint32_t*>(base + address)); };
    const uint32_t device = ctx.r3.u32;
    const uint32_t thread = word(ctx.r13.u32 + 0x100);
    DiagPrint("D3D GPU hang handler: thread %08X, clock [+0x58] = 0x%08X, fence cpu 0x%X gpu 0x%X\n",
        GuestThread::GetCurrentThreadId(), word(thread + 0x58), word(device + 0x2A9C), word(word(device + 0x2A90)));
    PrintGuestBacktrace(&ctx);
    __imp__sub_82A4FC60(ctx, base);
}

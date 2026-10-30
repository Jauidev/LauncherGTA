#pragma once
// gta4port: the runtime's view of Xenia's XMA decoder contexts (xenia_glue/apu.cc). Plain types
// only, like gta4_gpu.h. The runtime owns the hardware context table (runtime/apu/xma.cpp) and
// forwards the game's XMA register writes (runtime/apu/mmio.cpp); the contexts decode the input
// packets into the output buffers in guest memory, with FFmpeg (GTA4_XMA=new, the default) or
// without (GTA4_XMA=fake: buffers are consumed and the output is silence).
#include <cstdint>

namespace gta4apu {

// guestBase: the 4 GB guest space; physicalBase: the 512 MB of physical memory. contextTable is
// the guest virtual address of count 64-byte XMA_CONTEXT_DATA entries.
bool Initialize(uint8_t* guestBase, uint8_t* physicalBase, uint32_t contextTable,
                uint32_t count);

void AllocateContext(uint32_t index);
void ReleaseContext(uint32_t index);

// An XMA register write, value as the CPU meant it (the registers are little-endian; the game
// writes them with stwbrx). registerIndex is (address & 0xFFFF) / 4.
void WriteRegister(uint32_t registerIndex, uint32_t value);

}  // namespace gta4apu

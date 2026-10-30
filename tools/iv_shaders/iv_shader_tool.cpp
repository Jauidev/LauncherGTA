// Host tool for tools/iv_shaders/compare.py: what the game's shader cache holds against what a
// dumped container compiles to (GTA4_SHADER_DUMP in gta4_native_hooks.cpp).
//   iv_shader_tool hlsl <shader_common.h> <container> <out.hlsl>   Xenos container -> HLSL
//   iv_shader_tool cache <compressed blob> <list> <out dir>        cached SMOL-V -> <hash>.spv
//     (list: one "hash offset size" a line)
#include "pch.h"
#include "shader_recompiler.cpp"  // not its own unit: it relies on pch.h coming first
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

static std::vector<uint8_t> ReadAll(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

int main(int argc, char** argv) {
  if (argc == 5 && !strcmp(argv[1], "hlsl")) {
    const std::vector<uint8_t> include = ReadAll(argv[2]);
    std::vector<uint8_t> container = ReadAll(argv[3]);
    ShaderRecompiler recompiler;
    recompiler.recompile(container.data(),
                         std::string_view(reinterpret_cast<const char*>(include.data()), include.size()));
    std::ofstream(argv[4], std::ios::binary) << recompiler.out;
    printf("%s %u\n", recompiler.isPixelShader ? "pixel" : "vertex", recompiler.specConstantsMask);
    return 0;
  }
  if (argc == 5 && !strcmp(argv[1], "cache")) {
    const std::vector<uint8_t> blob = ReadAll(argv[2]);
    std::vector<uint8_t> data(ZSTD_getFrameContentSize(blob.data(), blob.size()));
    const size_t size = ZSTD_decompress(data.data(), data.size(), blob.data(), blob.size());
    if (ZSTD_isError(size)) {
      fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(size));
      return 1;
    }
    std::ifstream list(argv[3]);
    std::string hash;
    size_t offset, smolv_size;
    while (list >> hash >> offset >> smolv_size) {
      std::vector<uint8_t> words(smolv::GetDecodedBufferSize(data.data() + offset, smolv_size));
      if (!smolv::Decode(data.data() + offset, smolv_size, words.data(), words.size())) {
        fprintf(stderr, "smolv: %s\n", hash.c_str());
        return 1;
      }
      std::ofstream(std::string(argv[4]) + "/" + hash + ".spv", std::ios::binary)
          .write(reinterpret_cast<const char*>(words.data()), words.size());
    }
    return 0;
  }
  fprintf(stderr, "usage: see the top of iv_shader_tool.cpp\n");
  return 2;
}

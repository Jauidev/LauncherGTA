#pragma once

// Loads default.xex into guest memory and binds its imports. Returns the entry point (0 on failure).
uint32_t LdrLoadModule(const std::filesystem::path& path);

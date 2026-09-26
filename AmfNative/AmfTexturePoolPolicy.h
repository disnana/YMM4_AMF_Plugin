#pragma once
#include <algorithm>
#include <cstdint>

// Bounds the added BGRA/RGBA payload, not total VRAM (driver/encoder allocations
// and texture alignment are additional). Preserve the existing 4..8 range.
namespace AmfTexturePoolPolicy
{
    constexpr int Minimum = 4, LegacyMaximum = 8, Maximum = 128;
    constexpr uint64_t ExpandedPayloadLimit = 1024ull * 1024 * 1024;
    inline int Resolve(int requested, int width, int height)
    {
        requested = std::clamp(requested, Minimum, Maximum);
        if (requested <= LegacyMaximum) return requested;
        if (width <= 0 || height <= 0) return LegacyMaximum;
        const uint64_t frameBytes = static_cast<uint64_t>(width) * height * 4;
        const auto fitting = static_cast<int>(std::min<uint64_t>(Maximum, ExpandedPayloadLimit / frameBytes));
        return std::min(requested, std::max(LegacyMaximum, fitting));
    }
}

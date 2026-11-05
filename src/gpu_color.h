#ifndef GPU_COLOR_H
#define GPU_COLOR_H

#include <cstdint>

// NDS/GBA framebuffer color conversion helpers.
//
// The renderer stores NDS output as three adjacent 6-bit channels. The PS2
// presentation path converts 98,304 pixels per full DS frame, so keep the
// channel expansion tables pre-shifted: each pixel needs three small cached
// loads and ORs instead of three extra result shifts.
//
// gpuExpand6() is exactly floor(component * 255 / 63), matching the original
// conversion for every 6-bit input value.
constexpr uint32_t gpuExpand6(uint32_t component)
{
    return component * 255u / 63u;
}

#define GPU_RGB6_LEVELS(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7)  \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15) \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) \
    X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) \
    X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) \
    X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) \
    X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63)

#define GPU_RGB6_R_ENTRY(i) (gpuExpand6(i) << 0),
#define GPU_RGB6_G_ENTRY(i) (gpuExpand6(i) << 8),
#define GPU_RGB6_B_ENTRY(i) (gpuExpand6(i) << 16),

static const uint32_t gpuRgb6R[64] = { GPU_RGB6_LEVELS(GPU_RGB6_R_ENTRY) };
static const uint32_t gpuRgb6G[64] = { GPU_RGB6_LEVELS(GPU_RGB6_G_ENTRY) };
static const uint32_t gpuRgb6B[64] = { GPU_RGB6_LEVELS(GPU_RGB6_B_ENTRY) };

#undef GPU_RGB6_R_ENTRY
#undef GPU_RGB6_G_ENTRY
#undef GPU_RGB6_B_ENTRY
#undef GPU_RGB6_LEVELS

inline uint32_t gpuRgb6ToRgb8(uint32_t color)
{
    return 0xFF000000u |
        gpuRgb6R[(color >> 0) & 0x3Fu] |
        gpuRgb6G[(color >> 6) & 0x3Fu] |
        gpuRgb6B[(color >> 12) & 0x3Fu];
}

inline uint32_t gpuRgb5ToRgb8(uint32_t color)
{
    return 0xFF000000u |
        gpuRgb6R[((color >> 0) & 0x1Fu) << 1] |
        gpuRgb6G[((color >> 5) & 0x1Fu) << 1] |
        gpuRgb6B[((color >> 10) & 0x1Fu) << 1];
}

// The GS CT16 texture format has the same RGB555 channel order as the DS.
// Drop only the low bit of each 6-bit NDS channel and set the GS alpha bit.
// This avoids three table lookups and halves the EE -> GS texture transfer.
inline uint16_t gpuRgb6ToGsCt16(uint32_t color)
{
    return static_cast<uint16_t>(0x8000u |
        ((color >> 1) & 0x001Fu) |
        ((color >> 2) & 0x03E0u) |
        ((color >> 3) & 0x7C00u));
}

inline uint16_t gpuRgb5ToGsCt16(uint32_t color)
{
    return static_cast<uint16_t>(0x8000u | (color & 0x7FFFu));
}

#endif // GPU_COLOR_H

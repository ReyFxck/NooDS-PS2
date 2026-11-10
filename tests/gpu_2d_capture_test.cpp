#include <cassert>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <initializer_list>
#include <string>
#include <unistd.h>

#include "../src/core.h"
#include "../src/settings.h"

// Synthetic zero-filled fixtures are only needed to construct the core; no
// copyrighted BIOS/firmware is executed by these scanline tests.
static std::string fixture(int size)
{
    char path[] = "/tmp/noods-gpu-capture-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    assert(close(fd) == 0);
    return path;
}

int main()
{
    const std::string bios9 = fixture(0x1000);
    const std::string bios7 = fixture(0x4000);
    const std::string firmware = fixture(0x40000);
    Settings::setBios9Path(bios9);
    Settings::setBios7Path(bios7);
    Settings::setFirmwarePath(firmware);
    Settings::setThreaded2D(0);
    Settings::setThreaded3D(0);
    std::unique_ptr<Core> core(new Core());
    unlink(bios9.c_str()); unlink(bios7.c_str()); unlink(firmware.c_str());

    Gpu2D &gpu = core->gpu2D[0];
    uint8_t *palette = core->memory.getPalette();
    palette[0] = 0x1F; palette[1] = 0; // red backdrop, RGB5

    for (unsigned mode = 0; mode < 4; ++mode)
    {
        for (unsigned blended = 0; blended < 2; ++blended)
        {
            gpu.writeDispCnt(0xFFFFFFFFu, mode << 16);
            gpu.writeBldCnt(0xFFFFu, blended ? (0x20u | (3u << 6)) : 0u);
            gpu.writeBldY(8);
            gpu.writeMasterBright(0xFFFFu, 0x8000u | 16u); // screen black
            gpu.drawScanline(0);
            const uint32_t expected = blended ? 31u : 62u;
            for (int x = 0; x < 256; ++x)
                assert((gpu.getRawLine()[x] & 0x3FFFFu) == expected);
            // Master brightness applies to display, not capture source.
            if (mode != 3) // mode 3 FIFO is unimplemented
                assert(gpu.getFramebuffer()[0] == 0u);
        }
    }

    // A text tile row with every index zero must remain transparent for
    // both bpp modes. A single nonzero index verifies palette, clipping and
    // horizontal flip around the same early-exit path.
    core->memory.write<uint8_t>(0, 0x4000240u, 0x81); // VRAM A -> engine A BG
    palette[2] = 0xE0; palette[3] = 0x03; // palette index 1: green
    gpu.writeBldCnt(0xFFFFu, 0);
    gpu.writeMasterBright(0xFFFFu, 0);
    for (int bpp8 = 0; bpp8 < 2; ++bpp8)
    for (int flip = 0; flip < 2; ++flip)
    for (int fine : {0, 7})
    for (int nonzero = 0; nonzero < 2; ++nonzero)
    {
        gpu.writeDispCnt(0xFFFFFFFFu, (1u << 16) | (1u << 8));
        gpu.writeBgCnt(0, 0xFFFFu, 4u | (bpp8 ? 0x80u : 0u));
        gpu.writeBgHOfs(0, 0xFFFFu, fine);
        for (int tile = 0; tile < 32; ++tile)
            core->memory.write<uint16_t>(0, 0x6000000u + tile * 2,
                flip ? (1u << 10) : 0u);
        core->memory.write<uint32_t>(0, 0x6004000u,
            nonzero ? (bpp8 ? 0x01000000u : 0x00001000u) : 0u);
        if (bpp8)
            core->memory.write<uint32_t>(0, 0x6004004u, 0u);
        gpu.drawScanline(0);
        for (int x = 0; x < 256; ++x)
        {
            bool green = nonzero && ((x + fine) & 7) == (flip ? 4 : 3);
            const uint32_t expected = green ? (62u << 6) : 62u;
            assert((gpu.getFramebuffer()[x] & 0x3FFFFu) == expected);
            assert((gpu.getRawLine()[x] & 0x3FFFFu) == expected);
        }
    }

    // Three opaque backgrounds overlap. BG1 is hidden by the other two on
    // the right, but contributes to alpha blending where window 0 hides BG3.
    palette[2] = 0x1F; palette[3] = 0;       // red
    palette[4] = 0xE0; palette[5] = 0x03;   // green
    palette[6] = 0x00; palette[7] = 0x7C;   // blue
    for (int bg = 1; bg <= 3; ++bg)
    {
        const int screenBase = 4 + bg;
        const int charBase = 4 - bg;
        const int priority = (bg == 3) ? 0 : ((bg == 2) ? 1 : 3);
        gpu.writeBgCnt(bg, 0xFFFFu, (screenBase << 8) | (charBase << 2) | priority);
        gpu.writeBgHOfs(bg, 0xFFFFu, 0);
        gpu.writeBgVOfs(bg, 0xFFFFu, 0);
        for (int tile = 0; tile < 32; ++tile)
            core->memory.write<uint16_t>(0, 0x6000000u + screenBase * 0x800u + tile * 2, 0);
        const uint32_t index = static_cast<uint32_t>(4 - bg);
        core->memory.write<uint32_t>(0, 0x6000000u + charBase * 0x4000u, index * 0x11111111u);
    }
    gpu.writeDispCnt(0xFFFFFFFFu, (1u << 16) | (1u << 13) | (0xEu << 8));
    gpu.writeWinH(0, 0xFFFFu, 0x0080u);
    gpu.writeWinV(0, 0xFFFFu, 0x00C0u);
    gpu.writeWinIn(0xFFFFu, 0x37u);  // hide BG3 inside window 0
    gpu.writeWinOut(0xFFFFu, 0x3Fu);
    gpu.writeBldCnt(0xFFFFu, (1u << 3) | (1u << 2) | (1u << 6) | (1u << 9) | (1u << 10));
    gpu.writeBldAlpha(0xFFFFu, 8u | (8u << 8));
    gpu.drawScanline(0);
    for (int x = 0; x < 256; ++x)
    {
        const uint32_t expected = x < 128 ? ((31u << 6) | (31u << 12)) : (31u | (31u << 6));
        assert((gpu.getFramebuffer()[x] & 0x3FFFFu) == expected);
        assert((gpu.getRawLine()[x] & 0x3FFFFu) == expected);
    }
    // Independent per-pixel oracle: enumerate visible surfaces, sort by the
    // original priority/tie contract, then blend the first two. Cover every
    // combination of four BG priorities, both texel sizes, transparent holes,
    // tile flips, fine scroll, windows, and raw capture before brightness.
    const uint16_t colors[4] = {0x001F, 0x03E0, 0x7C00, 0x7FFF};
    for (int bg = 0; bg < 4; bg++)
    {
        palette[2 * (bg + 1)] = colors[bg];
        palette[2 * (bg + 1) + 1] = colors[bg] >> 8;
    }
    palette[0] = palette[1] = 0;
    for (int bpp8 = 0; bpp8 < 2; bpp8++)
    for (int flip = 0; flip < 2; flip++)
    {
        for (int bg = 0; bg < 4; bg++)
        {
            for (int tile = 0; tile < 32; tile++)
                core->memory.write<uint16_t>(0, 0x6010000u + bg * 0x800u + tile * 2,
                    flip ? (1u << 10) : 0u);
            // BG0 has no holes; other BGs expose lower surfaces at
            // different pixels. Pattern repeats in every tile.
            for (int x = 0; x < 8; x++)
            {
                const unsigned index = (bg == 0 || ((x + bg) % 3)) ? bg + 1 : 0;
                if (bpp8)
                    core->memory.write<uint8_t>(0, 0x6000000u + bg * 0x4000u + x, index);
                else if (!(x & 1))
                {
                    const unsigned next = (bg == 0 || ((x + 1 + bg) % 3)) ? bg + 1 : 0;
                    core->memory.write<uint8_t>(0, 0x6000000u + bg * 0x4000u + x / 2, index | (next << 4));
                }
            }
        }
        for (unsigned priorities = 0; priorities < 256; priorities++)
        for (int alpha = 0; alpha < 2; alpha++)
        {
            const int fine = priorities & 7;
            gpu.writeDispCnt(0xFFFFFFFFu, (1u << 27) | (1u << 16) | (1u << 13) | (0xFu << 8));
            gpu.writeWinIn(0xFFFFu, 0x35u); // BG1/BG3 hidden on left
            gpu.writeWinOut(0xFFFFu, 0x3Fu);
            gpu.writeMasterBright(0xFFFFu, 0x8010u); // display black; raw unchanged
            gpu.writeBldCnt(0xFFFFu, alpha ? 0x3F7Fu : 0u);
            gpu.writeBldAlpha(0xFFFFu, 0x0808u);
            for (int bg = 0; bg < 4; bg++)
            {
                gpu.writeBgCnt(bg, 0xFFFFu, (bg << 8) | (bg << 2) |
                    (bpp8 ? 0x80u : 0u) | ((priorities >> (bg * 2)) & 3));
                gpu.writeBgHOfs(bg, 0xFFFFu, fine);
                gpu.writeBgVOfs(bg, 0xFFFFu, 0);
            }
            gpu.drawScanline(0);
            for (int x = 0; x < 256; x++)
            {
                std::vector<int> surfaces;
                const int texX = flip ? 7 - ((x + fine) & 7) : ((x + fine) & 7);
                for (int bg = 0; bg < 4; bg++)
                    if ((x >= 128 || !(bg & 1)) &&
                        (bg == 0 || ((texX + bg) % 3))) surfaces.push_back(bg);
                std::sort(surfaces.begin(), surfaces.end(), [&](int a, int b)
                {
                    const unsigned pa = (priorities >> (a * 2)) & 3;
                    const unsigned pb = (priorities >> (b * 2)) & 3;
                    return pa != pb ? pa < pb : a < b;
                });
                uint32_t expected = 0;
                for (int channel = 0; channel < 3; channel++)
                {
                    const unsigned first = surfaces.empty() ? 0 :
                        ((colors[surfaces[0]] >> (channel * 5)) & 31) * 2;
                    const unsigned second = surfaces.size() < 2 ? 0 :
                        ((colors[surfaces[1]] >> (channel * 5)) & 31) * 2;
                    expected |= (alpha ? (first + second) / 2 : first) << (channel * 6);
                }
                assert((gpu.getRawLine()[x] & 0x3FFFFu) == expected);
                assert(gpu.getFramebuffer()[x] == 0);
            }
        }
    }
    // Overlapping OBJ: independently choose the nontransparent pixel with
    // lowest priority, then lowest OAM index. Include semitransparent sprites
    // and an object-window pass. No BG is enabled in this scene.
    core->memory.write<uint8_t>(0, 0x4000241u, 0x82); // VRAM B -> A OBJ
    for (int color = 0; color < 3; color++)
    {
        palette[0x202 + color * 2] = colors[color];
        palette[0x203 + color * 2] = colors[color] >> 8;
        uint32_t row = 0;
        for (int x = 0; x < 8; x++)
            if (color == 0 || ((x + color) % 3)) row |= (color + 1) << (x * 4);
        core->memory.write<uint32_t>(0, 0x6400000u + color * 32, row);
    }
    uint8_t *oam = core->memory.getOam();
    for (int rotation = 0; rotation < 4; rotation++)
    for (int window = 0; window < 2; window++)
    {
        for (int i = 0; i < 128; i++)
        {
            const int type = (window && i == 127) ? 2 : (i & 1);
            const uint16_t attr0 = type << 10;
            const uint16_t attr2 = (((i + rotation) & 3) << 10) | (i % 3);
            oam[i * 8] = attr0; oam[i * 8 + 1] = attr0 >> 8;
            oam[i * 8 + 2] = oam[i * 8 + 3] = 0;
            oam[i * 8 + 4] = attr2; oam[i * 8 + 5] = attr2 >> 8;
        }
        gpu.writeDispCnt(0xFFFFFFFFu, (1u << 16) | (1u << 12) |
            (1u << 4) | (window ? (1u << 15) : 0));
        gpu.writeWinOut(0xFFFFu, 0x3F20u);
        gpu.writeBldCnt(0xFFFFu, 1u << 13); // OBJ alpha targets black backdrop
        gpu.writeMasterBright(0xFFFFu, 0);
        gpu.drawScanline(0);
        for (int x = 0; x < 256; x++)
        {
            int winner = -1, bestPriority = 4;
            const bool inObjWindow = x < 8 && ((x + 1) % 3); // sprite 127, tile 1
            if (x < 8 && (!window || inObjWindow))
                for (int i = 0; i < (window ? 127 : 128); i++)
                {
                    const int color = i % 3;
                    const int priority = (i + rotation) & 3;
                    if ((color == 0 || ((x + color) % 3)) && priority < bestPriority)
                    {
                        winner = i;
                        bestPriority = priority;
                    }
                }
            uint32_t expected = 0;
            if (winner >= 0)
                for (int ch = 0; ch < 3; ch++)
                {
                    unsigned level = ((colors[winner % 3] >> (ch * 5)) & 31) * 2;
                    if (winner & 1) level /= 2;
                    expected |= level << (ch * 6);
                }
            assert((gpu.getRawLine()[x] & 0x3FFFFu) == expected);
        }
    }
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    gpu.resetProfile();
    gpu.setProfileEnabled(true);
    for (int i = 0; i < 512; i++) gpu.drawScanline(0);
    assert(gpu.getProfile().objDetail.culledSprites > 0);
    gpu.setProfileEnabled(false);
#endif
    // Restore the preceding opaque scene for the sampled work counters.
    gpu.writeDispCnt(0xFFFFFFFFu, (1u << 27) | (1u << 16) | (0xFu << 8));
    gpu.writeBldCnt(0xFFFFu, 0);
    for (int bg = 0; bg < 4; bg++)
    {
        gpu.writeBgCnt(bg, 0xFFFFu, (bg << 8) | (bg << 2));
        gpu.writeBgHOfs(bg, 0xFFFFu, 0);
    }
    core->memory.write<uint32_t>(0, 0x6000000u, 0x11111111u); // opaque BG0
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    gpu.resetProfile();
    gpu.setProfileEnabled(true);
    for (int i = 0; i < 512; ++i)
        gpu.drawScanline(0);
    const Gpu2DTextDetailCounters &detail = gpu.getProfile().textDetail;
    assert(detail.culledTiles > 0);
    assert(detail.paletteReads + detail.rejected == detail.writes);
#endif
}

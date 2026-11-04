/*
    Copyright 2019-2021 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#include <cstring>

#include "gpu_2d.h"
#include "gpu_2d_math.h"
#include "core.h"

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
#define GPU2D_DETAIL_INC(ptr, field, amount) \
    do { if (ptr) (ptr)->field += (amount); } while (0)
#else
#define GPU2D_DETAIL_INC(ptr, field, amount) do {} while (0)
#endif

Gpu2D::Gpu2D(Core *core, bool engine): core(core), engine(engine)
{
    if (engine == 0)
    {
        // Set up 2D GPU engine A
        bgVramAddr = 0x6000000;
        objVramAddr = 0x6400000;
        palette = core->memory.getPalette();
        oam = core->memory.getOam();
        extPalettes = core->memory.getEngAExtPal();
    }
    else
    {
        // Set up 2D GPU engine B
        bgVramAddr = 0x6200000;
        objVramAddr = 0x6600000;
        palette = core->memory.getPalette() + 0x400;
        oam = core->memory.getOam() + 0x400;
        extPalettes = core->memory.getEngBExtPal();
    }
}

#ifdef NOODS_PROFILE
bool Gpu2D::beginProfilePhase(Gpu2DProfilePhase phase, clock_t &start)
{
    if (!profileEnabled)
        return false;

    runProfile.phaseCount[phase]++;
    bool sampled = profileSample64(runProfile.phaseSampleState[phase],
        phase + (engine ? 0x300u : 0x200u));
    if (sampled)
        start = clock();
    return sampled;
}

void Gpu2D::endProfilePhase(Gpu2DProfilePhase phase, clock_t start, bool sampled)
{
    if (!sampled)
        return;

    runProfile.phaseTicks[phase] += static_cast<uint64_t>(clock() - start);
    runProfile.phaseSamples[phase]++;
}

#if NOODS_PROFILE_LEVEL >= 2
bool Gpu2D::beginExtendedProfile(Gpu2DExtendedMode mode, clock_t &start)
{
    if (!profileEnabled)
        return false;

    runProfile.phaseCount[GPU2D_PROFILE_EXTENDED]++;
    runProfile.extendedModeCount[mode]++;
    bool sampled = profileSample64(runProfile.extendedModeSampleState[mode],
        mode + (engine ? 0x500u : 0x400u));
    if (sampled)
        start = clock();
    return sampled;
}

void Gpu2D::endExtendedProfile(Gpu2DExtendedMode mode, clock_t start,
    clock_t setupEnd, bool sampled, const Gpu2DExtendedCounters &counters)
{
    if (!sampled)
        return;

    clock_t end = clock();
    uint64_t totalTicks = static_cast<uint64_t>(end - start);
    runProfile.phaseTicks[GPU2D_PROFILE_EXTENDED] += totalTicks;
    runProfile.phaseSamples[GPU2D_PROFILE_EXTENDED]++;
    runProfile.extendedModeTicks[mode] += totalTicks;
    runProfile.extendedSetupTicks[mode] +=
        static_cast<uint64_t>(setupEnd - start);
    runProfile.extendedBodyTicks[mode] +=
        static_cast<uint64_t>(end - setupEnd);
    runProfile.extendedModeSamples[mode]++;

    Gpu2DExtendedCounters &total = runProfile.extendedCounters[mode];
    total.pixels += counters.pixels;
    total.wrapped += counters.wrapped;
    total.rejected += counters.rejected;
    total.vramReads += counters.vramReads;
    total.tileCacheHits += counters.tileCacheHits;
    total.directPageHits += counters.directPageHits;
    total.paletteReads += counters.paletteReads;
    total.transparent += counters.transparent;
    total.writes += counters.writes;
}
#endif
#endif

uint32_t Gpu2D::rgb5ToRgb6(uint32_t color)
{
    return gpu2dRgb5ToRgb6(color);
}

void Gpu2D::drawGbaScanline(int line)
{
    // Reload the internal registers at the start of the frame
    if (line == 0)
    {
        internalX[0] = bgX[0];
        internalX[1] = bgX[1];
        internalY[0] = bgY[0];
        internalY[1] = bgY[1];
    }

    {
#ifdef NOODS_PROFILE
        ProfileScope profile(this, GPU2D_PROFILE_CLEAR);
#endif
        // Clear the layers with the backdrop (first palette index).
        // When blending is disabled the lower layer is never populated or
        // consumed, so avoid clearing 240 pixels and metadata unnecessarily.
        uint16_t backdrop = U8TO16(palette, 0) & ~BIT(15);
        gpu2dFill32(layers[0], 240, backdrop);
        memset(priorities[0], 4, 240 * sizeof(uint8_t));
        memset(blendBits[0], 5, 240 * sizeof(uint8_t));
        if (bldCnt != 0)
        {
            gpu2dFill32(layers[1], 240, backdrop);
            memset(priorities[1], 4, 240 * sizeof(uint8_t));
            memset(blendBits[1], 5, 240 * sizeof(uint8_t));
        }
    }

    // Draw the objects, object window first if enabled
    if (dispCnt & BIT(12))
    {
        if (dispCnt & BIT(15)) drawObjects(line, true);
        drawObjects(line, false);
    }

    // Draw the background layers depending on the BG mode
    switch (dispCnt & 0x7)
    {
        case 0:
            if (dispCnt & BIT(11)) drawText(3, line);
            if (dispCnt & BIT(10)) drawText(2, line);
            if (dispCnt & BIT(9))  drawText(1, line);
            if (dispCnt & BIT(8))  drawText(0, line);
            break;

        case 1:
            if (dispCnt & BIT(10)) drawAffine(2, line);
            if (dispCnt & BIT(9))    drawText(1, line);
            if (dispCnt & BIT(8))    drawText(0, line);
            break;

        case 2:
            if (dispCnt & BIT(11)) drawAffine(3, line);
            if (dispCnt & BIT(10)) drawAffine(2, line);
            break;

        case 3: case 4: case 5:
            if (dispCnt & BIT(10)) drawExtended(2, line);
            break;

        default:
            LOG("Unknown GBA BG mode: %d\n", dispCnt & 0x0007);
            break;
    }

    {
#ifdef NOODS_PROFILE
        ProfileScope profile(this, GPU2D_PROFILE_COMPOSE);
#endif
        // Blend the layers to form the final image. With BLDCNT=0
        // every existing branch is a no-op, so avoid walking all 240 pixels.
        if (bldCnt != 0)
        {
            const uint8_t mode = (bldCnt >> 6) & 0x3;
            const uint8_t eva = std::min((bldAlpha >> 0) & 0x1F, 16);
            const uint8_t evb = std::min((bldAlpha >> 8) & 0x1F, 16);
            for (int i = 0; i < 240; i++)
            {

        // Check if blending can/should be performed
        if (layers[0][i] & BIT(25)) // Semi-transparent pixel
        {
            // Force alpha blending if possible, otherwise allow brightness blending or do nothing
            if (bldCnt & BIT(8 + blendBits[1][i])) // Below pixel is blendable
                goto alpha;
            else if (mode < 2 || !(bldCnt & BIT(blendBits[0][i])))
                continue;
        }
        else if (mode == 0 || !(bldCnt & BIT(blendBits[0][i])) || (mode == 1 && !(bldCnt & BIT(8 + blendBits[1][i]))))
        {
            // Do nothing if blending is disabled or not possible
            continue;
        }

        // Skip blending if the pixel is in the bounds of a window that has it disabled
        if (dispCnt & 0x0000E000) // Windows enabled
        {
            uint8_t enabled;
            if ((dispCnt & BIT(13)) && i >= winX1[0] && i < winX2[0] && line >= winY1[0] && line < winY2[0])
                enabled = winIn >> 0; // Window 0
            else if ((dispCnt & BIT(14)) && i >= winX1[1] && i < winX2[1] && line >= winY1[1] && line < winY2[1])
                enabled = winIn >> 8; // Window 1
            else if ((dispCnt & BIT(15)) && (framebuffer[line * 256 + i] & BIT(24)))
                enabled = winOut >> 8; // Object window
            else
                enabled = winOut >> 0; // Outside of windows
            if (!(enabled & BIT(5))) continue;
        }

        // Apply blending, using 15-bit colors in GBA mode
            switch (mode)
            {
            case 1: // Alpha blending
            alpha:
            {
                uint8_t r = std::min<uint32_t>(((layers[0][i] >>  0) & 0x1F) * eva / 16 + ((layers[1][i] >>  0) & 0x1F) * evb / 16, 31);
                uint8_t g = std::min<uint32_t>(((layers[0][i] >>  5) & 0x1F) * eva / 16 + ((layers[1][i] >>  5) & 0x1F) * evb / 16, 31);
                uint8_t b = std::min<uint32_t>(((layers[0][i] >> 10) & 0x1F) * eva / 16 + ((layers[1][i] >> 10) & 0x1F) * evb / 16, 31);
                layers[0][i] = (b << 10) | (g << 5) | r;
                continue;
            }

            case 2: // Brightness increase
            {
                uint8_t r = (layers[0][i] >>  0) & 0x1F; r += (31 - r) * bldY / 16;
                uint8_t g = (layers[0][i] >>  5) & 0x1F; g += (31 - g) * bldY / 16;
                uint8_t b = (layers[0][i] >> 10) & 0x1F; b += (31 - b) * bldY / 16;
                layers[0][i] = (b << 10) | (g << 5) | r;
                continue;
            }

            case 3: // Brightness decrease
            {
                uint8_t r = (layers[0][i] >>  0) & 0x1F; r -= r * bldY / 16;
                uint8_t g = (layers[0][i] >>  5) & 0x1F; g -= g * bldY / 16;
                uint8_t b = (layers[0][i] >> 10) & 0x1F; b -= b * bldY / 16;
                layers[0][i] = (b << 10) | (g << 5) | r;
                continue;
            }
            }
        }
        }
    }

    {
#ifdef NOODS_PROFILE
        ProfileScope profile(this, GPU2D_PROFILE_OUTPUT);
#endif
        // Copy the final image to the framebuffer
        memcpy(&framebuffer[line * 256], layers[0], 240 * sizeof(uint32_t));
    }
}

void Gpu2D::drawScanline(int line)
{
    // Reload the internal registers at the start of the frame
    if (line == 0)
    {
        internalX[0] = bgX[0];
        internalX[1] = bgX[1];
        internalY[0] = bgY[0];
        internalY[1] = bgY[1];
    }

    {
#ifdef NOODS_PROFILE
        ProfileScope profile(this, GPU2D_PROFILE_CLEAR);
#endif
        // Clear the layers with the backdrop (first palette index).
        // BLDCNT=0 makes the lower layer unobservable, matching the top-only
        // pixel path below and halving this fixed scanline clear.
        uint16_t backdrop = U8TO16(palette, 0) & ~BIT(15);
        gpu2dFill32(layers[0], 256, backdrop);
        memset(priorities[0], 4, 256 * sizeof(uint8_t));
        memset(blendBits[0], 5, 256 * sizeof(uint8_t));
        if (bldCnt != 0)
        {
            gpu2dFill32(layers[1], 256, backdrop);
            memset(priorities[1], 4, 256 * sizeof(uint8_t));
            memset(blendBits[1], 5, 256 * sizeof(uint8_t));
        }
    }

    // Draw the objects, object window first if enabled
    if (dispCnt & BIT(12))
    {
        if (dispCnt & BIT(15)) drawObjects(line, true);
        drawObjects(line, false);
    }

    // Draw the background layers depending on the BG mode
    switch (dispCnt & 0x7)
    {
        case 0:
            if (dispCnt & BIT(11)) drawText(3, line);
            if (dispCnt & BIT(10)) drawText(2, line);
            if (dispCnt & BIT(9))  drawText(1, line);
            if (dispCnt & BIT(8))  drawText(0, line);
            break;

        case 1:
            if (dispCnt & BIT(11)) drawAffine(3, line);
            if (dispCnt & BIT(10))   drawText(2, line);
            if (dispCnt & BIT(9))    drawText(1, line);
            if (dispCnt & BIT(8))    drawText(0, line);
            break;

        case 2:
            if (dispCnt & BIT(11)) drawAffine(3, line);
            if (dispCnt & BIT(10)) drawAffine(2, line);
            if (dispCnt & BIT(9))    drawText(1, line);
            if (dispCnt & BIT(8))    drawText(0, line);
            break;

        case 3:
            if (dispCnt & BIT(11)) drawExtended(3, line);
            if (dispCnt & BIT(10))     drawText(2, line);
            if (dispCnt & BIT(9))      drawText(1, line);
            if (dispCnt & BIT(8))      drawText(0, line);
            break;

        case 4:
            if (dispCnt & BIT(11)) drawExtended(3, line);
            if (dispCnt & BIT(10))   drawAffine(2, line);
            if (dispCnt & BIT(9))      drawText(1, line);
            if (dispCnt & BIT(8))      drawText(0, line);
            break;

        case 5:
            if (dispCnt & BIT(11)) drawExtended(3, line);
            if (dispCnt & BIT(10)) drawExtended(2, line);
            if (dispCnt & BIT(9))      drawText(1, line);
            if (dispCnt & BIT(8))      drawText(0, line);
            break;

        case 6:
            if (dispCnt & BIT(10)) drawLarge(2, line);
            break;

        default:
            LOG("Unknown engine %c BG mode: %d\n", ((engine == 0) ? 'A' : 'B'), dispCnt & 0x7);
            break;
    }

    const uint32_t displayMode = (dispCnt >> 16) & 0x3;
    bool directLayerOutput = false;

    {
#ifdef NOODS_PROFILE
        ProfileScope profile(this, GPU2D_PROFILE_COMPOSE);
#endif
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
        Gpu2DComposeDetailCounters *composeDetail = nullptr;
        if (profileEnabled &&
            profileSample64(runProfile.composeDetailSampleState,
                0x720u + static_cast<uint32_t>(engine)))
        {
            composeDetail = &runProfile.composeDetail;
            runProfile.composeDetailSamples++;
            composeDetail->calls++;
        }
#endif
        // With blending disabled and normal layer output selected, conversion
        // can go straight to the framebuffer. This avoids writing converted
        // pixels back to layers[0] only to memcpy them immediately afterwards.
        if (bldCnt == 0)
        {
            GPU2D_DETAIL_INC(composeDetail, noBlendCalls, 1);
            if (displayMode == 1)
            {
                directLayerOutput = true;
                const uint64_t threeDMask =
                    static_cast<uint64_t>(BIT(26)) |
                    (static_cast<uint64_t>(BIT(26)) << 32);
                uint32_t *output = &framebuffer[line * 256];

                GPU2D_DETAIL_INC(composeDetail, pixels, 256);
                GPU2D_DETAIL_INC(composeDetail, directWrites, 256);
                for (int i = 0; i < 256; i += 2)
                {
                    uint64_t pair;
                    std::memcpy(&pair, &layers[0][i], sizeof(pair));

                    if ((pair & threeDMask) == 0)
                    {
                        GPU2D_DETAIL_INC(composeDetail, rgbConversions, 2);
                        pair = gpu2dRgb5PairToRgb6(pair);
                    }
                    else
                    {
                        uint32_t low = static_cast<uint32_t>(pair);
                        uint32_t high = static_cast<uint32_t>(pair >> 32);
                        if (low & BIT(26))
                            GPU2D_DETAIL_INC(composeDetail, threeDPixels, 1);
                        else
                        {
                            GPU2D_DETAIL_INC(composeDetail, rgbConversions, 1);
                            low = gpu2dRgb5ToRgb6(low);
                        }
                        if (high & BIT(26))
                            GPU2D_DETAIL_INC(composeDetail, threeDPixels, 1);
                        else
                        {
                            GPU2D_DETAIL_INC(composeDetail, rgbConversions, 1);
                            high = gpu2dRgb5ToRgb6(high);
                        }
                        pair = static_cast<uint64_t>(low) |
                            (static_cast<uint64_t>(high) << 32);
                    }

                    std::memcpy(&output[i], &pair, sizeof(pair));
                }
            }
        }
        else if (displayMode == 1)
        {
            GPU2D_DETAIL_INC(composeDetail, blendedCalls, 1);
            // Compose directly into the final scanline. The old path rewrote
            // layers[0] and then memcpy'd all 256 pixels to framebuffer even
            // though no later stage consumes the composed layer buffer.
            directLayerOutput = true;
            uint32_t *output = &framebuffer[line * 256];
            const uint8_t mode = (bldCnt >> 6) & 0x3;
            const uint8_t eva = std::min((bldAlpha >> 0) & 0x1F, 16);
            const uint8_t evb = std::min((bldAlpha >> 8) & 0x1F, 16);
            const bool noWindows = (dispCnt & 0x0000E000u) == 0;
            GPU2D_DETAIL_INC(composeDetail, windowedCalls,
                noWindows ? 0 : 1);
            GPU2D_DETAIL_INC(composeDetail, pixels, 256);

            for (int i = 0; i < 256; i++)
            {
                uint32_t pixel = layers[0][i];
                bool forceAlpha = false;

                // Check if blending can/should be performed
                if (pixel & BIT(26)) // 3D pixel
                {
                    GPU2D_DETAIL_INC(composeDetail, threeDPixels, 1);
                    if (bldCnt & BIT(8 + blendBits[1][i]))
                    {
                        GPU2D_DETAIL_INC(composeDetail, special3DAlpha, 1);
                        // 3D alpha blending overrides the normal rules and,
                        // matching the original path, occurs before window
                        // effect masking.
                        uint32_t blend = rgb5ToRgb6(layers[1][i]);
                        uint8_t alphaA = ((pixel >> 18) & 0x3F) + 1;
                        uint8_t alphaB = 64 - alphaA;
                        uint8_t r = std::min<uint32_t>(
                            ((pixel >> 0) & 0x3F) * alphaA / 64 +
                            ((blend >> 0) & 0x3F) * alphaB / 64, 63);
                        uint8_t g = std::min<uint32_t>(
                            ((pixel >> 6) & 0x3F) * alphaA / 64 +
                            ((blend >> 6) & 0x3F) * alphaB / 64, 63);
                        uint8_t b = std::min<uint32_t>(
                            ((pixel >> 12) & 0x3F) * alphaA / 64 +
                            ((blend >> 12) & 0x3F) * alphaB / 64, 63);
                        output[i] = (b << 12) | (g << 6) | r;
                        GPU2D_DETAIL_INC(composeDetail, directWrites, 1);
                        continue;
                    }
                    if (mode < 2 || !(bldCnt & BIT(blendBits[0][i])))
                    {
                        output[i] = pixel;
                        continue;
                    }
                }
                else
                {
                    // Convert the pixel to 18-bit since it isn't a 3D pixel.
                    GPU2D_DETAIL_INC(composeDetail, rgbConversions, 1);
                    pixel = rgb5ToRgb6(pixel);

                    if (pixel & BIT(25)) // Semi-transparent object pixel
                    {
                        GPU2D_DETAIL_INC(composeDetail,
                            semiTransparentPixels, 1);
                        if (bldCnt & BIT(8 + blendBits[1][i]))
                            forceAlpha = true;
                        else if (mode < 2 ||
                            !(bldCnt & BIT(blendBits[0][i])))
                        {
                            output[i] = pixel;
                            continue;
                        }
                    }
                    else if (mode == 0 ||
                        !(bldCnt & BIT(blendBits[0][i])) ||
                        (mode == 1 &&
                            !(bldCnt & BIT(8 + blendBits[1][i]))))
                    {
                        output[i] = pixel;
                        continue;
                    }
                }

                // Skip blending if a window disables color effects. Avoid the
                // entire decision tree on the common no-window path.
                if (!noWindows)
                {
                    GPU2D_DETAIL_INC(composeDetail, windowChecks, 1);
                    uint8_t enabled;
                    if ((dispCnt & BIT(13)) &&
                        i >= winX1[0] && i < winX2[0] &&
                        line >= winY1[0] && line < winY2[0])
                        enabled = winIn >> 0;
                    else if ((dispCnt & BIT(14)) &&
                        i >= winX1[1] && i < winX2[1] &&
                        line >= winY1[1] && line < winY2[1])
                        enabled = winIn >> 8;
                    else if ((dispCnt & BIT(15)) &&
                        (output[i] & BIT(24)))
                        enabled = winOut >> 8;
                    else
                        enabled = winOut >> 0;

                    if (!(enabled & BIT(5)))
                    {
                        GPU2D_DETAIL_INC(composeDetail, windowRejects, 1);
                        GPU2D_DETAIL_INC(composeDetail, directWrites, 1);
                        output[i] = pixel;
                        continue;
                    }
                }

                // Apply blending, using 18-bit colors in DS mode.
                if (forceAlpha || mode == 1)
                {
                    GPU2D_DETAIL_INC(composeDetail, alphaBlend, 1);
                    uint32_t blend = (layers[1][i] & BIT(26))
                        ? layers[1][i] : rgb5ToRgb6(layers[1][i]);
                    uint8_t r = std::min<uint32_t>(
                        ((pixel >> 0) & 0x3F) * eva / 16 +
                        ((blend >> 0) & 0x3F) * evb / 16, 63);
                    uint8_t g = std::min<uint32_t>(
                        ((pixel >> 6) & 0x3F) * eva / 16 +
                        ((blend >> 6) & 0x3F) * evb / 16, 63);
                    uint8_t b = std::min<uint32_t>(
                        ((pixel >> 12) & 0x3F) * eva / 16 +
                        ((blend >> 12) & 0x3F) * evb / 16, 63);
                    pixel = (b << 12) | (g << 6) | r;
                }
                else if (mode == 2)
                {
                    GPU2D_DETAIL_INC(composeDetail, brightUp, 1);
                    uint8_t r = (pixel >> 0) & 0x3F;
                    uint8_t g = (pixel >> 6) & 0x3F;
                    uint8_t b = (pixel >> 12) & 0x3F;
                    r += (63 - r) * bldY / 16;
                    g += (63 - g) * bldY / 16;
                    b += (63 - b) * bldY / 16;
                    pixel = (b << 12) | (g << 6) | r;
                }
                else if (mode == 3)
                {
                    GPU2D_DETAIL_INC(composeDetail, brightDown, 1);
                    uint8_t r = (pixel >> 0) & 0x3F;
                    uint8_t g = (pixel >> 6) & 0x3F;
                    uint8_t b = (pixel >> 12) & 0x3F;
                    r -= r * bldY / 16;
                    g -= g * bldY / 16;
                    b -= b * bldY / 16;
                    pixel = (b << 12) | (g << 6) | r;
                }

                GPU2D_DETAIL_INC(composeDetail, directWrites, 1);
                output[i] = pixel;
            }
        }
    }

    {
#ifdef NOODS_PROFILE
        ProfileScope profile(this, GPU2D_PROFILE_OUTPUT);
#endif
        // Copy the final image to the framebuffer
        switch (displayMode) // Display mode
        {
            case 0: // Display off
            {
                // Fill the display with white
                memset(&framebuffer[line * 256], 0xFF, 256 * sizeof(uint32_t));
                break;
            }

            case 1: // Layers
            {
                if (!directLayerOutput)
                    memcpy(&framebuffer[line * 256], layers[0],
                        256 * sizeof(uint32_t));
                break;
            }

            case 2: // VRAM display
            {
                // Draw raw bitmap data from a VRAM block
                uint32_t address = 0x6800000 + ((dispCnt & 0x000C0000) >> 18) * 0x20000 + line * 256 * 2;
                for (int i = 0; i < 256; i++)
                    framebuffer[line * 256 + i] = rgb5ToRgb6(core->memory.read<uint16_t>(0, address + i * 2));
                break;
            }

            case 3: // Main memory display
            {
                LOG("Unimplemented engine %c display mode: display FIFO\n", ((engine == 0) ? 'A' : 'B'));
                break;
            }
        }
    }

    {
#ifdef NOODS_PROFILE
        ProfileScope profile(this, GPU2D_PROFILE_BRIGHTNESS);
#endif
        // Apply master brightness (DS-only, 18-bit)
        switch ((masterBright >> 14) & 0x3) // Mode
        {
            case 1: // Increase
            {
                uint8_t factor = std::min(masterBright & 0x1F, 16);
                if (!factor)
                    break;
                if (factor == 16)
                {
                    gpu2dFill32(&framebuffer[line * 256], 256, 0x3FFFFu);
                    break;
                }
                for (int i = 0; i < 256; i++)
                {
                    uint32_t *pixel = &framebuffer[line * 256 + i];
                    uint8_t r = (*pixel >>  0) & 0x3F; r += (63 - r) * factor / 16;
                    uint8_t g = (*pixel >>  6) & 0x3F; g += (63 - g) * factor / 16;
                    uint8_t b = (*pixel >> 12) & 0x3F; b += (63 - b) * factor / 16;
                    *pixel = (b << 12) | (g << 6) | r;
                }
                break;
            }

            case 2: // Decrease
            {
                uint8_t factor = std::min(masterBright & 0x1F, 16);
                if (!factor)
                    break;
                if (factor == 16)
                {
                    gpu2dFill32(&framebuffer[line * 256], 256, 0);
                    break;
                }
                for (int i = 0; i < 256; i++)
                {
                    uint32_t *pixel = &framebuffer[line * 256 + i];
                    uint8_t r = (*pixel >>  0) & 0x3F; r -= r * factor / 16;
                    uint8_t g = (*pixel >>  6) & 0x3F; g -= g * factor / 16;
                    uint8_t b = (*pixel >> 12) & 0x3F; b -= b * factor / 16;
                    *pixel = (b << 12) | (g << 6) | r;
                }
                break;
            }
        }
    }
}

FORCE_INLINE void Gpu2D::drawBgPixelNoWindow(int bg, int x,
    uint32_t pixel, int8_t priority)
{
    if (priority <= priorities[0][x])
    {
        // With blending disabled only the top visible layer can affect the
        // final framebuffer. Avoid maintaining the second layer for every BG
        // pixel; it is cleared again before any later scanline can use it.
        if (bldCnt != 0)
        {
            layers[1][x] = layers[0][x];
            priorities[1][x] = priorities[0][x];
            blendBits[1][x] = blendBits[0][x];
        }

        layers[0][x] = pixel;
        priorities[0][x] = priority;
        blendBits[0][x] = bg;
    }
    else if (bldCnt != 0 && priority <= priorities[1][x])
    {
        layers[1][x] = pixel;
        priorities[1][x] = priority;
        blendBits[1][x] = bg;
    }
}

void Gpu2D::drawBgPixel(int bg, int line, int x, uint32_t pixel)
{
    // Skip the pixel if it's in the bounds of a window that has its layer disabled
    if (dispCnt & 0x0000E000) // Windows enabled
    {
        uint8_t enabled;
        if ((dispCnt & BIT(13)) && x >= winX1[0] && x < winX2[0] && line >= winY1[0] && line < winY2[0])
            enabled = winIn >> 0; // Window 0
        else if ((dispCnt & BIT(14)) && x >= winX1[1] && x < winX2[1] && line >= winY1[1] && line < winY2[1])
            enabled = winIn >> 8; // Window 1
        else if ((dispCnt & BIT(15)) && (framebuffer[line * 256 + x] & BIT(24)))
            enabled = winOut >> 8; // Object window
        else
            enabled = winOut >> 0; // Outside of windows
        if (!(enabled & BIT(bg))) return;
    }

    // Draw the pixel to one of 2 layers, depending on priority, for later
    // blending. If BLDCNT is zero the lower layer is unobservable, so skip
    // those stores while keeping identical top-layer priority semantics.
    const int8_t priority = bgCnt[bg] & 0x3;
    if (priority <= priorities[0][x]) // Higher than topmost
    {
        if (bldCnt != 0)
        {
            // Move the topmost pixel to the second topmost
            layers[1][x] = layers[0][x];
            priorities[1][x] = priorities[0][x];
            blendBits[1][x] = blendBits[0][x];
        }

        // Update the topmost pixel
        layers[0][x] = pixel;
        priorities[0][x] = priority;
        blendBits[0][x] = bg;
    }
    else if (bldCnt != 0 && priority <= priorities[1][x])
    {
        // Update the second topmost
        layers[1][x] = pixel;
        priorities[1][x] = priority;
        blendBits[1][x] = bg;
    }
}

FORCE_INLINE void Gpu2D::drawObjPixelNoWindow(int x,
    uint32_t pixel, int8_t priority)
{
    if (!(layers[0][x] & BIT(15)) || priority < priorities[0][x])
    {
        layers[0][x] = pixel;
        priorities[0][x] = priority;
        blendBits[0][x] = 4;
    }
}

void Gpu2D::drawObjPixel(int line, int x, uint32_t pixel, int8_t priority)
{
    // Skip the pixel if it's in the bounds of a window that has objects disabled
    if (dispCnt & 0x0000E000) // Windows enabled
    {
        uint8_t enabled;
        if ((dispCnt & BIT(13)) && x >= winX1[0] && x < winX2[0] && line >= winY1[0] && line < winY2[0])
            enabled = winIn >> 0; // Window 0
        else if ((dispCnt & BIT(14)) && x >= winX1[1] && x < winX2[1] && line >= winY1[1] && line < winY2[1])
            enabled = winIn >> 8; // Window 1
        else if ((dispCnt & BIT(15)) && (framebuffer[line * 256 + x] & BIT(24)))
            enabled = winOut >> 8; // Object window
        else
            enabled = winOut >> 0; // Outside of windows
        if (!(enabled & BIT(4))) return;
    }

    // Draw a pixel to the top layer if the old one is transparent or lower priority
    // Objects are drawn first, and are treated as one layer, so they don't push pixels down
    if (!(layers[0][x] & BIT(15)) || priority < priorities[0][x])
    {
        layers[0][x] = pixel;
        priorities[0][x] = priority;
        blendBits[0][x] = 4;
    }
}

void Gpu2D::drawText(int bg, int line)
{
#ifdef NOODS_PROFILE
    ProfileScope profile(this, GPU2D_PROFILE_TEXT);
#endif
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    Gpu2DTextDetailCounters *textDetail = nullptr;
    if (profileEnabled && profileSample64(runProfile.textDetailSampleState,
        0x620u + static_cast<uint32_t>(engine)))
    {
        textDetail = &runProfile.textDetail;
        runProfile.textDetailSamples++;
        textDetail->calls++;
    }
#endif

    // If 3D is enabled, override BG0 in text mode. The normal text path
    // already bypasses per-pixel window checks when no windows are active;
    // do the same here because 3D BG0 can cover all 256 pixels of every line.
    if (!core->isGbaMode() && bg == 0 && (dispCnt & BIT(3)))
    {
        uint32_t *data = core->gpu3DRenderer.getLine(line);
        const bool noWindows = (dispCnt & 0x0000E000u) == 0;
        GPU2D_DETAIL_INC(textDetail, threeD, 1);
        GPU2D_DETAIL_INC(textDetail, windowed, noWindows ? 0 : 1);
        const int8_t bgPriority = bgCnt[0] & 0x3;
        if (noWindows)
        {
            for (int i = 0; i < 256; i++)
            {
                GPU2D_DETAIL_INC(textDetail, pixelsVisited, 1);
                if (data[i] & 0xFC0000)
                {
                    GPU2D_DETAIL_INC(textDetail, writes, 1);
                    drawBgPixelNoWindow(0, i, data[i], bgPriority);
                }
                else
                {
                    GPU2D_DETAIL_INC(textDetail, transparent, 1);
                }
            }
        }
        else
        {
            for (int i = 0; i < 256; i++)
            {
                GPU2D_DETAIL_INC(textDetail, pixelsVisited, 1);
                if (data[i] & 0xFC0000)
                {
                    GPU2D_DETAIL_INC(textDetail, writes, 1);
                    drawBgPixel(0, line, i, data[i]);
                }
                else
                {
                    GPU2D_DETAIL_INC(textDetail, transparent, 1);
                }
            }
        }
        return;
    }

    // Get the base data addresses
    uint32_t tileBase  = bgVramAddr + ((dispCnt >> 11) & 0x70000) + ((bgCnt[bg] <<  3) & 0x0F800);
    uint32_t indexBase = bgVramAddr + ((dispCnt >>  8) & 0x70000) + ((bgCnt[bg] << 12) & 0x3C000);

    // Move the tile address to the current line
    int yOffset = (((bgCnt[bg] & BIT(6)) ? (line - (line % (((mosaic >> 4) & 0xF) + 1))) : line) + bgVOfs[bg]) & 0x1FF;
    tileBase += (yOffset & 0xF8) << 3;

    // If the Y-offset exceeds 256 and the background is 512 pixels tall, move to the next 256x256 section
    // If the background is 512 pixels wide, move 2 sections to skip the second X section
    if (yOffset >= 256 && (bgCnt[bg] & BIT(15)))
        tileBase += (bgCnt[bg] & BIT(14)) ? 0x1000 : 0x800;

    const bool gba = core->isGbaMode();
    const bool noWindows = (dispCnt & 0x0000E000u) == 0;
    GPU2D_DETAIL_INC(textDetail, windowed, noWindows ? 0 : 1);
    const int8_t bgPriority = bgCnt[bg] & 0x3;
    auto putBgPixel = [&](int x, uint32_t pixel)
    {
        if (noWindows)
            drawBgPixelNoWindow(bg, x, pixel, bgPriority);
        else
            drawBgPixel(bg, line, x, pixel);
    };
    Gpu2DDirectPageCache tilePageCache;
    Gpu2DDirectPageCache indexPageCache;

    // H-offset fine scroll is constant across the whole scanline. Iterate 32
    // tiles when aligned and fetch the 33rd only when a partial tile is
    // actually visible. Advance tile-map X directly instead of rebuilding the
    // wrapped pixel coordinate for every tile.
    const int fineX = bgHOfs[bg] & 7;
    const int tileCount = 32 + (fineX != 0);
    const int firstTileX = (bgHOfs[bg] >> 3) & 0x3F;
    const bool wide = (bgCnt[bg] & BIT(14)) != 0;

    uint8_t *extendedPalette = nullptr;
    if ((bgCnt[bg] & BIT(7)) && (dispCnt & BIT(30)))
    {
        // Extended-palette slot selection depends only on the BG, not on the
        // individual tile, so resolve it once per scanline.
        int slot = (bg < 2 && (bgCnt[bg] & BIT(13))) ? (bg + 2) : bg;
        if (!extPalettes[slot])
            return;
        extendedPalette = extPalettes[slot];
    }

    // Draw a line
    if (bgCnt[bg] & BIT(7)) // 8-bit
    {
        GPU2D_DETAIL_INC(textDetail, mode8, 1);
        for (int tileIndex = 0; tileIndex < tileCount; tileIndex++)
        {
            GPU2D_DETAIL_INC(textDetail, tiles, 1);
            const int i = tileIndex << 3;
            const int tileX = (firstTileX + tileIndex) & 0x3F;
            uint32_t tileAddr = tileBase + ((tileX & 0x1F) << 1);

            // If the X-offset exceeds 256 and the background is 512 pixels wide, move to the next 256x256 section
            if ((tileX & 0x20) && wide)
                tileAddr += 0x800;

            // Get the current tile. Text backgrounds repeatedly touch the
            // same tile-map page, so retain its direct mapping for this scanline.
            uint8_t *page = nullptr;
            uint16_t tile;
            if (tilePageCache.lookup(tileAddr, page))
            {
                GPU2D_DETAIL_INC(textDetail, tilePageHits, 1);
                tile = U8TO16(page, tileAddr & 0xFFFu);
            }
            else if ((page = core->memory.getDirectReadPage(gba, tileAddr)))
            {
                GPU2D_DETAIL_INC(textDetail, tilePageMaps, 1);
                tilePageCache.store(tileAddr, page);
                tile = U8TO16(page, tileAddr & 0xFFFu);
            }
            else
            {
                GPU2D_DETAIL_INC(textDetail, tileFallbackReads, 1);
                tile = core->memory.read<uint16_t>(gba, tileAddr);
            }

            GPU2D_DETAIL_INC(textDetail, hFlipTiles, (tile & BIT(10)) ? 1 : 0);
            GPU2D_DETAIL_INC(textDetail, vFlipTiles, (tile & BIT(11)) ? 1 : 0);

            // Get the tile's palette. Extended slot selection was already
            // resolved once for this scanline above.
            uint8_t *pal = extendedPalette
                ? &extendedPalette[(tile & 0xF000) >> 3]
                : palette;

            // Get the palette indices for the current line of the tile, flipped vertically if enabled
            uint32_t indexAddr = indexBase + (tile & 0x3FF) * 64 + ((tile & BIT(11)) ? (7 - (yOffset & 7)) : (yOffset & 7)) * 8;
            uint64_t indices;
            page = nullptr;
            if (indexPageCache.lookup(indexAddr, page))
            {
                GPU2D_DETAIL_INC(textDetail, indexPageHits, 1);
                indices = U8TO32(page, indexAddr & 0xFFFu) |
                    (static_cast<uint64_t>(U8TO32(page, (indexAddr & 0xFFFu) + 4)) << 32);
            }
            else if ((page = core->memory.getDirectReadPage(gba, indexAddr)))
            {
                GPU2D_DETAIL_INC(textDetail, indexPageMaps, 1);
                indexPageCache.store(indexAddr, page);
                indices = U8TO32(page, indexAddr & 0xFFFu) |
                    (static_cast<uint64_t>(U8TO32(page, (indexAddr & 0xFFFu) + 4)) << 32);
            }
            else
            {
                GPU2D_DETAIL_INC(textDetail, indexFallbackReads, 2);
                indices = core->memory.read<uint32_t>(gba, indexAddr) |
                    (static_cast<uint64_t>(core->memory.read<uint32_t>(gba, indexAddr + 4)) << 32);
            }

            // Draw the current line of the tile, flipped horizontally if enabled
            const int xStep = (tile & BIT(10)) ? -1 : 1;
            for (int x = i - fineX + ((tile & BIT(10)) ? 7 : 0);
                indices != 0; x += xStep, indices >>= 8)
            {
                GPU2D_DETAIL_INC(textDetail, pixelsVisited, 1);
                const uint8_t index = indices & 0xFF;
                if (x < 0 || x >= 256)
                {
                    GPU2D_DETAIL_INC(textDetail, clipped, 1);
                }
                else if (!index)
                {
                    GPU2D_DETAIL_INC(textDetail, transparent, 1);
                }
                else
                {
                    GPU2D_DETAIL_INC(textDetail, paletteReads, 1);
                    GPU2D_DETAIL_INC(textDetail, writes, 1);
                    putBgPixel(x, U8TO16(pal, index * 2) | BIT(15));
                }
            }
        }
    }
    else // 4-bit
    {
        GPU2D_DETAIL_INC(textDetail, mode4, 1);
        for (int tileIndex = 0; tileIndex < tileCount; tileIndex++)
        {
            GPU2D_DETAIL_INC(textDetail, tiles, 1);
            const int i = tileIndex << 3;
            const int tileX = (firstTileX + tileIndex) & 0x3F;
            uint32_t tileAddr = tileBase + ((tileX & 0x1F) << 1);

            // If the X-offset exceeds 256 and the background is 512 pixels wide, move to the next 256x256 section
            if ((tileX & 0x20) && wide)
                tileAddr += 0x800;

            // Get the current tile. Text backgrounds repeatedly touch the
            // same tile-map page, so retain its direct mapping for this scanline.
            uint8_t *page = nullptr;
            uint16_t tile;
            if (tilePageCache.lookup(tileAddr, page))
            {
                GPU2D_DETAIL_INC(textDetail, tilePageHits, 1);
                tile = U8TO16(page, tileAddr & 0xFFFu);
            }
            else if ((page = core->memory.getDirectReadPage(gba, tileAddr)))
            {
                GPU2D_DETAIL_INC(textDetail, tilePageMaps, 1);
                tilePageCache.store(tileAddr, page);
                tile = U8TO16(page, tileAddr & 0xFFFu);
            }
            else
            {
                GPU2D_DETAIL_INC(textDetail, tileFallbackReads, 1);
                tile = core->memory.read<uint16_t>(gba, tileAddr);
            }

            GPU2D_DETAIL_INC(textDetail, hFlipTiles, (tile & BIT(10)) ? 1 : 0);
            GPU2D_DETAIL_INC(textDetail, vFlipTiles, (tile & BIT(11)) ? 1 : 0);

            // Get the tile's palette
            // In 4-bit mode, the tile can select from multiple 16-color palettes
            uint8_t *pal = &palette[(tile & 0xF000) >> 7];

            // Get the palette indices for the current line of the tile, flipped vertically if enabled
            uint32_t indexAddr = indexBase + (tile & 0x3FF) * 32 + ((tile & BIT(11)) ? (7 - (yOffset & 7)) : (yOffset & 7)) * 4;
            uint32_t indices;
            page = nullptr;
            if (indexPageCache.lookup(indexAddr, page))
            {
                GPU2D_DETAIL_INC(textDetail, indexPageHits, 1);
                indices = U8TO32(page, indexAddr & 0xFFFu);
            }
            else if ((page = core->memory.getDirectReadPage(gba, indexAddr)))
            {
                GPU2D_DETAIL_INC(textDetail, indexPageMaps, 1);
                indexPageCache.store(indexAddr, page);
                indices = U8TO32(page, indexAddr & 0xFFFu);
            }
            else
            {
                GPU2D_DETAIL_INC(textDetail, indexFallbackReads, 1);
                indices = core->memory.read<uint32_t>(gba, indexAddr);
            }

            // Draw the current line of the tile, flipped horizontally if enabled
            const int xStep = (tile & BIT(10)) ? -1 : 1;
            for (int x = i - fineX + ((tile & BIT(10)) ? 7 : 0);
                indices != 0; x += xStep, indices >>= 4)
            {
                GPU2D_DETAIL_INC(textDetail, pixelsVisited, 1);
                const uint8_t index = indices & 0xF;
                if (x < 0 || x >= 256)
                {
                    GPU2D_DETAIL_INC(textDetail, clipped, 1);
                }
                else if (!index)
                {
                    GPU2D_DETAIL_INC(textDetail, transparent, 1);
                }
                else
                {
                    GPU2D_DETAIL_INC(textDetail, paletteReads, 1);
                    GPU2D_DETAIL_INC(textDetail, writes, 1);
                    putBgPixel(x, U8TO16(pal, index * 2) | BIT(15));
                }
            }
        }
    }
}

void Gpu2D::drawAffine(int bg, int line)
{
#ifdef NOODS_PROFILE
    ProfileScope profile(this, GPU2D_PROFILE_AFFINE);
#endif

    // Get the base data addresses
    uint32_t tileBase  = bgVramAddr + ((bgCnt[bg] & 0x1F00) >> 8) * 0x0800 + ((dispCnt & 0x38000000) >> 27) * 0x10000;
    uint32_t indexBase = bgVramAddr + ((bgCnt[bg] & 0x003C) >> 2) * 0x4000 + ((dispCnt & 0x07000000) >> 24) * 0x10000;

    // Get the background's size. Classic affine sizes are always powers of two.
    const int size = 128 << ((bgCnt[bg] & 0xC000) >> 14);
    const int tilesPerRow = size >> 3;
    const bool wrap = bg < 2 || (bgCnt[bg] & BIT(13));

    // Increment the fixed-point affine coordinates instead of multiplying by
    // the pixel index on every iteration. This is the same stepper already
    // validated by the extended-background renderer.
    Gpu2DAffineLineStepper coordinates(internalX[bg - 2], internalY[bg - 2],
        bgPA[bg - 2], bgPC[bg - 2]);
    const bool gba = core->isGbaMode();
    const bool noWindows = (dispCnt & 0x0000E000u) == 0;
    const int8_t bgPriority = bgCnt[bg] & 0x3;
    auto putBgPixel = [&](int x, uint32_t pixel)
    {
        if (noWindows)
            drawBgPixelNoWindow(bg, x, pixel, bgPriority);
        else
            drawBgPixel(bg, line, x, pixel);
    };
    Gpu2DLastAddressCache<uint8_t> tileCache;
    Gpu2DDirectPageCache tilePageCache;
    Gpu2DDirectPageCache indexPageCache;

    // Draw a line
    for (int i = 0; i < 256; i++)
    {
        int rotscaleX = coordinates.x();
        int rotscaleY = coordinates.y();
        coordinates.advance();

        // Handle display area overflow. Since size is a power of two, a mask
        // is exactly equivalent to positive modulo and avoids two divisions.
        if (wrap)
        {
            rotscaleX = gpu2dWrapPowerOfTwo(rotscaleX, size);
            rotscaleY = gpu2dWrapPowerOfTwo(rotscaleY, size);
        }
        else if (rotscaleX < 0 || rotscaleX >= size ||
            rotscaleY < 0 || rotscaleY >= size) // Transparent
        {
            continue;
        }

        // Get the current tile. Adjacent affine pixels often remain in the
        // same tile, and their tile/index bytes usually remain in one VRAM page.
        uint32_t tileAddr = tileBase +
            (rotscaleY >> 3) * tilesPerRow + (rotscaleX >> 3);
        uint8_t tile;
        if (!tileCache.lookup(tileAddr, tile))
        {
            uint8_t *page = nullptr;
            if (tilePageCache.lookup(tileAddr, page))
                tile = page[tileAddr & 0xFFFu];
            else if ((page = core->memory.getDirectReadPage(gba, tileAddr)))
            {
                tilePageCache.store(tileAddr, page);
                tile = page[tileAddr & 0xFFFu];
            }
            else
                tile = core->memory.read<uint8_t>(gba, tileAddr);
            tileCache.store(tileAddr, tile);
        }

        // Get the palette index for the current pixel of the tile.
        uint32_t indexAddr = indexBase + tile * 64 +
            (rotscaleY & 7) * 8 + (rotscaleX & 7);
        uint8_t *page = nullptr;
        uint8_t index;
        if (indexPageCache.lookup(indexAddr, page))
            index = page[indexAddr & 0xFFFu];
        else if ((page = core->memory.getDirectReadPage(gba, indexAddr)))
        {
            indexPageCache.store(indexAddr, page);
            index = page[indexAddr & 0xFFFu];
        }
        else
            index = core->memory.read<uint8_t>(gba, indexAddr);

        // Draw a pixel
        if (index)
            putBgPixel(i, U8TO16(palette, index * 2) | BIT(15));
    }

    // Increment the internal registers at the end of the scanline
    internalX[bg - 2] += bgPB[bg - 2];
    internalY[bg - 2] += bgPD[bg - 2];
}

void Gpu2D::drawExtended(int bg, int line)
{
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL < 2
    ProfileScope profile(this, GPU2D_PROFILE_EXTENDED);
#endif

    const bool gba = core->isGbaMode();
    const uint16_t control = bgCnt[bg];
    const bool bitmap = gba || (control & BIT(7));
    const bool directColor = bitmap &&
        ((gba && (dispCnt & 0x0007) != 4) || (!gba && (control & BIT(2))));
    const int affine = bg - 2;

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    Gpu2DExtendedMode extendedMode = bitmap
        ? (directColor ? GPU2D_EXTENDED_DIRECT : GPU2D_EXTENDED_INDEXED)
        : GPU2D_EXTENDED_TILED;
    clock_t extendedStart = 0;
    clock_t extendedSetupEnd = 0;
    Gpu2DExtendedCounters extendedCounters;
    bool extendedSampled = beginExtendedProfile(extendedMode, extendedStart);
#define GPU2D_EXT_COUNT(field, amount) \
    do { if (extendedSampled) extendedCounters.field += (amount); } while (0)
#else
#define GPU2D_EXT_COUNT(field, amount) do {} while (0)
#endif

    if (bitmap)
    {
        uint32_t dataBase = bgVramAddr;
        int sizeX, sizeY;

        // Get information about the bitmap data
        if (gba)
        {
            // Get the base data address
            dataBase += ((control & 0x1F00) >> 8) * 0x0800;

            // Modes 4 and 5 have two bitmaps; one can be drawn while the other is displayed
            // Draw the second bitmap if enabled
            if ((dispCnt & BIT(4)) && (dispCnt & 0x0007) > 3)
                dataBase += 0xA000;

            // Get the bitmap size
            switch (dispCnt & 0x0007)
            {
                case 5:  sizeX = 160; sizeY = 128; break;
                default: sizeX = 240; sizeY = 160; break;
            }
        }
        else
        {
            // Get the base data address
            dataBase += ((control & 0x1F00) >> 8) * 0x4000;

            // Get the bitmap size
            switch ((control & 0xC000) >> 14)
            {
                case 0: sizeX = 128; sizeY = 128; break;
                case 1: sizeX = 256; sizeY = 256; break;
                case 2: sizeX = 512; sizeY = 256; break;
                case 3: sizeX = 512; sizeY = 512; break;
            }
        }

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
        if (extendedSampled)
            extendedSetupEnd = clock();
#endif

        const bool wrap = (control & BIT(13)) != 0;
        Gpu2DAffineLineStepper coordinates(internalX[affine], internalY[affine],
            bgPA[affine], bgPC[affine]);

        if (directColor)
        {
            // Draw a line
            for (int i = 0; i < 256; i++)
            {
                // Calculate the rotscaled coordinates relative to the background
                int rotscaleX = coordinates.x();
                int rotscaleY = coordinates.y();
                coordinates.advance();
                GPU2D_EXT_COUNT(pixels, 1);

                // Handle display area overflow
                if (wrap)
                {
                    rotscaleX %= sizeX; if (rotscaleX < 0) rotscaleX += sizeX;
                    rotscaleY %= sizeY; if (rotscaleY < 0) rotscaleY += sizeY;
                    GPU2D_EXT_COUNT(wrapped, 1);
                }
                else if (rotscaleX < 0 || rotscaleX >= sizeX || rotscaleY < 0 || rotscaleY >= sizeY) // Transparent
                {
                    GPU2D_EXT_COUNT(rejected, 1);
                    continue;
                }

                // Draw a pixel, ignoring transparency in GBA mode
                GPU2D_EXT_COUNT(vramReads, 1);
                uint16_t pixel = (static_cast<uint16_t>(gba) << 15) |
                    core->memory.read<uint16_t>(gba,
                        dataBase + (rotscaleY * sizeX + rotscaleX) * 2);
                if (pixel & BIT(15))
                {
                    GPU2D_EXT_COUNT(writes, 1);
                    drawBgPixel(bg, line, i, pixel);
                }
                else
                {
                    GPU2D_EXT_COUNT(transparent, 1);
                }
            }
        }
        else // 256 color bitmap
        {
            // Draw a line
            for (int i = 0; i < 256; i++)
            {
                // Calculate the rotscaled coordinates relative to the background
                int rotscaleX = coordinates.x();
                int rotscaleY = coordinates.y();
                coordinates.advance();
                GPU2D_EXT_COUNT(pixels, 1);

                // Handle display area overflow
                if (wrap)
                {
                    rotscaleX %= sizeX; if (rotscaleX < 0) rotscaleX += sizeX;
                    rotscaleY %= sizeY; if (rotscaleY < 0) rotscaleY += sizeY;
                    GPU2D_EXT_COUNT(wrapped, 1);
                }
                else if (rotscaleX < 0 || rotscaleX >= sizeX || rotscaleY < 0 || rotscaleY >= sizeY) // Transparent
                {
                    GPU2D_EXT_COUNT(rejected, 1);
                    continue;
                }

                // Get the palette index for the current pixel
                GPU2D_EXT_COUNT(vramReads, 1);
                uint8_t index = core->memory.read<uint8_t>(gba,
                    dataBase + rotscaleY * sizeX + rotscaleX);

                // Draw a pixel
                if (index)
                {
                    GPU2D_EXT_COUNT(paletteReads, 1);
                    GPU2D_EXT_COUNT(writes, 1);
                    drawBgPixel(bg, line, i, U8TO16(palette, index * 2) | BIT(15));
                }
                else
                {
                    GPU2D_EXT_COUNT(transparent, 1);
                }
            }
        }
    }
    else // Extended affine
    {
        // Get the base data addresses
        uint32_t tileBase  = bgVramAddr + ((control & 0x1F00) >> 8) * 0x0800 + ((dispCnt & 0x38000000) >> 27) * 0x10000;
        uint32_t indexBase = bgVramAddr + ((control & 0x003C) >> 2) * 0x4000 + ((dispCnt & 0x07000000) >> 24) * 0x10000;

        // Get the background's size
        int size = 128 << ((control & 0xC000) >> 14);
        const bool wrap = bg < 2 || (control & BIT(13));
        const bool extendedPalette = (dispCnt & BIT(30)) != 0;

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
        if (extendedSampled)
            extendedSetupEnd = clock();
#endif

        Gpu2DAffineLineStepper coordinates(internalX[affine], internalY[affine],
            bgPA[affine], bgPC[affine]);
        Gpu2DLastAddressCache<uint16_t> tileCache;
        Gpu2DDirectPageCache tilePageCache;
        Gpu2DDirectPageCache indexPageCache;

        // Draw a line
        for (int i = 0; i < 256; i++)
        {
            // Calculate the rotscaled coordinates relative to the background
            int rotscaleX = coordinates.x();
            int rotscaleY = coordinates.y();
            coordinates.advance();
            GPU2D_EXT_COUNT(pixels, 1);

            // Handle display area overflow
            if (wrap)
            {
                rotscaleX = gpu2dWrapPowerOfTwo(rotscaleX, size);
                rotscaleY = gpu2dWrapPowerOfTwo(rotscaleY, size);
                GPU2D_EXT_COUNT(wrapped, 1);
            }
            else if (rotscaleX < 0 || rotscaleX >= size || rotscaleY < 0 || rotscaleY >= size) // Transparent
            {
                GPU2D_EXT_COUNT(rejected, 1);
                continue;
            }

            // Get the current tile
            uint32_t tileAddr = tileBase +
                gpu2dExtendedTileOffset(rotscaleX, rotscaleY, size);
            uint16_t tile;
            if (!tileCache.lookup(tileAddr, tile))
            {
                GPU2D_EXT_COUNT(vramReads, 1);
                uint8_t *page = nullptr;
                if (tilePageCache.lookup(tileAddr, page))
                {
                    GPU2D_EXT_COUNT(directPageHits, 1);
                    tile = U8TO16(page, tileAddr & 0xFFFu);
                }
                else if ((page = core->memory.getDirectReadPage(gba, tileAddr)))
                {
                    tilePageCache.store(tileAddr, page);
                    tile = U8TO16(page, tileAddr & 0xFFFu);
                }
                else
                {
                    tile = core->memory.read<uint16_t>(gba, tileAddr);
                }
                tileCache.store(tileAddr, tile);
            }
            else
            {
                GPU2D_EXT_COUNT(tileCacheHits, 1);
            }

            // Get the tile's palette
            uint8_t *pal;
            if (extendedPalette)
            {
                // In extended palette mode, the tile can select from multiple 256-color palettes
                if (!extPalettes[bg])
                {
                    GPU2D_EXT_COUNT(transparent, 1);
                    continue;
                }
                pal = &extPalettes[bg][(tile & 0xF000) >> 3];
            }
            else // Standard palette
            {
                pal = palette;
            }

            // Get the palette index for the current pixel of the tile, flipped vertically or horizontally if enabled
            uint32_t indexAddr = indexBase +
                gpu2dExtendedPixelOffset(tile, rotscaleX, rotscaleY);
            GPU2D_EXT_COUNT(vramReads, 1);
            uint8_t *page = nullptr;
            uint8_t index;
            if (indexPageCache.lookup(indexAddr, page))
            {
                GPU2D_EXT_COUNT(directPageHits, 1);
                index = page[indexAddr & 0xFFFu];
            }
            else if ((page = core->memory.getDirectReadPage(gba, indexAddr)))
            {
                indexPageCache.store(indexAddr, page);
                index = page[indexAddr & 0xFFFu];
            }
            else
            {
                index = core->memory.read<uint8_t>(gba, indexAddr);
            }

            // Draw a pixel
            if (index)
            {
                GPU2D_EXT_COUNT(paletteReads, 1);
                GPU2D_EXT_COUNT(writes, 1);
                drawBgPixel(bg, line, i, U8TO16(pal, index * 2) | BIT(15));
            }
            else
            {
                GPU2D_EXT_COUNT(transparent, 1);
            }
        }
    }

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    endExtendedProfile(extendedMode, extendedStart, extendedSetupEnd,
        extendedSampled, extendedCounters);
#endif
#undef GPU2D_EXT_COUNT

    // Increment the internal registers at the end of the scanline
    internalX[affine] += bgPB[affine];
    internalY[affine] += bgPD[affine];
}

void Gpu2D::drawLarge(int bg, int line)
{
#ifdef NOODS_PROFILE
    ProfileScope profile(this, GPU2D_PROFILE_LARGE);
#endif

    // Get the bitmap size
    int sizeX, sizeY;
    if (((bgCnt[bg] & 0xC000) >> 14) == 0)
    {
        sizeX =  512;
        sizeY = 1024;
    }
    else
    {
        sizeX = 1024;
        sizeY =  512;
    }

    // Draw a line
    for (int i = 0; i < 256; i++)
    {
        // Calculate the rotscaled coordinates relative to the background
        int rotscaleX = (internalX[bg - 2] + bgPA[bg - 2] * i) >> 8;
        int rotscaleY = (internalY[bg - 2] + bgPC[bg - 2] * i) >> 8;

        // Handle display area overflow
        if (bgCnt[bg] & BIT(13)) // Wraparound
        {
            rotscaleX %= sizeX; if (rotscaleX < 0) rotscaleX += sizeX;
            rotscaleY %= sizeY; if (rotscaleY < 0) rotscaleY += sizeY;
        }
        else if (rotscaleX < 0 || rotscaleX >= sizeX || rotscaleY < 0 || rotscaleY >= sizeY) // Transparent
        {
            continue;
        }

        // A full large bitmap requires 512KB of VRAM, but engine B can only use 128KB
        // For engine B, wrap the 128KB bitmap 4 times to cover the full area
        if (engine == 1)
            rotscaleY %= sizeY / 4;

        // Get the palette index for the current pixel
        uint8_t index = core->memory.read<uint8_t>(0, bgVramAddr + rotscaleY * sizeX + rotscaleX);

        // Draw a pixel
        if (index)
            drawBgPixel(bg, line, i, U8TO16(palette, index * 2) | BIT(15));
    }

    // Increment the internal registers at the end of the scanline
    internalX[bg - 2] += bgPB[bg - 2];
    internalY[bg - 2] += bgPD[bg - 2];
}

void Gpu2D::drawObjects(int line, bool window)
{
#ifdef NOODS_PROFILE
    ProfileScope profile(this, GPU2D_PROFILE_OBJECTS);
#endif
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    Gpu2DObjDetailCounters *objDetail = nullptr;
    if (profileEnabled && profileSample64(runProfile.objDetailSampleState,
        0x6A0u + static_cast<uint32_t>(engine)))
    {
        objDetail = &runProfile.objDetail;
        runProfile.objDetailSamples++;
        objDetail->calls++;
        objDetail->windowCalls += window ? 1u : 0u;
    }
#endif

    const bool gba = core->isGbaMode();
    const bool noWindows = (dispCnt & 0x0000E000u) == 0;
    uint32_t *frameLine = &framebuffer[line * 256];
    auto putObjPixel = [&](int x, uint32_t pixel, int8_t priority)
    {
        GPU2D_DETAIL_INC(objDetail, pixelWrites, 1);
        if (noWindows)
            drawObjPixelNoWindow(x, pixel, priority);
        else
            drawObjPixel(line, x, pixel, priority);
    };
    Gpu2DDirectPageCache objectPageCache;

    auto readObject8 = [&](uint32_t address) -> uint8_t
    {
        GPU2D_DETAIL_INC(objDetail, texels, 1);
        uint8_t *page = nullptr;
        if (objectPageCache.lookup(address, page))
        {
            GPU2D_DETAIL_INC(objDetail, pageHits, 1);
            return page[address & 0xFFFu];
        }
        if ((page = core->memory.getDirectReadPage(gba, address)))
        {
            GPU2D_DETAIL_INC(objDetail, pageMaps, 1);
            objectPageCache.store(address, page);
            return page[address & 0xFFFu];
        }
        GPU2D_DETAIL_INC(objDetail, fallbackReads, 1);
        return core->memory.read<uint8_t>(gba, address);
    };

    auto readObject16 = [&](uint32_t address) -> uint16_t
    {
        GPU2D_DETAIL_INC(objDetail, texels, 1);
        uint8_t *page = nullptr;
        if (objectPageCache.lookup(address, page))
        {
            GPU2D_DETAIL_INC(objDetail, pageHits, 1);
            return U8TO16(page, address & 0xFFFu);
        }
        if ((page = core->memory.getDirectReadPage(gba, address)))
        {
            GPU2D_DETAIL_INC(objDetail, pageMaps, 1);
            objectPageCache.store(address, page);
            return U8TO16(page, address & 0xFFFu);
        }
        GPU2D_DETAIL_INC(objDetail, fallbackReads, 1);
        return core->memory.read<uint16_t>(gba, address);
    };

    auto readObject32 = [&](uint32_t address) -> uint32_t
    {
        uint8_t *page = nullptr;
        const uint32_t offset = address & 0xFFFu;
        if (offset <= 0xFFCu && objectPageCache.lookup(address, page))
        {
            GPU2D_DETAIL_INC(objDetail, pageHits, 1);
            return U8TO32(page, offset);
        }
        if (offset <= 0xFFCu &&
            (page = core->memory.getDirectReadPage(gba, address)))
        {
            GPU2D_DETAIL_INC(objDetail, pageMaps, 1);
            objectPageCache.store(address, page);
            return U8TO32(page, offset);
        }
        GPU2D_DETAIL_INC(objDetail, fallbackReads, 1);
        return core->memory.read<uint32_t>(gba, address);
    };

    auto readObject64 = [&](uint32_t address) -> uint64_t
    {
        uint8_t *page = nullptr;
        const uint32_t offset = address & 0xFFFu;
        if (offset <= 0xFF8u && objectPageCache.lookup(address, page))
        {
            GPU2D_DETAIL_INC(objDetail, pageHits, 1);
            return static_cast<uint64_t>(U8TO32(page, offset)) |
                (static_cast<uint64_t>(U8TO32(page, offset + 4)) << 32);
        }
        if (offset <= 0xFF8u &&
            (page = core->memory.getDirectReadPage(gba, address)))
        {
            GPU2D_DETAIL_INC(objDetail, pageMaps, 1);
            objectPageCache.store(address, page);
            return static_cast<uint64_t>(U8TO32(page, offset)) |
                (static_cast<uint64_t>(U8TO32(page, offset + 4)) << 32);
        }
        GPU2D_DETAIL_INC(objDetail, fallbackReads, 2);
        return static_cast<uint64_t>(
            core->memory.read<uint32_t>(gba, address)) |
            (static_cast<uint64_t>(
                core->memory.read<uint32_t>(gba, address + 4)) << 32);
    };

    // Loop through and draw the 128 sprites in OAM
    for (int i = 0; i < 128; i++)
    {
        GPU2D_DETAIL_INC(objDetail, spritesScanned, 1);
        uint8_t byte = oam[i * 8 + 1];
        uint8_t type = (byte >> 2) & 0x3;

        // Skip objects that are disabled or are/aren't window type
        if ((byte & 0x3) == 2 || (type == 2) != window)
        {
            GPU2D_DETAIL_INC(objDetail, spritesDisabled, 1);
            continue;
        }

        // Get the current object
        // Each object takes up 8 bytes in memory, but the last 2 bytes are reserved for rotscale
        uint16_t object[3] =
        {
            (uint16_t)U8TO16(oam, i * 8 + 0),
            (uint16_t)U8TO16(oam, i * 8 + 2),
            (uint16_t)U8TO16(oam, i * 8 + 4)
        };

        // Determine the dimensions of the object
        int width, height;
        switch (((object[0] >> 12) & 0xC) | ((object[1] >> 14) & 0x3)) // Shape, size
        {
            case 0x0: width =  8; height =  8; break; // Square, 0
            case 0x1: width = 16; height = 16; break; // Square, 1
            case 0x2: width = 32; height = 32; break; // Square, 2
            case 0x3: width = 64; height = 64; break; // Square, 3
            case 0x4: width = 16; height =  8; break; // Horizontal, 0
            case 0x5: width = 32; height =  8; break; // Horizontal, 1
            case 0x6: width = 32; height = 16; break; // Horizontal, 2
            case 0x7: width = 64; height = 32; break; // Horizontal, 3
            case 0x8: width =  8; height = 16; break; // Vertical, 0
            case 0x9: width =  8; height = 32; break; // Vertical, 1
            case 0xA: width = 16; height = 32; break; // Vertical, 2
            case 0xB: width = 32; height = 64; break; // Vertical, 3

            default:
                LOG("Unknown object dimensions: shape=%d, size=%d\n", (object[0] >> 14) & 0x3, (object[1] >> 14) & 0x3);
                continue;
        }

        // Double the object bounds for rotscale objects with the double size bit set
        int width2 = width, height2 = height;
        if ((object[0] & 0x0300) == 0x0300)
        {
            width2  *= 2;
            height2 *= 2;
        }

        // Get the Y coordinate and wrap it around if it exceeds the screen bounds
        int y = object[0] & 0xFF;
        if (y >= 192) y -= 256;

        // Don't draw anything if the current scanline lies outside of the object's bounds
        int spriteY = ((object[0] & BIT(12)) ? (line - (line % (((mosaic >> 12) & 0xF) + 1))) : line) - y;
        if (spriteY < 0 || spriteY >= height2)
        {
            GPU2D_DETAIL_INC(objDetail, spritesOutsideY, 1);
            continue;
        }
        GPU2D_DETAIL_INC(objDetail, spritesVisible, 1);

        // Get the X coordinate and wrap it around if it exceeds the screen bounds
        int x = object[1] & 0x1FF;
        if (x >= 256) x -= 512;

        // Objects are higher priority than background layers, so the priority is given a little boost
        int8_t priority = ((object[2] >> 10) & 0x3) - 1;

        // Draw bitmap objects
        if (!gba && type == 3)
        {
            GPU2D_DETAIL_INC(objDetail, bitmapSprites, 1);
            uint32_t dataBase;
            int bitmapWidth;

            // Determine the address and width of the bitmap
            if (dispCnt & BIT(6)) // 1D mapping
            {
                dataBase = objVramAddr + (object[2] & 0x3FF) * ((dispCnt & BIT(22)) ? 256 : 128);
                bitmapWidth = width;
            }
            else // 2D mapping
            {
                uint8_t xMask = (dispCnt & BIT(5)) ? 0x1F : 0x0F;
                dataBase = objVramAddr + (object[2] & xMask) * 0x10 + (object[2] & 0x3FF & ~xMask) * 0x80;
                bitmapWidth = (dispCnt & BIT(5)) ? 256 : 128;
            }

            if (object[0] & BIT(8)) // Rotscale
            {
                // Get the rotscale parameters
                int16_t params[4] =
                {
                    (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x06),
                    (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x0E),
                    (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x16),
                    (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x1E)
                };

                const int centeredY = spriteY - height2 / 2;
                Gpu2DAffineLineStepper objectCoordinates(
                    params[0] * (-width2 / 2) + params[1] * centeredY,
                    params[2] * (-width2 / 2) + params[3] * centeredY,
                    params[0], params[2]);

                // Draw a line of the object
                for (int j = 0; j < width2; j++)
                {
                    int rotscaleX = objectCoordinates.x() + width / 2;
                    int rotscaleY = objectCoordinates.y() + height / 2;
                    objectCoordinates.advance();

                    int offset = x + j;
                    if (offset < 0 || offset >= 256)
                    {
                        GPU2D_DETAIL_INC(objDetail, clipped, 1);
                        continue;
                    }
                    if (rotscaleX < 0 || rotscaleX >= width) continue;
                    if (rotscaleY < 0 || rotscaleY >= height) continue;

                    // Draw a pixel
                    uint16_t pixel = readObject16(
                        dataBase + (rotscaleY * bitmapWidth + rotscaleX) * 2);
                    if (pixel & BIT(15))
                        putObjPixel(offset, pixel, priority);
                }
            }
            else
            {
                // Draw a line of the object
                for (int j = 0; j < width; j++)
                {
                    int offset = x + j;
                    if (offset < 0 || offset >= 256)
                    {
                        GPU2D_DETAIL_INC(objDetail, clipped, 1);
                        continue;
                    }

                    // Draw a pixel
                    uint16_t pixel = readObject16(
                        dataBase + (spriteY * bitmapWidth + j) * 2);
                    if (pixel & BIT(15))
                        putObjPixel(offset, pixel, priority);
                }
            }

            continue;
        }

        // Get the base tile address
        uint32_t tileBase;
        if (gba)
        {
            tileBase = 0x6010000 + (object[2] & 0x3FF) * 32;
        }
        else
        {
            // On the DS, the boundary between tiles can be 32, 64, 128, or 256 bytes for 1D tile mapping
            uint16_t bound = (dispCnt & BIT(4)) ? (32 << ((dispCnt >> 20) & 0x3)) : 32;
            tileBase = objVramAddr + (object[2] & 0x3FF) * bound;
        }

        if (object[0] & BIT(8)) // Rotscale
        {
            GPU2D_DETAIL_INC(objDetail, affineSprites, 1);
            // Get the rotscale parameters
            int16_t params[4] =
            {
                (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x06),
                (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x0E),
                (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x16),
                (int16_t)U8TO16(oam, ((object[1] >> 9) & 0x1F) * 0x20 + 0x1E)
            };

            if (object[0] & BIT(13)) // 8-bit
            {
                GPU2D_DETAIL_INC(objDetail, bpp8Sprites, 1);
                int mapWidth = (dispCnt & BIT(gba ? 6 : 4)) ? width : 128;

                // Get the object's palette
                uint8_t *pal;
                if (dispCnt & BIT(31)) // Extended palette
                {
                    // In extended palette mode, the object can select from multiple 256-color palettes
                    if (!extPalettes[4]) continue;
                    pal = &extPalettes[4][(object[2] & 0xF000) >> 3];
                }
                else // Standard palette
                {
                    pal = &palette[0x200];
                }

                const int centeredY = spriteY - height2 / 2;
                Gpu2DAffineLineStepper objectCoordinates(
                    params[0] * (-width2 / 2) + params[1] * centeredY,
                    params[2] * (-width2 / 2) + params[3] * centeredY,
                    params[0], params[2]);

                // Draw a line of the object
                for (int j = 0; j < width2; j++)
                {
                    int rotscaleX = objectCoordinates.x() + width / 2;
                    int rotscaleY = objectCoordinates.y() + height / 2;
                    objectCoordinates.advance();

                    int offset = x + j;
                    if (offset < 0 || offset >= 256)
                    {
                        GPU2D_DETAIL_INC(objDetail, clipped, 1);
                        continue;
                    }
                    if (rotscaleX < 0 || rotscaleX >= width) continue;
                    if (rotscaleY < 0 || rotscaleY >= height) continue;

                    // Get the palette index for the current pixel
                    uint8_t index = readObject8(tileBase +
                        ((rotscaleY >> 3) * mapWidth + (rotscaleY & 7)) * 8 +
                        (rotscaleX >> 3) * 64 + (rotscaleX & 7));

                    if (index && type == 2) // Object window
                    {
                        // Mark object window pixels with an extra bit, and don't draw anything
                        GPU2D_DETAIL_INC(objDetail, objectWindowWrites, 1);
                        frameLine[offset] |= BIT(24);
                    }
                    else
                    {
                        // Draw a pixel, marking semi-transparent pixels with an extra bit
                        if (index)
                            putObjPixel(offset, ((type == 1) << 25) | BIT(15) | U8TO16(pal, index * 2), priority);

                        // Update the priority even if the pixel is transparent
                        // This is a quirky behavior of the GBA, but it seems to have been fixed on the DS
                        if (gba && priority < priorities[0][offset] && blendBits[0][offset] == 4)
                            priorities[0][offset] = priority;
                    }
                }
            }
            else // 4-bit
            {
                GPU2D_DETAIL_INC(objDetail, bpp4Sprites, 1);
                int mapWidth = (dispCnt & BIT(gba ? 6 : 4)) ? width : 256;

                // Get the object's palette
                // In 4-bit mode, the object can select from multiple 16-color palettes
                uint8_t *pal = &palette[0x200 + ((object[2] & 0xF000) >> 12) * 32];

                const int centeredY = spriteY - height2 / 2;
                Gpu2DAffineLineStepper objectCoordinates(
                    params[0] * (-width2 / 2) + params[1] * centeredY,
                    params[2] * (-width2 / 2) + params[3] * centeredY,
                    params[0], params[2]);

                // Draw a line of the object
                for (int j = 0; j < width2; j++)
                {
                    int rotscaleX = objectCoordinates.x() + width / 2;
                    int rotscaleY = objectCoordinates.y() + height / 2;
                    objectCoordinates.advance();

                    int offset = x + j;
                    if (offset < 0 || offset >= 256)
                    {
                        GPU2D_DETAIL_INC(objDetail, clipped, 1);
                        continue;
                    }
                    if (rotscaleX < 0 || rotscaleX >= width) continue;
                    if (rotscaleY < 0 || rotscaleY >= height) continue;

                    // Get the palette index for the current pixel
                    uint8_t index = readObject8(tileBase +
                        ((rotscaleY >> 3) * mapWidth + (rotscaleY & 7)) * 4 +
                        (rotscaleX >> 3) * 32 + ((rotscaleX & 7) >> 1));
                    index = (rotscaleX & 1) ? ((index & 0xF0) >> 4) : (index & 0x0F);

                    if (index && type == 2) // Object window
                    {
                        // Mark object window pixels with an extra bit, and don't draw anything
                        GPU2D_DETAIL_INC(objDetail, objectWindowWrites, 1);
                        frameLine[offset] |= BIT(24);
                    }
                    else
                    {
                        // Draw a pixel, marking semi-transparent pixels with an extra bit
                        if (index)
                            putObjPixel(offset, ((type == 1) << 25) | BIT(15) | U8TO16(pal, index * 2), priority);

                        // Update the priority even if the pixel is transparent
                        // This is a quirky behavior of the GBA, but it seems to have been fixed on the DS
                        if (gba && priority < priorities[0][offset] && blendBits[0][offset] == 4)
                            priorities[0][offset] = priority;
                    }
                }
            }
        }
        else if (object[0] & BIT(13)) // 8-bit
        {
            GPU2D_DETAIL_INC(objDetail, bpp8Sprites, 1);
            // Adjust the current tile to align with the current Y coordinate relative to the object
            int mapWidth = (dispCnt & BIT(gba ? 6 : 4)) ? width : 128;
            if (object[1] & BIT(13)) // Vertical flip
                tileBase += (7 - (spriteY % 8) + ((height - 1 - spriteY) / 8) * mapWidth) * 8;
            else
                tileBase += ((spriteY % 8) + (spriteY / 8) * mapWidth) * 8;

            // Get the object's palette
            uint8_t *pal;
            if (dispCnt & BIT(31)) // Extended palette
            {
                // In extended palette mode, the object can select from multiple 256-color palettes
                if (!extPalettes[4]) continue;
                pal = &extPalettes[4][(object[2] & 0xF000) >> 3];
            }
            else // Standard palette
            {
                pal = &palette[0x200];
            }

            // Clip once per sprite instead of checking screen bounds for
            // every texel. Texture j remains in the original logical order,
            // including horizontal flip.
            const bool hFlip = (object[1] & BIT(12)) != 0;
            int jStart = 0;
            int jEnd = width;
            if (!hFlip)
            {
                if (x < 0) jStart = -x;
                if (x + jEnd > 256) jEnd = 256 - x;
            }
            else
            {
                if (x + width > 256) jStart = x + width - 256;
                if (x < 0) jEnd = x + width;
            }
            if (jStart < 0) jStart = 0;
            if (jEnd > width) jEnd = width;
            GPU2D_DETAIL_INC(objDetail, clipped,
                static_cast<uint64_t>(width - (jEnd > jStart ? (jEnd - jStart) : 0)));
            if (jStart >= jEnd) continue;

            // Draw one 8-pixel tile row at a time. Normal OBJ rows are
            // contiguous in VRAM, so one 64-bit fetch replaces eight page-cache
            // lookups/byte reads while preserving logical texel order for flip.
            for (int tileJ = jStart & ~7; tileJ < jEnd; tileJ += 8)
            {
                const uint64_t packed = readObject64(
                    tileBase + (tileJ >> 3) * 64);
                const int localStart =
                    (jStart > tileJ) ? (jStart - tileJ) : 0;
                const int localEnd =
                    (jEnd < tileJ + 8) ? (jEnd - tileJ) : 8;
                GPU2D_DETAIL_INC(objDetail, texels,
                    static_cast<uint64_t>(localEnd - localStart));

                for (int local = localStart; local < localEnd; local++)
                {
                    const int j = tileJ + local;
                    const int offset =
                        hFlip ? (x + width - j - 1) : (x + j);
                    const uint8_t index =
                        static_cast<uint8_t>(packed >> (local * 8));

                    if (!index)
                    {
                        GPU2D_DETAIL_INC(objDetail, transparent, 1);
                    }
                    else if (type == 2) // Object window
                    {
                        GPU2D_DETAIL_INC(objDetail, objectWindowWrites, 1);
                        frameLine[offset] |= BIT(24);
                    }
                    else
                    {
                        putObjPixel(offset,
                            ((type == 1) << 25) | BIT(15) |
                            U8TO16(pal, index * 2), priority);
                    }

                    // Preserve the GBA transparent-OBJ priority quirk.
                    if (gba && priority < priorities[0][offset] &&
                        blendBits[0][offset] == 4)
                        priorities[0][offset] = priority;
                }
            }
        }
        else // 4-bit
        {
            GPU2D_DETAIL_INC(objDetail, bpp4Sprites, 1);
            // Adjust the current tile to align with the current Y coordinate relative to the object
            int mapWidth = (dispCnt & BIT(gba ? 6 : 4)) ? width : 256;
            if (object[1] & BIT(13)) // Vertical flip
                tileBase += (7 - (spriteY % 8) + ((height - 1 - spriteY) / 8) * mapWidth) * 4;
            else
                tileBase += ((spriteY % 8) + (spriteY / 8) * mapWidth) * 4;

            // Get the palette of the object
            // In 4-bit mode, the object can select from multiple 16-color palettes
            uint8_t *pal = &palette[0x200 + ((object[2] & 0xF000) >> 12) * 32];

            // Clip once per sprite instead of checking screen bounds for
            // every 4-bit texel.
            const bool hFlip = (object[1] & BIT(12)) != 0;
            int jStart = 0;
            int jEnd = width;
            if (!hFlip)
            {
                if (x < 0) jStart = -x;
                if (x + jEnd > 256) jEnd = 256 - x;
            }
            else
            {
                if (x + width > 256) jStart = x + width - 256;
                if (x < 0) jEnd = x + width;
            }
            if (jStart < 0) jStart = 0;
            if (jEnd > width) jEnd = width;
            GPU2D_DETAIL_INC(objDetail, clipped,
                static_cast<uint64_t>(width - (jEnd > jStart ? (jEnd - jStart) : 0)));
            if (jStart >= jEnd) continue;

            // Four bytes encode one 8-pixel 4bpp tile row. Fetch them
            // together instead of repeating the same byte/page lookup twice.
            for (int tileJ = jStart & ~7; tileJ < jEnd; tileJ += 8)
            {
                const uint32_t packed = readObject32(
                    tileBase + (tileJ >> 3) * 32);
                const int localStart =
                    (jStart > tileJ) ? (jStart - tileJ) : 0;
                const int localEnd =
                    (jEnd < tileJ + 8) ? (jEnd - tileJ) : 8;
                GPU2D_DETAIL_INC(objDetail, texels,
                    static_cast<uint64_t>(localEnd - localStart));

                for (int local = localStart; local < localEnd; local++)
                {
                    const int j = tileJ + local;
                    const int offset =
                        hFlip ? (x + width - j - 1) : (x + j);
                    const uint8_t index =
                        static_cast<uint8_t>((packed >> (local * 4)) & 0xFu);

                    if (!index)
                    {
                        GPU2D_DETAIL_INC(objDetail, transparent, 1);
                    }
                    else if (type == 2) // Object window
                    {
                        GPU2D_DETAIL_INC(objDetail, objectWindowWrites, 1);
                        frameLine[offset] |= BIT(24);
                    }
                    else
                    {
                        putObjPixel(offset,
                            ((type == 1) << 25) | BIT(15) |
                            U8TO16(pal, index * 2), priority);
                    }

                    if (gba && priority < priorities[0][offset] &&
                        blendBits[0][offset] == 4)
                        priorities[0][offset] = priority;
                }
            }
        }
    }
}

void Gpu2D::writeDispCnt(uint32_t mask, uint32_t value)
{
    // Write to the DISPCNT register
    mask &= ((engine == 0) ? 0xFFFFFFFF : 0xC0B1FFF7);
    dispCnt = (dispCnt & ~mask) | (value & mask);
    if (core->isGbaMode()) dispCnt &= 0xFFFF;
}

void Gpu2D::writeBgCnt(int bg, uint16_t mask, uint16_t value)
{
    // Write to one of the BGCNT registers
    if (core->isGbaMode() && bg < 2) mask &= 0xDFFF;
    bgCnt[bg] = (bgCnt[bg] & ~mask) | (value & mask);
}

void Gpu2D::writeBgHOfs(int bg, uint16_t mask, uint16_t value)
{
    // Write to one of the BGHOFS registers
    mask &= 0x01FF;
    bgHOfs[bg] = (bgHOfs[bg] & ~mask) | (value & mask);
}

void Gpu2D::writeBgVOfs(int bg, uint16_t mask, uint16_t value)
{
    // Write to one of the BGVOFS registers
    mask &= 0x01FF;
    bgVOfs[bg] = (bgVOfs[bg] & ~mask) | (value & mask);
}

void Gpu2D::writeBgPA(int bg, uint16_t mask, uint16_t value)
{
    // Write to one of the BGPA registers
    bgPA[bg - 2] = (bgPA[bg - 2] & ~mask) | (value & mask);
}

void Gpu2D::writeBgPB(int bg, uint16_t mask, uint16_t value)
{
    // Write to one of the BGPB registers
    bgPB[bg - 2] = (bgPB[bg - 2] & ~mask) | (value & mask);
}

void Gpu2D::writeBgPC(int bg, uint16_t mask, uint16_t value)
{
    // Write to one of the BGPC registers
    bgPC[bg - 2] = (bgPC[bg - 2] & ~mask) | (value & mask);
}

void Gpu2D::writeBgPD(int bg, uint16_t mask, uint16_t value)
{
    // Write to one of the BGPD registers
    bgPD[bg - 2] = (bgPD[bg - 2] & ~mask) | (value & mask);
}

void Gpu2D::writeBgX(int bg, uint32_t mask, uint32_t value)
{
    // Write to one of the BGX registers
    mask &= 0x0FFFFFFF;
    bgX[bg - 2] = (bgX[bg - 2] & ~mask) | (value & mask);

    // Extend the sign to 32 bits
    if (bgX[bg - 2] & BIT(27)) bgX[bg - 2] |= 0xF0000000; else bgX[bg - 2] &= ~0xF0000000;

    // Reload the internal register
    internalX[bg - 2] = bgX[bg - 2];
}

void Gpu2D::writeBgY(int bg, uint32_t mask, uint32_t value)
{
    // Write to one of the BGY registers
    mask &= 0x0FFFFFFF;
    bgY[bg - 2] = (bgY[bg - 2] & ~mask) | (value & mask);

    // Extend the sign to 32 bits
    if (bgY[bg - 2] & BIT(27)) bgY[bg - 2] |= 0xF0000000; else bgY[bg - 2] &= ~0xF0000000;

    // Reload the internal register
    internalY[bg - 2] = bgY[bg - 2];
}


void Gpu2D::writeWinH(int win, uint16_t mask, uint16_t value)
{
    // Write to one of the WINH registers
    if (mask & 0x00FF) winX2[win] = (value & 0x00FF) >> 0;
    if (mask & 0xFF00) winX1[win] = (value & 0xFF00) >> 8;

    // Handle invalid values
    if (winX1[win] > winX2[win])
        winX2[win] = 256;
}

void Gpu2D::writeWinV(int win, uint16_t mask, uint16_t value)
{
    // Write to one of the WINV registers
    if (mask & 0x00FF) winY2[win] = (value & 0x00FF) >> 0;
    if (mask & 0xFF00) winY1[win] = (value & 0xFF00) >> 8;

    // Handle invalid values
    if (winY1[win] > winY2[win])
        winY2[win] = 192;
}

void Gpu2D::writeWinIn(uint16_t mask, uint16_t value)
{
    // Write to the WININ register
    mask &= 0x3F3F;
    winIn = (winIn & ~mask) | (value & mask);
}

void Gpu2D::writeWinOut(uint16_t mask, uint16_t value)
{
    // Write to the WINOUT register
    mask &= 0x3F3F;
    winOut = (winOut & ~mask) | (value & mask);
}

void Gpu2D::writeMosaic(uint16_t mask, uint16_t value)
{
    mosaic = (mosaic & ~mask) | (value & mask);
}

void Gpu2D::writeBldCnt(uint16_t mask, uint16_t value)
{
    // Write to the BLDCNT register
    mask &= 0x3FFF;
    bldCnt = (bldCnt & ~mask) | (value & mask);
}

void Gpu2D::writeBldAlpha(uint16_t mask, uint16_t value)
{
    // Write to the BLDALPHA register
    mask &= 0x1F1F;
    bldAlpha = (bldAlpha & ~mask) | (value & mask);
}

void Gpu2D::writeBldY(uint8_t value)
{
    // Write to the BLDY register
    bldY = value & 0x1F;
    if (bldY > 16) bldY = 16;
}

void Gpu2D::writeMasterBright(uint16_t mask, uint16_t value)
{
    // Write to the MASTER_BRIGHT register
    mask &= 0xC01F;
    masterBright = (masterBright & ~mask) | (value & mask);
}

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

#include "gpu_3d_renderer.h"
#include "core.h"
#include "settings.h"

namespace
{
inline uint16_t gpu3dLoad16(const uint8_t *data, size_t index)
{
    uint16_t value = 0;
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    std::memcpy(&value, data + index, sizeof(value));
#else
    value = static_cast<uint16_t>(data[index]) |
        (static_cast<uint16_t>(data[index + 1]) << 8);
#endif
    return value;
}
}

Gpu3DRenderer::Gpu3DRenderer(Core *core): core(core)
{
    textureSlots = core->memory.getTex3D();
    paletteSlots = core->memory.getPal3D();

    // Mark the scanlines as ready to start
    // This is mainly in case 3D is requested before the threads have a chance to start
    for (int i = 0; i < 192; i++)
        ready[i].store(3);
}

Gpu3DRenderer::~Gpu3DRenderer()
{
    // Clean up the threads
    for (int i = 0; i < activeThreads; i++)
    {
        if (threads[i]) 
        {
            threads[i]->join();
            delete threads[i];
        }
    }
}

uint32_t Gpu3DRenderer::rgba5ToRgba6(uint32_t color)
{
    // DS 3D expands each non-zero 5-bit component to (value * 2 + 1).
    // Express that directly so the R5900 hot texel path has no per-channel
    // conditional branches.
    uint32_t r = (color >>  0) & 0x1Fu;
    uint32_t g = (color >>  5) & 0x1Fu;
    uint32_t b = (color >> 10) & 0x1Fu;
    uint32_t a = (color >> 15) & 0x1Fu;
    r = (r << 1) + (r != 0);
    g = (g << 1) + (g != 0);
    b = (b << 1) + (b != 0);
    a = (a << 1) + (a != 0);
    return (a << 18) | (b << 12) | (g << 6) | r;
}

uint32_t *Gpu3DRenderer::getLine(int line)
{
    // If a thread is falling behind, see if this thread can help out instead of waiting around
    // Threads go back for the final pass after drawing their next scanline, so check 2 scanlines ahead
    if (ready[line].load() < 3 && line + activeThreads * 2 < 192)
    {
        int next = line + activeThreads * 2;
        switch (ready[next].exchange(1))
        {
            case 0:
                // Draw the scanline if it hasn't been started yet
                drawScanline1(next);
                ready[next].store(2);
                break;

            case 2:
                // If the thread somehow caught up, restore the scanline's state
                if (ready[next].exchange(2) == 3)
                    ready[next].store(3);
                break;

            case 3:
                // If the thread somehow caught up, restore the scanline's state
                ready[next].store(3);
                break;
        }
    }

    // Wait until a scanline is ready, and then return it
    while (ready[line].load() < 3) std::this_thread::yield();
    return &framebuffer[0][256 * line];
}

void Gpu3DRenderer::drawScanline(int line)
{
    if (line == 0)
    {
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
        if (profileEnabled)
            detailProfile.frames++;
#endif
        // Finish the previous frame before touching polygon bounds or the
        // per-line mask. Threaded drawScanline1() reads both structures, so
        // rebuilding them first would race a worker still completing the old
        // frame.
        for (int i = 0; i < activeThreads; i++)
        {
            if (threads[i])
            {
                threads[i]->join();
                delete threads[i];
                threads[i] = nullptr;
            }
        }

        // Calculate the scanline bounds for each polygon and build a
        // compact active-polygon mask for every visible line. drawScanline1()
        // can then skip all polygons that do not intersect its line instead of
        // repeating polygonCount range checks 192 times per frame.
        memset(polygonLineMask, 0, sizeof(polygonLineMask));
        const int polygonCount = core->gpu3D.getPolygonCount();
        _Polygon *polygons = core->gpu3D.getPolygons();
        for (int i = 0; i < polygonCount; i++)
        {
            polygonTop[i] = 192;
            polygonBot[i] =   0;

            _Polygon *polygon = &polygons[i];
            for (int j = 0; j < polygon->size; j++)
            {
                Vertex *vertex = &polygon->vertices[j];
                if (vertex->y < polygonTop[i]) polygonTop[i] = vertex->y;
                if (vertex->y > polygonBot[i]) polygonBot[i] = vertex->y;
            }

            // Allow horizontal line polygons to be drawn
            if (polygonTop[i] == polygonBot[i]) polygonBot[i]++;

            int firstLine = polygonTop[i];
            int lastLine = polygonBot[i];
            if (firstLine < 0) firstLine = 0;
            if (lastLine > 192) lastLine = 192;
            if (firstLine >= lastLine) continue;

            const uint32_t bit = 1u << (i & 31);
            const int word = i >> 5;
            for (int y = firstLine; y < lastLine; y++)
                polygonLineMask[y][word] |= bit;
        }

        // Update the thread count
        activeThreads = Settings::getThreaded3D();
        if (activeThreads > 3) activeThreads = 3;
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
        if (profileEnabled && activeThreads > 0)
            detailProfile.threadedFrames++;
#endif

        // Set up threaded 3D rendering if enabled
        if (activeThreads > 0)
        {
            // Mark the scanlines as not ready
            for (int i = 0; i < 192; i++)
                ready[i].store(0);

            // Create threads to draw the scanlines
            for (int i = 0; i < activeThreads; i++)
                threads[i] = new std::thread(&Gpu3DRenderer::drawThreaded, this, i);
        }
    }

    // Draw one scanline at a time if threading is disabled
    if (activeThreads == 0)
    {
        drawScanline1(line);
        if (line > 0) finishScanline(line - 1);
        if (line == 191) finishScanline(191);
    }
}

void Gpu3DRenderer::drawThreaded(int thread)
{
    // Draw the 3D scanlines in a threaded sequence
    // The amount of scanlines skipped per thread depends on the number of active threads
    // Together, they render the entire 3D image
    int i;
    for (i = thread; i < 192; i += activeThreads)
    {
        switch (ready[i].exchange(1))
        {
            case 0:
                // Draw a scanline if it hasn't been started, save for the final pass
                drawScanline1(i);
                ready[i].store(2);
                break;

            case 2:
                // Restore the scanline's state if it was already drawn
                ready[i].store(2);
                break;
        }

        if (i < activeThreads) continue;
        int prev = i - activeThreads;

        // Wait for this thread's previous scanline and its surrounding scanlines to be drawn
        while ((prev > 0 && ready[prev - 1].load() < 2) || ready[prev].load() < 2 || ready[prev + 1].load() < 2)
            std::this_thread::yield();

        // Finish this thread's previous scanline
        finishScanline(prev);
        ready[prev].store(3);
    }

    int prev = i - activeThreads;

    // Wait for this thread's final scanline and its surrounding scanlines to be drawn
    while (ready[prev - 1].load() < 2 || ready[prev].load() < 2 || (prev < 191 && ready[prev + 1].load() < 2))
        std::this_thread::yield();

    // Finish this thread's final scanline
    finishScanline(prev);
    ready[prev].store(3);
}

void Gpu3DRenderer::drawScanline1(int line)
{
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    bool scanlineTimingSampled = false;
    clock_t scanlineTimingStart = 0;
    if (profileEnabled && activeThreads == 0)
    {
        detailProfile.scanlines++;
        scanlineTimingSampled =
            profileSample64(scanlineTimingState, 0x8C0u);
        if (scanlineTimingSampled)
        {
            detailProfile.scanlineTimingSamples++;
            scanlineTimingStart = clock();
        }
    }
#endif
    // Convert the clear values
    // The attribute buffer contains the polygon IDs (0-5, 6-11), transparency bit (12), fog bit (13), edge bit (14), and edge alpha (15-20)
    uint32_t color = BIT(26) | rgba5ToRgba6(((clearColor & 0x001F0000) >> 1) | (clearColor & 0x00007FFF));
    int32_t depth = (clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9);
    uint32_t attrib = ((clearColor & BIT(15)) >> 2) | ((clearColor & 0x3F000000) >> 18) | ((clearColor & 0x3F000000) >> 24) |
        (0x3F << 15) | (((clearColor & 0x001F0000) && ((clearColor & 0x001F0000) >> 16) < 31) << 12);

    // Clear the scanline buffers with the clear values. Keep line-local
    // pointers and unroll four pixels at a time; older PS2 GCC does not
    // reliably unroll this interleaved three-buffer loop at -O2.
    uint32_t *colorLine = &framebuffer[0][line * 256];
    int32_t *depthLine = &depthBuffer[0][line * 256];
    uint32_t *attribLine = &attribBuffer[0][line * 256];
    for (int i = 0; i < 256; i += 4)
    {
        colorLine[i + 0] = color;
        colorLine[i + 1] = color;
        colorLine[i + 2] = color;
        colorLine[i + 3] = color;

        depthLine[i + 0] = depth;
        depthLine[i + 1] = depth;
        depthLine[i + 2] = depth;
        depthLine[i + 3] = depth;

        attribLine[i + 0] = attrib;
        attribLine[i + 1] = attrib;
        attribLine[i + 2] = attrib;
        attribLine[i + 3] = attrib;
    }

    stencilClear[line] = false;

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    clock_t polygonTimingStart = 0;
    if (scanlineTimingSampled)
    {
        polygonTimingStart = clock();
        detailProfile.clearTicks +=
            static_cast<uint64_t>(
                polygonTimingStart - scanlineTimingStart);
    }
#endif

    // Draw only polygons pre-indexed for this scanline. Two tiny bitset
    // passes preserve the original solid-then-translucent ordering without
    // heap allocation or a 4 KiB per-thread temporary index buffer.
    _Polygon *polygons = core->gpu3D.getPolygons();
    for (int pass = 0; pass < 2; pass++)
    {
        const bool wantTranslucent = pass != 0;
        for (int word = 0; word < 64; word++)
        {
            uint32_t bits = polygonLineMask[line][word];
            while (bits)
            {
                const int bit = __builtin_ctz(bits);
                const int i = (word << 5) + bit;
                bits &= bits - 1;

                _Polygon *polygon = &polygons[i];
                const bool translucent =
                    polygon->alpha < 0x3F ||
                    polygon->textureFmt == 1 ||
                    polygon->textureFmt == 6;
                if (translucent == wantTranslucent)
                    drawPolygon(line, i);
            }
        }
    }

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    if (scanlineTimingSampled)
    {
        detailProfile.polygonTicks +=
            static_cast<uint64_t>(clock() - polygonTimingStart);
    }
#endif
}

void Gpu3DRenderer::finishScanline(int line)
{
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    bool finishProfileSampled = false;
    clock_t finishTimingStart = 0;
    if (profileEnabled && activeThreads == 0)
    {
        finishProfileSampled =
            profileSample64(finishSampleState, 0x8A0u);
        if (finishProfileSampled)
        {
            detailProfile.finishSamples++;
            detailProfile.finishTimingSamples++;
            finishTimingStart = clock();
        }
    }
#endif
    // Perform edge marking if enabled
    if (disp3DCnt & BIT(5))
    {
        for (int i = line * 256; i < (line + 1) * 256; i++)
        {
            if (attribBuffer[0][i] & BIT(14)) // Edge bit
            {
                // Get the polygon IDs of the surrounding pixels
                uint32_t id[4] =
                {
                    ((i % 256 >   0) ? attribBuffer[0][i -   1] : (clearColor >> 24)) & 0x3F, // Left
                    ((i % 256 < 255) ? attribBuffer[0][i +   1] : (clearColor >> 24)) & 0x3F, // Right
                    ((line    >   0) ? attribBuffer[0][i - 256] : (clearColor >> 24)) & 0x3F, // Up
                    ((line    < 191) ? attribBuffer[0][i + 256] : (clearColor >> 24)) & 0x3F  // Down
                };

                // Get the depth values of the surrounding pixels
                int32_t depth[4] =
                {
                    ((i % 256 >   0) ? depthBuffer[0][i -   1] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))), // Left
                    ((i % 256 < 255) ? depthBuffer[0][i +   1] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))), // Right
                    ((line    >   0) ? depthBuffer[0][i - 256] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9))), // Up
                    ((line    < 191) ? depthBuffer[0][i + 256] : ((clearDepth == 0x7FFF) ? 0xFFFFFF : (clearDepth << 9)))  // Down
                };

                // Check the surrounding pixels, and mark the edge if at least one has a different ID and greater depth
                for (int j = 0; j < 4; j++)
                {
                    if ((attribBuffer[0][i] & 0x3F) != id[j] && depthBuffer[0][i] < depth[j])
                    {
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
                        if (finishProfileSampled)
                            detailProfile.edgePixels++;
#endif
                        framebuffer[0][i] = BIT(26) | rgba5ToRgba6((0x1F << 15) | edgeColor[(attribBuffer[0][i] & 0x3F) >> 3]);
                        attribBuffer[0][i] = (attribBuffer[0][i] & ~(0x3F << 15)) | (0x20 << 15);
                        break;
                    }
                }
            }
        }
    }

    // Draw fog if enabled
    if (disp3DCnt & BIT(7))
    {
        uint32_t fog = rgba5ToRgba6(((fogColor & 0x001F0000) >> 1) | (fogColor & 0x00007FFF));
        const int fogControl = (disp3DCnt & 0x0F00) >> 8;
        const int fogShift = 10 - fogControl;
        const bool validFogStep = fogShift >= 0;
        const int fogStep = validFogStep ? (1 << fogShift) : 0;

        for (int layer = 0; layer < ((disp3DCnt & BIT(4)) ? 2 : 1); layer++) // Apply to the back layer as well if anti-aliased
        {
            for (int i = line * 256; i < (line + 1) * 256; i++)
            {
                if (attribBuffer[layer][i] & BIT(13)) // Fog bit
                {
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
                    if (finishProfileSampled)
                        detailProfile.fogPixels++;
#endif
                    // Determine the fog table index for the current pixel's
                    // depth. fogStep is a power of two, so reproduce signed
                    // division toward zero with shifts instead of R5900 DIV.
                    int32_t offset = ((depthBuffer[layer][i] / 0x200) - fogOffset);
                    int n;
                    if (validFogStep)
                        n = (offset >= 0)
                            ? (offset >> fogShift)
                            : -((-offset) >> fogShift);
                    else
                        n = (offset > 0) ? 31 : 0;

                    // Get the fog density from the table
                    uint8_t density;
                    if (n >= 31) // Maximum
                    {
                        density = fogTable[31];
                    }
                    else if (n < 0 || !validFogStep) // Minimum
                    {
                        density = fogTable[0];
                    }
                    else // Linear interpolation
                    {
                        // n is non-negative here, so the remainder is too.
                        const int m = offset - (n << fogShift);
                        const int numerator =
                            fogTable[n + 1] * m +
                            fogTable[n] * (fogStep - m);
                        density = static_cast<uint8_t>(numerator >> fogShift);
                    }

                    // Blend the fog with the pixel
                    uint8_t a = (((fog >> 18) & 0x3F) * density + ((framebuffer[layer][i] >> 18) & 0x3F) * (128 - density)) / 128;
                    if (disp3DCnt & BIT(6)) // Only alpha
                    {
                        framebuffer[layer][i] = (framebuffer[layer][i] & ~(0x3F << 18)) | (a << 18);
                    }
                    else
                    {
                        uint8_t r = (((fog >>  0) & 0x3F) * density + ((framebuffer[layer][i] >>  0) & 0x3F) * (128 - density)) / 128;
                        uint8_t g = (((fog >>  6) & 0x3F) * density + ((framebuffer[layer][i] >>  6) & 0x3F) * (128 - density)) / 128;
                        uint8_t b = (((fog >> 12) & 0x3F) * density + ((framebuffer[layer][i] >> 12) & 0x3F) * (128 - density)) / 128;
                        framebuffer[layer][i] = BIT(26) | (a << 18) | (b << 12) | (g << 6) | r;
                    }
                }
            }
        }
    }

    // Perform anti-aliasing if enabled
    if (disp3DCnt & BIT(4))
    {
        for (int i = line * 256; i < (line + 1) * 256; i++)
        {
            if (((attribBuffer[0][i] >> 15) & 0x3F) < 0x3F) // Edge not opaque
            {
                // Blend with the lower pixel, or simply set the alpha if the lower pixel has alpha 0
                if ((framebuffer[1][i] >> 18) & 0x3F)
                    framebuffer[0][i] = BIT(26) | interpolateColor(framebuffer[1][i], framebuffer[0][i], 0, ((attribBuffer[0][i] >> 15) & 0x3F), 0x3F);
                else
                    framebuffer[0][i] = (framebuffer[0][i] & ~0xFC0000) | ((attribBuffer[0][i] & 0x1F8000) << 3);
            }
        }
    }

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    if (finishProfileSampled)
        detailProfile.finishTicks +=
            static_cast<uint64_t>(clock() - finishTimingStart);
#endif
}

FORCE_INLINE uint8_t *Gpu3DRenderer::getTexture(uint32_t address)
{
    // Get a pointer to texture data
    uint8_t *slot = textureSlots[address >> 17];
    return slot ? &slot[address & 0x1FFFF] : nullptr;
}

FORCE_INLINE uint8_t *Gpu3DRenderer::getPalette(uint32_t address)
{
    // Get a pointer to palette data
    uint8_t *slot = paletteSlots[address >> 14];
    return slot ? &slot[address & 0x3FFF] : nullptr;
}

uint32_t Gpu3DRenderer::interpolateLinear(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2)
{
    if (x <= x1) return v1;
    if (x >= x2) return v2;

    // Linearly interpolate a new value between the min and max values
    if (v1 <= v2)
        return v1 + (v2 - v1) * (x - x1) / (x2 - x1);
    else
        return v2 + (v1 - v2) * (x2 - x) / (x2 - x1);
}

uint32_t Gpu3DRenderer::interpolateLinRev(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2)
{
    if (x <= x1) return v1;
    if (x >= x2) return v2;

    // Linearly interpolate a new value between the min and max values (reverse formula)
    if (v1 <= v2)
        return v2 - (v2 - v1) * (x2 - x) / (x2 - x1);
    else
        return v1 - (v1 - v2) * (x - x1) / (x2 - x1);
}

uint32_t Gpu3DRenderer::interpolateFill(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2, uint32_t w1, uint32_t w2)
{
    if (x <= x1) return v1;
    if (x >= x2) return v2;

    // Fall back to linear interpolation if the W values are equal and their lower bits are clear
    if (w1 == w2 && !(w1 & 0x007F))
        return interpolateLinear(v1, v2, x1, x, x2);

    // Calculate the interpolation factor with a precision of 8 bits for polygon fills
    uint32_t factor = ((w1 * (x - x1)) << 8) / (w2 * (x2 - x) + w1 * (x - x1));

    // Interpolate a new value between the min and max values
    if (v1 <= v2)
        return v1 + (((v2 - v1) * factor) >> 8);
    else
        return v2 + (((v1 - v2) * ((1 << 8) - factor)) >> 8);
}

uint32_t Gpu3DRenderer::interpolateEdge(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2, uint32_t w1, uint32_t w2)
{
    if (x <= x1) return v1;
    if (x >= x2) return v2;

    // Fall back to linear interpolation if the W values are equal and their lower bits are clear
    if (w1 == w2 && !(w1 & 0x00FE))
        return interpolateLinear(v1, v2, x1, x, x2);

    // Adjust the W values to be 15-bit so the calculation doesn't overflow
    uint32_t w1a = w1 >> 1;
    if ((w1 & 1) && !(w2 & 1)) w1a++;
    w1 >>= 1;
    w2 >>= 1;

    // Calculate the interpolation factor with a precision of 9 bits for polygon edges
    uint32_t factor = ((w1 * (x - x1)) << 9) / (w2 * (x2 - x) + w1a * (x - x1));

    // Interpolate a new value between the min and max values
    if (v1 <= v2)
        return v1 + (((v2 - v1) * factor) >> 9);
    else
        return v2 + (((v1 - v2) * ((1 << 9) - factor)) >> 9);
}

uint32_t Gpu3DRenderer::interpolateColor(uint32_t c1, uint32_t c2, uint32_t x1, uint32_t x, uint32_t x2)
{
    // Apply linear interpolation separately on the RGB values
    uint32_t r = interpolateLinear((c1 >>  0) & 0x3F, (c2 >>  0) & 0x3F, x1, x, x2);
    uint32_t g = interpolateLinear((c1 >>  6) & 0x3F, (c2 >>  6) & 0x3F, x1, x, x2);
    uint32_t b = interpolateLinear((c1 >> 12) & 0x3F, (c2 >> 12) & 0x3F, x1, x, x2);
    uint32_t a = (((c1 >> 18) & 0x3F) > ((c2 >> 18) & 0x3F)) ? ((c1 >> 18) & 0x3F) : ((c2 >> 18) & 0x3F);
    return (a << 18) | (b << 12) | (g << 6) | r;
}

uint32_t Gpu3DRenderer::readTexture(_Polygon *polygon, int s, int t,
    uint8_t *basePalette, uint8_t sizeShiftS, bool profileSampled)
{
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    if (profileSampled)
    {
        detailProfile.textureSamples++;
        detailProfile.textureFormat[polygon->textureFmt & 7u]++;
    }
#endif
    // Handle S-coordinate overflows
    if (polygon->repeatS)
    {
        // Flip the S-coordinate every second repeat
        if (polygon->flipS && (s & polygon->sizeS))
            s = -1 - s;

        // Wrap the S-coordinate
        s &= polygon->sizeS - 1;
    }
    else if (s < 0)
    {
        // Clamp the S-coordinate on the left
        s = 0;
    }
    else if (s >= polygon->sizeS)
    {
        // Clamp the S-coordinate on the right
        s = polygon->sizeS - 1;
    }

    // Handle T-coordinate overflows
    if (polygon->repeatT)
    {
        // Flip the T-coordinate every second repeat
        if (polygon->flipT && (t & polygon->sizeT))
            t = -1 - t;

        // Wrap the T-coordinate
        t &= polygon->sizeT - 1;
    }
    else if (t < 0)
    {
        // Clamp the T-coordinate on the top
        t = 0;
    }
    else if (t >= polygon->sizeT)
    {
        // Clamp the T-coordinate on the bottom
        t = polygon->sizeT - 1;
    }

    // Coordinates are guaranteed non-negative after the clamp/wrap logic
    // above. Keep them unsigned so power-of-two texture addressing maps
    // directly to shifts/masks on the R5900 instead of signed / and % paths.
    const uint32_t us = static_cast<uint32_t>(s);
    const uint32_t ut = static_cast<uint32_t>(t);

    // Decode a texel
    switch (polygon->textureFmt)
    {
        case 1: // A3I5 translucent
        {
            // Get the 8-bit palette index
            uint32_t address = polygon->textureAddr + ((ut << sizeShiftS) + us);
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            uint8_t index = *data;

            // Get the palette
            uint8_t *palette = basePalette;
            if (!palette) return 0;

            // Return the palette color
            uint16_t color = gpu3dLoad16(palette, (index & 0x1F) * 2) & ~BIT(15);
            uint8_t alpha = (index >> 5) * 4 + (index >> 5) / 2;
            return rgba5ToRgba6((alpha << 15) | color);
        }

        case 2: // 4-color palette
        {
            // Get the 2-bit palette index
            uint32_t address = polygon->textureAddr + (((ut << sizeShiftS) + us) >> 2);
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            uint8_t index = (*data >> ((us & 3u) << 1)) & 0x03;

            // Return a transparent pixel if enabled
            if (polygon->transparent0 && index == 0)
                return 0;

            // Get the palette
            uint8_t *palette = basePalette;
            if (!palette) return 0;

            // Return the palette color
            return rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, index * 2));
        }

        case 3: // 16-color palette
        {
            // Get the 4-bit palette index
            uint32_t address = polygon->textureAddr + (((ut << sizeShiftS) + us) >> 1);
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            uint8_t index = (*data >> ((us & 1u) << 2)) & 0x0F;

            // Return a transparent pixel if enabled
            if (polygon->transparent0 && index == 0)
                return 0;

            // Get the palette
            uint8_t *palette = basePalette;
            if (!palette) return 0;

            // Return the palette color
            return rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, index * 2));
        }

        case 4: // 256-color palette
        {
            // Get the 8-bit palette index
            uint32_t address = polygon->textureAddr + ((ut << sizeShiftS) + us);
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            uint8_t index = *data;

            // Return a transparent pixel if enabled
            if (polygon->transparent0 && index == 0)
                return 0;

            // Get the palette
            uint8_t *palette = basePalette;
            if (!palette) return 0;

            // Return the palette color
            return rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, index * 2));
        }

        case 5: // 4x4 compressed
        {
            // Get the 2-bit palette index
            uint32_t tile = (ut >> 2) * (1u << (sizeShiftS - 2)) + (us >> 2);
            uint32_t address = polygon->textureAddr + (tile * 4 + (ut & 3u));
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            uint8_t index = (*data >> ((us & 3u) << 1)) & 0x03;

            // Get the palette, using the base for the tile stored in slot 1
            address = 0x20000 + ((polygon->textureAddr & 0x1FFFFu) >> 1) + (((polygon->textureAddr >> 17) == 2) ? 0x10000 : 0);
            uint16_t palBase = gpu3dLoad16(getTexture(address), tile * 2);
            uint8_t *palette = getPalette(polygon->paletteAddr + (palBase & 0x3FFF) * 4);
            if (!palette) return 0;

            // Return the palette color or a transparent or interpolated color based on the mode
            switch ((palBase & 0xC000) >> 14) // Interpolation mode
            {
                case 0:
                {
                    if (index == 3) return 0;
                    return rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, index * 2));
                }

                case 1:
                {
                    switch (index)
                    {
                        case 2:
                        {
                            uint32_t c1 = rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, 0));
                            uint32_t c2 = rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, 2));
                            return interpolateColor(c1, c2, 0, 1, 2);
                        }

                        case 3:
                        {
                            return 0;
                        }

                        default:
                        {
                            return rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, index * 2));
                        }
                    }
                }

                case 2:
                {
                    return rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, index * 2));
                }

                case 3:
                {
                    switch (index)
                    {
                        case 2:
                        {
                            uint32_t c1 = rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, 0));
                            uint32_t c2 = rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, 2));
                            return interpolateColor(c1, c2, 0, 3, 8);
                        }

                        case 3:
                        {
                            uint32_t c1 = rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, 0));
                            uint32_t c2 = rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, 2));
                            return interpolateColor(c1, c2, 0, 5, 8);
                        }

                        default:
                        {
                            return rgba5ToRgba6((0x1F << 15) | gpu3dLoad16(palette, index * 2));
                        }
                    }
                }
            }
        }

        case 6: // A5I3 translucent
        {
            // Get the 8-bit palette index
            uint32_t address = polygon->textureAddr + ((ut << sizeShiftS) + us);
            uint8_t *data = getTexture(address);
            if (!data) return 0;
            uint8_t index = *data;

            // Get the palette
            uint8_t *palette = basePalette;
            if (!palette) return 0;

            // Return the palette color
            uint16_t color = gpu3dLoad16(palette, (index & 0x07) * 2) & ~BIT(15);
            uint8_t alpha = index >> 3;
            return rgba5ToRgba6((alpha << 15) | color);
        }

        default: // Direct color
        {
            // Get the color data
            uint8_t *data = getTexture(polygon->textureAddr);
            if (!data) return 0;

            // Return the direct color
            uint16_t color = gpu3dLoad16(data, ((ut << sizeShiftS) + us) * 2);
            uint8_t alpha = (color & BIT(15)) ? 0x1F : 0;
            return rgba5ToRgba6((alpha << 15) | color);
        }
    }
}

void Gpu3DRenderer::drawPolygon(int line, int polygonIndex)
{
    _Polygon *polygon = &core->gpu3D.getPolygons()[polygonIndex];

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    bool profileSampled = false;
    if (profileEnabled && activeThreads == 0)
    {
        detailProfile.polygonCalls++;
        const bool translucent =
            polygon->alpha < 0x3F || polygon->textureFmt == 1 ||
            polygon->textureFmt == 6;
        if (translucent)
            detailProfile.translucentCalls++;
        else
            detailProfile.solidCalls++;

        profileSampled = profileSample64(detailSampleState, 0x830u);
        if (profileSampled)
            detailProfile.polygonSamples++;
    }
#else
    const bool profileSampled = false;
#endif

    const uint8_t textureSizeShiftS = polygon->sizeS > 0
        ? static_cast<uint8_t>(
            __builtin_ctz(static_cast<unsigned>(polygon->sizeS)))
        : 0;

    // Formats 1/2/3/4/6 use one fixed palette base for the whole polygon.
    // Resolve it once instead of indexing the palette-slot table for every
    // texel. A missing mapping makes the same formats fully transparent, so
    // the polygon can return immediately.
    uint8_t *basePalette = nullptr;
    if (polygon->textureFmt == 1 || polygon->textureFmt == 2 ||
        polygon->textureFmt == 3 || polygon->textureFmt == 4 ||
        polygon->textureFmt == 6)
    {
        basePalette = getPalette(polygon->paletteAddr);
        if (!basePalette)
            return;
    }

    // Get the polygon vertices
    Vertex *vertices[10];
    for (int i = 0; i < polygon->size; i++)
        vertices[i] = &polygon->vertices[i];

    // Unclipped quad strip polygons have their vertices crossed, so uncross them
    if (polygon->crossed)
        SWAP(vertices[2], vertices[3]);

    // Find the starting (top) vertex
    int start = 0;
    for (int i = 0; i < polygon->size; i++)
    {
        if (vertices[start]->y > vertices[i]->y)
            start = i;
    }

    // Set the starting edges. Avoid variable modulo in this per-polygon,
    // per-scanline walk; polygon size is tiny and explicit wrap is exact.
    int nextVertex = start + 1;
    if (nextVertex >= polygon->size) nextVertex = 0;
    int prevVertex = start - 1;
    if (prevVertex < 0) prevVertex = polygon->size - 1;
    int v[4] = { start, nextVertex, start, prevVertex };

    // Follow the vertices forwards to the first intersecting edge
    while (vertices[v[1]]->y <= line)
    {
        v[0] = v[1];
        v[1]++;
        if (v[1] >= polygon->size) v[1] = 0;
        if (v[0] == start) break; // Full loop, no intersection
    }

    // Follow the vertices backwards to the first intersecting edge
    while (vertices[v[3]]->y <= line)
    {
        v[2] = v[3];
        v[3]--;
        if (v[3] < 0) v[3] = polygon->size - 1;
        if (v[2] == start) break; // Full loop, no intersection
    }

    // Swap the edges depending on polygon orientation
    if (polygon->clockwise)
    {
        SWAP(v[0], v[2]);
        SWAP(v[1], v[3]);
    }

    // Rearrange the vertices so the lower Y values come first
    if (vertices[v[0]]->y > vertices[v[1]]->y) SWAP(v[0], v[1]);
    if (vertices[v[2]]->y > vertices[v[3]]->y) SWAP(v[2], v[3]);

    uint32_t x1, x2, x3, x4;
    uint32_t x1a = 0x3F, x2a = 0x3F, x3a = 0x3F, x4a = 0x3F;

    // Calculate the left edge bounds
    if (vertices[v[0]]->y == vertices[v[1]]->y) // Horizontal
    {
        x1 = vertices[v[0]]->x;
        x2 = vertices[v[1]]->x;

        // Rearrange the vertices so the lower X values come first
        if (vertices[v[0]]->x > vertices[v[1]]->x)
        {
            SWAP(v[0], v[1]);
            SWAP(x1, x2);
        }
    }
    else if (abs(vertices[v[1]]->x - vertices[v[0]]->x) > vertices[v[1]]->y - vertices[v[0]]->y) // X-major
    {
        // Interpolate with an extra bit of precision so the result can be rounded
        x1 = interpolateLinear(vertices[v[0]]->x << 1, vertices[v[1]]->x << 1, vertices[v[0]]->y, line,     vertices[v[1]]->y) + 1;
        x2 = interpolateLinear(vertices[v[0]]->x << 1, vertices[v[1]]->x << 1, vertices[v[0]]->y, line + 1, vertices[v[1]]->y) + 1;

        bool negative = (vertices[v[0]]->x > vertices[v[1]]->x);

        // Rearrange the vertices so the lower X values come first
        if (negative)
        {
            SWAP(v[0], v[1]);
            SWAP(x1, x2);
        }

        // Calculate the edge alpha values if anti-aliasing is enabled
        if (disp3DCnt & BIT(4))
        {
            x1a = interpolateLinear(vertices[v[0]]->y << 6, vertices[v[1]]->y << 6, vertices[v[0]]->x << 1, x1,     vertices[v[1]]->x << 1) & 0x3F;
            x2a = interpolateLinear(vertices[v[0]]->y << 6, vertices[v[1]]->y << 6, vertices[v[0]]->x << 1, x2 - 2, vertices[v[1]]->x << 1) & 0x3F;

            if (negative)
            {
                x1a = 0x3F - x1a;
                x2a = 0x3F - x2a;
            }
        }

        x1 >>= 1;
        x2 >>= 1;
    }
    else // Y-major
    {
        // Interpolate with extra precision for edge alpha calculation
        // Note that negative Y-major edges seem to be interpolated in reverse from positive ones
        if (vertices[v[0]]->x > vertices[v[1]]->x)
            x1 = interpolateLinRev(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6, vertices[v[0]]->y, line, vertices[v[1]]->y) - 1;
        else
            x1 = interpolateLinear(vertices[v[0]]->x << 6, vertices[v[1]]->x << 6, vertices[v[0]]->y, line, vertices[v[1]]->y);

        // Set the edge alpha values if anti-aliasing is enabled
        if (disp3DCnt & BIT(4))
        {
            if (abs(vertices[v[1]]->x - vertices[v[0]]->x) == vertices[v[1]]->y - vertices[v[0]]->y)
                x2a = x1a = 0x20;
            else
                x2a = x1a = 0x3F - (x1 & 0x3F);
        }

        x2 = x1 >>= 6;
    }

    // Calculate the right edge bounds
    if (vertices[v[2]]->y == vertices[v[3]]->y) // Horizontal
    {
        x3 = vertices[v[2]]->x;
        x4 = vertices[v[3]]->x;

        // Rearrange the vertices so the lower X values come first
        if (vertices[v[2]]->x > vertices[v[3]]->x)
        {
            SWAP(v[2], v[3]);
            SWAP(x3, x4);
        }
    }
    else if (vertices[v[2]]->x == vertices[v[3]]->x) // Vertical
    {
        // Unless a line polygon, vertical right edges have their X reduced by 1
        x3 = vertices[v[2]]->x;
        if ((vertices[v[0]]->x != vertices[v[1]]->x || vertices[v[0]]->x != vertices[v[2]]->x) && x3 > 0) x3--;
        x4 = x3;
    }
    else if (abs(vertices[v[3]]->x - vertices[v[2]]->x) > vertices[v[3]]->y - vertices[v[2]]->y) // X-major
    {
        // Interpolate with an extra bit of precision so the result can be rounded
        x3 = interpolateLinear(vertices[v[2]]->x << 1, vertices[v[3]]->x << 1, vertices[v[2]]->y, line,     vertices[v[3]]->y) + 1;
        x4 = interpolateLinear(vertices[v[2]]->x << 1, vertices[v[3]]->x << 1, vertices[v[2]]->y, line + 1, vertices[v[3]]->y) + 1;

        bool negative = (vertices[v[2]]->x > vertices[v[3]]->x);

        // Rearrange the vertices so the lower X values come first
        if (negative)
        {
            SWAP(v[2], v[3]);
            SWAP(x3, x4);
        }

        // Calculate the edge alpha values if anti-aliasing is enabled
        if (disp3DCnt & BIT(4))
        {
            x3a = interpolateLinear(vertices[v[2]]->y << 6, vertices[v[3]]->y << 6, vertices[v[2]]->x << 1, x3,     vertices[v[3]]->x << 1) & 0x3F;
            x4a = interpolateLinear(vertices[v[2]]->y << 6, vertices[v[3]]->y << 6, vertices[v[2]]->x << 1, x4 - 2, vertices[v[3]]->x << 1) & 0x3F;

            if (!negative)
            {
                x3a = 0x3F - x3a;
                x4a = 0x3F - x4a;
            }
        }

        x3 >>= 1;
        x4 >>= 1;
    }
    else // Y-major
    {
        // Interpolate with extra precision for edge alpha calculation
        // Note that negative Y-major edges seem to be interpolated in reverse from positive ones
        if (vertices[v[2]]->x > vertices[v[3]]->x)
            x3 = interpolateLinRev(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6, vertices[v[2]]->y, line, vertices[v[3]]->y) - 1;
        else
            x3 = interpolateLinear(vertices[v[2]]->x << 6, vertices[v[3]]->x << 6, vertices[v[2]]->y, line, vertices[v[3]]->y);

        // Set the edge alpha values if anti-aliasing is enabled
        if (disp3DCnt & BIT(4))
        {
            if (abs(vertices[v[3]]->x - vertices[v[2]]->x) == vertices[v[3]]->y - vertices[v[2]]->y)
                x4a = x3a = 0x20;
            else
                x4a = x3a = x3 & 0x3F;
        }

        x4 = x3 >>= 6;
    }

    // Reduce the bounds so they don't overlap
    if (x2 > x1) x2--;
    if (x4 > x3) x4--;

    // Handle crossed edges; these become "dotted" if their orientation is wrong
    if (x2 > x3) { x2 = x3;                                     }
    if (x1 > x2) { x2 = x1; x1a = 0x3F - x1a; x2a = 0x3F - x2a; }
    if (x2 > x3) { x3 = x2;                                     }
    if (x3 > x4) { x3 = x4; x3a = 0x3F - x3a; x4a = 0x3F - x4a; }

    // Swap the edges so the leftmost one comes first
    if (x1 > x4)
    {
        SWAP(v[0], v[2]);
        SWAP(v[1], v[3]);
        SWAP(x1, x3);
        SWAP(x2, x4);
        SWAP(x1a, x3a);
        SWAP(x2a, x4a);
    }

    uint32_t lx1, lx, lx2;
    uint32_t rx1, rx, rx2;
    bool hideLeft, hideRight;

    // Choose between X and Y coordinates for interpolation on the left (whichever is more precise)
    if (abs(vertices[v[1]]->x - vertices[v[0]]->x) > abs(vertices[v[1]]->y - vertices[v[0]]->y)) // X-major
    {
        lx1 = vertices[v[0]]->x;
        lx = x1;
        lx2 = vertices[v[1]]->x;
        hideLeft = (vertices[v[0]]->y < vertices[v[1]]->y);
    }
    else // Y-major
    {
        lx1 = vertices[v[0]]->y;
        lx = line;
        lx2 = vertices[v[1]]->y;
        hideLeft = false;
    }

    // Choose between X and Y coordinates for interpolation on the right (whichever is more precise)
    if (abs(vertices[v[3]]->x - vertices[v[2]]->x) > abs(vertices[v[3]]->y - vertices[v[2]]->y)) // X-major
    {
        rx1 = vertices[v[2]]->x;
        rx = x4;
        rx2 = vertices[v[3]]->x;
        hideRight = (vertices[v[2]]->y > vertices[v[3]]->y);
    }
    else // Y-major
    {
        rx1 = vertices[v[2]]->y;
        rx = line;
        rx2 = vertices[v[3]]->y;
        hideRight = (vertices[v[2]]->x != vertices[v[3]]->x);
    }

    // Apply W-shift to reduce (or expand) W values to 16 bits
    uint32_t ws[4];
    if (polygon->wShift >= 0)
    {
        for (int i = 0; i < 4; i++)
            ws[i] = vertices[v[i]]->w >> polygon->wShift;
    }
    else
    {
        for (int i = 0; i < 4; i++)
            ws[i] = vertices[v[i]]->w << -polygon->wShift;
    }

    // Calculate the Z values of the polygon edges on the current line
    uint32_t z1 = interpolateLinear(vertices[v[0]]->z, vertices[v[1]]->z, lx1, lx, lx2);
    uint32_t z2 = interpolateLinear(vertices[v[2]]->z, vertices[v[3]]->z, rx1, rx, rx2);

    // All edge attributes use the same perspective factor for a given side
    // of the polygon. Build one exact interpolator per edge so W, RGB and ST
    // share that division instead of recomputing it six times.
    struct EdgeInterpolationState
    {
        uint32_t xStart, xPos, xEnd;
        uint32_t factor;
        bool atStart, atEnd, linear;
    };

    auto makeEdgeInterpolator = [&](uint32_t xStart, uint32_t xPos,
        uint32_t xEnd, uint32_t weight1, uint32_t weight2)
        -> EdgeInterpolationState
    {
        EdgeInterpolationState state = {};
        state.xStart = xStart;
        state.xPos = xPos;
        state.xEnd = xEnd;
        state.atStart = xPos <= xStart;
        state.atEnd = xPos >= xEnd;
        state.linear = (weight1 == weight2 && !(weight1 & 0x00FE));

        if (!state.atStart && !state.atEnd && !state.linear)
        {
            uint32_t weight1a = weight1 >> 1;
            if ((weight1 & 1) && !(weight2 & 1))
                weight1a++;
            uint32_t half1 = weight1 >> 1;
            uint32_t half2 = weight2 >> 1;
            uint32_t left = half1 * (xPos - xStart);
            uint32_t right = half2 * (xEnd - xPos);
            state.factor = (left << 9) /
                (right + weight1a * (xPos - xStart));
        }

        return state;
    };

    const EdgeInterpolationState leftEdge = makeEdgeInterpolator(
        lx1, lx, lx2, ws[0], ws[1]);
    const EdgeInterpolationState rightEdge = makeEdgeInterpolator(
        rx1, rx, rx2, ws[2], ws[3]);

    auto interpolateEdgeValue = [&](const EdgeInterpolationState &edge,
        uint32_t v1, uint32_t v2) -> uint32_t
    {
        if (edge.atStart) return v1;
        if (edge.atEnd) return v2;
        if (edge.linear)
            return interpolateLinear(v1, v2,
                edge.xStart, edge.xPos, edge.xEnd);

        if (v1 <= v2)
            return v1 + (((v2 - v1) * edge.factor) >> 9);
        return v2 + (((v1 - v2) * (0x200u - edge.factor)) >> 9);
    };

    // Calculate the W values of the polygon edges on the current line
    uint32_t w1 = interpolateEdgeValue(leftEdge, ws[0], ws[1]);
    uint32_t w2 = interpolateEdgeValue(rightEdge, ws[2], ws[3]);

    // Interpolate the vertex color of the polygon edges on the current line
    // The color values are expanded to 9 bits during interpolation for extra precision
    uint32_t r1 = interpolateEdgeValue(leftEdge,
        ((vertices[v[0]]->color >> 0) & 0x3F) << 3,
        ((vertices[v[1]]->color >> 0) & 0x3F) << 3);
    uint32_t g1 = interpolateEdgeValue(leftEdge,
        ((vertices[v[0]]->color >> 6) & 0x3F) << 3,
        ((vertices[v[1]]->color >> 6) & 0x3F) << 3);
    uint32_t b1 = interpolateEdgeValue(leftEdge,
        ((vertices[v[0]]->color >> 12) & 0x3F) << 3,
        ((vertices[v[1]]->color >> 12) & 0x3F) << 3);
    uint32_t r2 = interpolateEdgeValue(rightEdge,
        ((vertices[v[2]]->color >> 0) & 0x3F) << 3,
        ((vertices[v[3]]->color >> 0) & 0x3F) << 3);
    uint32_t g2 = interpolateEdgeValue(rightEdge,
        ((vertices[v[2]]->color >> 6) & 0x3F) << 3,
        ((vertices[v[3]]->color >> 6) & 0x3F) << 3);
    uint32_t b2 = interpolateEdgeValue(rightEdge,
        ((vertices[v[2]]->color >> 12) & 0x3F) << 3,
        ((vertices[v[3]]->color >> 12) & 0x3F) << 3);

    // Interpolate the texture coordinates of the polygon edges on the current line
    // Interpolation is unsigned, so temporarily convert the signed values to unsigned
    int32_t s1 = static_cast<int32_t>(interpolateEdgeValue(leftEdge,
        static_cast<uint32_t>(static_cast<int32_t>(vertices[v[0]]->s) + 0xFFFF),
        static_cast<uint32_t>(static_cast<int32_t>(vertices[v[1]]->s) + 0xFFFF))) - 0xFFFF;
    int32_t t1 = static_cast<int32_t>(interpolateEdgeValue(leftEdge,
        static_cast<uint32_t>(static_cast<int32_t>(vertices[v[0]]->t) + 0xFFFF),
        static_cast<uint32_t>(static_cast<int32_t>(vertices[v[1]]->t) + 0xFFFF))) - 0xFFFF;
    int32_t s2 = static_cast<int32_t>(interpolateEdgeValue(rightEdge,
        static_cast<uint32_t>(static_cast<int32_t>(vertices[v[2]]->s) + 0xFFFF),
        static_cast<uint32_t>(static_cast<int32_t>(vertices[v[3]]->s) + 0xFFFF))) - 0xFFFF;
    int32_t t2 = static_cast<int32_t>(interpolateEdgeValue(rightEdge,
        static_cast<uint32_t>(static_cast<int32_t>(vertices[v[2]]->t) + 0xFFFF),
        static_cast<uint32_t>(static_cast<int32_t>(vertices[v[3]]->t) + 0xFFFF))) - 0xFFFF;

    // Keep track of shadow mask polygons
    if (polygon->mode == 3 && polygon->id == 0) // Shadow mask polygon
    {
        // Clear the stencil buffer at the start of a shadow mask polygon group
        if (!stencilClear[line])
        {
            memset(&stencilBuffer[line * 256], 0, 256 * sizeof(uint8_t));
            stencilClear[line] = true;
        }
    }
    else
    {
        // End a shadow mask polygon group
        stencilClear[line] = false;
    }

    // Increment the right bound not only for drawing, but for interpolation across the scanline as well
    // This seems to give results accurate to hardware
    uint32_t x1e = x1, x4e = ++x4;

    // Set special bounds that hide some edges for opaque pixels with no edge effects
    if (polygon->alpha != 0 && !(disp3DCnt & (BIT(4) | BIT(5))))
    {
        if (hideLeft)  x1e = x2 + 1;
        if (hideRight) x4e = x3;
        if (!(hideLeft && hideRight) && x4e <= x1e)
            x4e = x1e + 1;
    }

    // Because edge traversal doesn't consider equal values to be intersecting, it skips horizontal edges
    // Instead, simply consider the entire span across the top and bottom of a polygon to be an edge
    bool horizontal = (line == polygonTop[polygonIndex] || line == polygonBot[polygonIndex] - 1);

    // interpolateFill() uses the same perspective factor for every
    // attribute at a given X. Compute that expensive division once per pixel
    // and reuse it for W/color/texture coordinates. Preserve the old linear
    // path verbatim because its per-channel integer rounding is observable.
    const bool linearFill = (w1 == w2 && !(w1 & 0x007F));

    // Draw a line segment
    for (uint32_t x = x1; x < x4; x++)
    {
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
        if (profileSampled)
        {
            detailProfile.sampledPixels++;
            if (linearFill)
                detailProfile.linearPixels++;
            else
                detailProfile.perspectivePixels++;
        }
#endif
        uint32_t fillFactor = 0;
        bool fillFactorReady = false;

        auto fillValue = [&](uint32_t v1, uint32_t v2) -> uint32_t
        {
            if (x <= x1) return v1;
            if (x >= x4) return v2;

            if (linearFill)
                return interpolateLinear(v1, v2, x1, x, x4);

            if (!fillFactorReady)
            {
                const uint32_t left = w1 * (x - x1);
                const uint32_t right = w2 * (x4 - x);
                fillFactor = (left << 8) / (right + left);
                fillFactorReady = true;
            }

            if (v1 <= v2)
                return v1 + (((v2 - v1) * fillFactor) >> 8);
            return v2 + (((v1 - v2) * (0x100u - fillFactor)) >> 8);
        };
        // Skip the polygon interior for wireframe polygons
        if (!horizontal && polygon->alpha == 0 && x == x2 + 1 && x3 > x2)
            x = x3;

        // Invalid viewports can cause out-of-bounds vertices, so only draw within bounds
        if (x >= 256) break;

        bool layer = 0;
        int i = line * 256 + x;

        // Calculate the depth value of the current pixel
        int32_t depth;
        if (polygon->wBuffer)
        {
            depth = fillValue(w1, w2);
            if (polygon->wShift > 0)
                depth <<= polygon->wShift;
            else if (polygon->wShift < 0)
                depth >>= -polygon->wShift;
        }
        else
        {
            depth = interpolateLinear(z1, z2, x1, x, x4);
        }

        // Depth test the pixel on the front layer, and on the back layer if under an anti-aliased edge
        bool depthPass[2];
        if (polygon->depthTestEqual)
        {
            uint32_t margin = (polygon->wBuffer ? 0xFF : 0x200);
            depthPass[0] = (depthBuffer[0][i] >= depth - margin && depthBuffer[0][i] <= depth + margin);
            depthPass[1] = (disp3DCnt & BIT(4)) && (attribBuffer[0][i] & BIT(14)) &&
                (depthBuffer[1][i] >= depth - margin && depthBuffer[1][i] <= depth + margin);
        }
        else
        {
            depthPass[0] = (depthBuffer[0][i] > depth);
            depthPass[1] = (disp3DCnt & BIT(4)) && (attribBuffer[0][i] & BIT(14)) && (depthBuffer[1][i] > depth);
        }
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
        if (profileSampled)
        {
            detailProfile.depthTests++;
            if (!depthPass[0] && !depthPass[1])
                detailProfile.depthFailBoth++;
            if (polygon->mode == 3)
                detailProfile.shadowPixels++;
        }
#endif

        // Check if the pixel should be drawn
        if (polygon->mode == 3) // Shadow
        {
            if (polygon->id == 0) // Mask
            {
                // Don't render shadow mask pixels, but set a stencil buffer bit for ones that fail the depth test
                if (!depthPass[0]) stencilBuffer[i] |= BIT(0);
                if (!depthPass[1]) stencilBuffer[i] |= BIT(1);
                continue;
            }
            else if (!depthPass[0] || !(stencilBuffer[i] & BIT(0)) || (attribBuffer[0][i] & 0x3F) == polygon->id)
            {
                // Only render non-mask shadow pixels if the stencil bit is set and the old pixel's polygon ID differs
                if (!depthPass[1] || !(stencilBuffer[i] & BIT(1)) || (attribBuffer[1][i] & 0x3F) == polygon->id)
                    continue;

                // Draw the pixel on the back layer
                layer = 1;
            }
        }
        else if (!depthPass[0])
        {
            // Only render normal pixels if the depth test passes
            if (!depthPass[1])
                continue;

            // Draw the pixel on the back layer
            layer = 1;
        }

        // Interpolate the vertex color at the current pixel
        uint32_t r = fillValue(r1, r2) >> 3;
        uint32_t g = fillValue(g1, g2) >> 3;
        uint32_t b = fillValue(b1, b2) >> 3;
        uint32_t color = ((polygon->alpha ? polygon->alpha : 0x3F) << 18) | (b << 12) | (g << 6) | r;

        // Blend the texture with the vertex color
        if (polygon->textureFmt != 0)
        {
            // Interpolate the texture coordinates at the current pixel
            int s = static_cast<int>(fillValue(
                static_cast<uint32_t>(s1 + 0xFFFF),
                static_cast<uint32_t>(s2 + 0xFFFF))) - 0xFFFF;
            int t = static_cast<int>(fillValue(
                static_cast<uint32_t>(t1 + 0xFFFF),
                static_cast<uint32_t>(t2 + 0xFFFF))) - 0xFFFF;

            // Read a texel from the texture
            uint32_t texel = readTexture(polygon, s >> 4, t >> 4, basePalette,
                textureSizeShiftS, profileSampled);

            // Apply texture blending
            // These formulas are a translation of the pseudocode from GBATEK to C++
            switch (polygon->mode)
            {
                case 0: // Modulation
                {
                    uint8_t r = ((((texel >>  0) & 0x3F) + 1) * (((color >>  0) & 0x3F) + 1) - 1) / 64;
                    uint8_t g = ((((texel >>  6) & 0x3F) + 1) * (((color >>  6) & 0x3F) + 1) - 1) / 64;
                    uint8_t b = ((((texel >> 12) & 0x3F) + 1) * (((color >> 12) & 0x3F) + 1) - 1) / 64;
                    uint8_t a = ((((texel >> 18) & 0x3F) + 1) * (((color >> 18) & 0x3F) + 1) - 1) / 64;
                    color = (a << 18) | (b << 12) | (g << 6) | r;
                    break;
                }

                case 1: // Decal
                case 3: // Shadow
                {
                    uint8_t at = ((texel >> 18) & 0x3F);
                    uint8_t r = (((texel >>  0) & 0x3F) * at + ((color >>  0) & 0x3F) * (63 - at)) / 64;
                    uint8_t g = (((texel >>  6) & 0x3F) * at + ((color >>  6) & 0x3F) * (63 - at)) / 64;
                    uint8_t b = (((texel >> 12) & 0x3F) * at + ((color >> 12) & 0x3F) * (63 - at)) / 64;
                    uint8_t a =  ((color >> 18) & 0x3F);
                    color = (a << 18) | (b << 12) | (g << 6) | r;
                    break;
                }

                case 2: // Toon/Highlight
                {
                    uint32_t toon = rgba5ToRgba6(toonTable[(color & 0x3F) / 2]);
                    uint8_t r, g, b;

                    if (disp3DCnt & BIT(1)) // Highlight
                    {
                        r = ((((texel >>  0) & 0x3F) + 1) * (((color >>  0) & 0x3F) + 1) - 1) / 64;
                        g = ((((texel >>  6) & 0x3F) + 1) * (((color >>  6) & 0x3F) + 1) - 1) / 64;
                        b = ((((texel >> 12) & 0x3F) + 1) * (((color >> 12) & 0x3F) + 1) - 1) / 64;
                        r += ((toon >>  0) & 0x3F); if (r > 63) r = 63;
                        g += ((toon >>  6) & 0x3F); if (g > 63) g = 63;
                        b += ((toon >> 12) & 0x3F); if (b > 63) b = 63;
                    }
                    else // Toon
                    {
                        r = ((((texel >>  0) & 0x3F) + 1) * (((toon >>  0) & 0x3F) + 1) - 1) / 64;
                        g = ((((texel >>  6) & 0x3F) + 1) * (((toon >>  6) & 0x3F) + 1) - 1) / 64;
                        b = ((((texel >> 12) & 0x3F) + 1) * (((toon >> 12) & 0x3F) + 1) - 1) / 64;
                    }

                    uint8_t a = ((((texel >> 18) & 0x3F) + 1) * (((color >> 18) & 0x3F) + 1) - 1) / 64;
                    color = (a << 18) | (b << 12) | (g << 6) | r;
                    break;
                }
            }
        }
        else if (polygon->mode == 2) // Toon/Highlight (no texture)
        {
            uint32_t toon = rgba5ToRgba6(toonTable[(color & 0x3F) / 2]);
            uint8_t r, g, b;

            if (disp3DCnt & BIT(1)) // Highlight
            {
                r = ((color >>  0) & 0x3F) + ((toon >>  0) & 0x3F); if (r > 63) r = 63;
                g = ((color >>  6) & 0x3F) + ((toon >>  6) & 0x3F); if (g > 63) g = 63;
                b = ((color >> 12) & 0x3F) + ((toon >> 12) & 0x3F); if (b > 63) b = 63;
            }
            else // Toon
            {
                r = ((toon >>  0) & 0x3F);
                g = ((toon >>  6) & 0x3F);
                b = ((toon >> 12) & 0x3F);
            }

            color = (color & 0xFC0000) | (b << 12) | (g << 6) | r;
        }

        // Skip fully transparent pixels, and hidden edge pixels if the pixel is opaque or blending is disabled
        if (!(color & 0xFC0000) ||
            ((x < x1e || x >= x4e) &&
                ((color >> 18) == 0x3F || !(disp3DCnt & BIT(3)))))
        {
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
            if (profileSampled)
                detailProfile.alphaRejects++;
#endif
            continue;
        }

        // Draw a pixel, marked with an extra bit as an indicator for 2D blending
        if ((color >> 18) == 0x3F) // Opaque
        {
            uint8_t edgeAlpha = 0x3F;
            bool edge = (x <= x2 || x >= x3 || horizontal);
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
            if (profileSampled)
            {
                detailProfile.opaqueWrites++;
                if (edge)
                    detailProfile.edgePixels++;
            }
#endif

            // Push the previous pixel to the back layer if drawing a front anti-aliased edge pixel
            if ((disp3DCnt & BIT(4)) && layer == 0 && edge)
            {
                framebuffer[1][i]  = framebuffer[0][i];
                depthBuffer[1][i]  = depthBuffer[0][i];
                attribBuffer[1][i] = attribBuffer[0][i];

                // Set the pixel transparency for anti-aliasing
                if (x <= x2)
                    edgeAlpha = interpolateLinear(x1a, x2a, x1, x, x2);
                else if (x >= x3)
                    edgeAlpha = interpolateLinear(x3a, x4a, x3, x, x4);
            }

            framebuffer[layer][i] = BIT(26) | color;
            depthBuffer[layer][i] = depth;
            attribBuffer[layer][i] = (attribBuffer[layer][i] & 0x0FC0) | (edgeAlpha << 15) |
                (edge << 14) | (polygon->fog << 13) | polygon->id;
        }
        else if (!(attribBuffer[layer][i] & BIT(12)) || ((attribBuffer[layer][i] >> 6) & 0x3F) != polygon->id) // Transparent
        {
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
            if (profileSampled)
            {
                detailProfile.transparentWrites++;
                if ((disp3DCnt & BIT(3)) &&
                    (framebuffer[layer][i] & 0xFC0000))
                    detailProfile.blendOps++;
            }
#endif
            // Transparent pixels are only drawn if the old pixel isn't transparent or the polygon ID differs
            framebuffer[layer][i] = BIT(26) | (((disp3DCnt & BIT(3)) && (framebuffer[layer][i] & 0xFC0000)) ?
                interpolateColor(framebuffer[layer][i], color, 0, color >> 18, 63) : color);
            if (polygon->transNewDepth) depthBuffer[layer][i] = depth;
            attribBuffer[layer][i] = (attribBuffer[layer][i] & 0x1FE03F) | BIT(12) | (polygon->id << 6);

            // Blend with the back layer as well if drawing over a front anti-aliased edge pixel
            if ((disp3DCnt & BIT(4)) && layer == 0 && (attribBuffer[0][i] & BIT(14)))
            {
                framebuffer[1][i] = BIT(26) | (((disp3DCnt & BIT(3)) && (framebuffer[1][i] & 0xFC0000)) ?
                    interpolateColor(framebuffer[1][i], color, 0, color >> 18, 63) : color);
                if (polygon->transNewDepth) depthBuffer[1][i] = depth;
                attribBuffer[1][i] = (attribBuffer[1][i] & 0x1FE03F) | BIT(12) | (polygon->id << 6);
            }
        }
    }
}

void Gpu3DRenderer::writeDisp3DCnt(uint16_t mask, uint16_t value)
{
    // If any of the error bits are set, acknowledge the errors by clearing them
    if (value & BIT(12)) disp3DCnt &= ~BIT(12);
    if (value & BIT(13)) disp3DCnt &= ~BIT(13);

    // Write to the DISP3DCNT register and invalidate the 3D if a parameter changed
    mask &= 0x4FFF;
    if ((value & mask) == (disp3DCnt & mask)) return;
    disp3DCnt = (disp3DCnt & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeEdgeColor(int index, uint16_t mask, uint16_t value)
{
    // Write to one of the EDGE_COLOR registers and invalidate the 3D if a parameter changed
    mask &= 0x7FFF;
    if ((value & mask) == (edgeColor[index] & mask)) return;
    edgeColor[index] = (edgeColor[index] & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeClearColor(uint32_t mask, uint32_t value)
{
    // Write to the CLEAR_COLOR register and invalidate the 3D if a parameter changed
    mask &= 0x3F1FFFFF;
    if ((value & mask) == (clearColor & mask)) return;
    clearColor = (clearColor & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeClearDepth(uint16_t mask, uint16_t value)
{
    // Write to the CLEAR_DEPTH register and invalidate the 3D if a parameter changed
    mask &= 0x7FFF;
    if ((value & mask) == (clearDepth & mask)) return;
    clearDepth = (clearDepth & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeToonTable(int index, uint16_t mask, uint16_t value)
{
    // Write to one of the TOON_TABLE registers and invalidate the 3D if a parameter changed
    mask &= 0x7FFF;
    if ((value & mask) == (toonTable[index] & mask)) return;
    toonTable[index] = (toonTable[index] & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeFogColor(uint32_t mask, uint32_t value)
{
    // Write to the FOG_COLOR register and invalidate the 3D if a parameter changed
    mask &= 0x001F7FFF;
    if ((value & mask) == (fogColor & mask)) return;
    fogColor = (fogColor & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeFogOffset(uint16_t mask, uint16_t value)
{
    // Write to the FOG_OFFSET register and invalidate the 3D if a parameter changed
    mask &= 0x7FFF;
    if ((value & mask) == (fogOffset & mask)) return;
    fogOffset = (fogOffset & ~mask) | (value & mask);
    core->gpu.invalidate3D();
}

void Gpu3DRenderer::writeFogTable(int index, uint8_t value)
{
    // Write to one of the FOG_TABLE registers and invalidate the 3D if a parameter changed
    if ((value & 0x7F) == (fogTable[index] & 0x7F)) return;
    fogTable[index] = value & 0x7F;
    core->gpu.invalidate3D();
}

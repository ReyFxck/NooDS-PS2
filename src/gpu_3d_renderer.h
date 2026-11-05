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

#ifndef GPU_3D_RENDERER_H
#define GPU_3D_RENDERER_H

#include <atomic>
#include <cstdint>
#include <thread>

class Core;
struct Vertex;
struct _Polygon;

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
struct Gpu3DDetailProfile
{
    uint64_t frames = 0;
    uint64_t scanlines = 0;
    uint64_t threadedFrames = 0;
    uint64_t polygonCalls = 0;
    uint64_t solidCalls = 0;
    uint64_t translucentCalls = 0;
    uint64_t polygonSamples = 0;
    uint64_t finishSamples = 0;
    uint64_t sampledPixels = 0;
    uint64_t linearPixels = 0;
    uint64_t perspectivePixels = 0;
    uint64_t depthTests = 0;
    uint64_t depthFailBoth = 0;
    uint64_t shadowPixels = 0;
    uint64_t textureSamples = 0;
    uint64_t textureFormat[8] = {};
    uint64_t alphaRejects = 0;
    uint64_t opaqueWrites = 0;
    uint64_t transparentWrites = 0;
    uint64_t blendOps = 0;
    uint64_t fogPixels = 0;
    uint64_t edgePixels = 0;
};
#endif

class Gpu3DRenderer
{
    public:
        Gpu3DRenderer(Core *core);
        ~Gpu3DRenderer();

        void drawScanline(int line);

        uint32_t *getLine(int line);

#ifdef NOODS_PROFILE
        void setProfileEnabled(bool enabled) { profileEnabled = enabled; }
        void resetProfile()
        {
#if NOODS_PROFILE_LEVEL >= 2
            detailProfile = Gpu3DDetailProfile();
            detailSampleState = 0;
            finishSampleState = 0;
#endif
        }
#if NOODS_PROFILE_LEVEL >= 2
        const Gpu3DDetailProfile &getDetailProfile() const
        {
            return detailProfile;
        }
#endif
#endif

        uint16_t readDisp3DCnt() { return disp3DCnt; }

        void writeDisp3DCnt(uint16_t mask, uint16_t value);
        void writeEdgeColor(int index, uint16_t mask, uint16_t value);
        void writeClearColor(uint32_t mask, uint32_t value);
        void writeClearDepth(uint16_t mask, uint16_t value);
        void writeFogColor(uint32_t mask, uint32_t value);
        void writeFogOffset(uint16_t mask, uint16_t value);
        void writeFogTable(int index, uint8_t value);
        void writeToonTable(int index, uint16_t mask, uint16_t value);

    private:
        Core *core;
        // Stable addresses of Memory's texture/palette slot arrays. VRAM
        // remaps update their entries in place, so the renderer can avoid
        // rediscovering the arrays on every texel lookup.
        uint8_t **textureSlots = nullptr;
        uint8_t **paletteSlots = nullptr;

#ifdef NOODS_PROFILE
        bool profileEnabled = false;
#if NOODS_PROFILE_LEVEL >= 2
        Gpu3DDetailProfile detailProfile;
        uint32_t detailSampleState = 0;
        uint32_t finishSampleState = 0;
#endif
#endif

        uint32_t framebuffer[2][256 * 192] = {};
        int32_t depthBuffer[2][256 * 192] = {};
        uint32_t attribBuffer[2][256 * 192] = {};
        uint8_t stencilBuffer[256 * 192] = {};
        bool stencilClear[256] = {};

        int polygonTop[2048] = {};
        int polygonBot[2048] = {};
        // 2048 polygon bits per scanline. Built once at line 0 so the
        // renderer doesn't rescan every polygon for all 192 scanlines.
        uint32_t polygonLineMask[192][64] = {};

        int activeThreads = 0;
        std::thread *threads[3] = {};
        std::atomic<int> ready[192];

        uint16_t disp3DCnt = 0;
        uint16_t edgeColor[8] = {};
        uint32_t clearColor = 0;
        uint16_t clearDepth = 0;
        uint32_t fogColor = 0;
        uint16_t fogOffset = 0;
        uint8_t fogTable[32] = {};
        uint16_t toonTable[32] = {};

        static uint32_t rgba5ToRgba6(uint32_t color);

        void drawThreaded(int thread);
        void drawScanline1(int line);
        void finishScanline(int line);

        FORCE_INLINE uint8_t *getTexture(uint32_t address);
        FORCE_INLINE uint8_t *getPalette(uint32_t address);

        static uint32_t interpolateLinear(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2);
        static uint32_t interpolateLinRev(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2);
        static uint32_t interpolateFill(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2, uint32_t w1, uint32_t w2);
        static uint32_t interpolateEdge(uint32_t v1, uint32_t v2, uint32_t x1, uint32_t x, uint32_t x2, uint32_t w1, uint32_t w2);
        static uint32_t interpolateColor(uint32_t c1, uint32_t c2, uint32_t x1, uint32_t x, uint32_t x2);

        uint32_t readTexture(_Polygon *polygon, int s, int t,
            uint8_t *basePalette, bool profileSampled = false);
        void drawPolygon(int line, int polygonIndex);
};

#endif // GPU_3D_RENDERER_H

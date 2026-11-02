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

#ifndef GPU_2D_H
#define GPU_2D_H

#include <cstdint>
#ifdef NOODS_PROFILE
#include <ctime>
#endif

class Core;

#ifdef NOODS_PROFILE
enum Gpu2DProfilePhase : uint8_t
{
    GPU2D_PROFILE_CLEAR = 0,
    GPU2D_PROFILE_OBJECTS,
    GPU2D_PROFILE_TEXT,
    GPU2D_PROFILE_AFFINE,
    GPU2D_PROFILE_EXTENDED,
    GPU2D_PROFILE_LARGE,
    GPU2D_PROFILE_COMPOSE,
    GPU2D_PROFILE_OUTPUT,
    GPU2D_PROFILE_BRIGHTNESS,
    GPU2D_PROFILE_PHASE_COUNT
};

#if NOODS_PROFILE_LEVEL >= 2
enum Gpu2DExtendedMode : uint8_t
{
    GPU2D_EXTENDED_DIRECT = 0,
    GPU2D_EXTENDED_INDEXED,
    GPU2D_EXTENDED_TILED,
    GPU2D_EXTENDED_MODE_COUNT
};

struct Gpu2DExtendedCounters
{
    uint64_t pixels = 0;
    uint64_t wrapped = 0;
    uint64_t rejected = 0;
    uint64_t vramReads = 0;
    uint64_t tileCacheHits = 0;
    uint64_t directPageHits = 0;
    uint64_t paletteReads = 0;
    uint64_t transparent = 0;
    uint64_t writes = 0;
};
#endif

struct Gpu2DRunProfile
{
    uint64_t phaseCount[GPU2D_PROFILE_PHASE_COUNT] = {};
    uint64_t phaseTicks[GPU2D_PROFILE_PHASE_COUNT] = {};
    uint64_t phaseSamples[GPU2D_PROFILE_PHASE_COUNT] = {};
    uint32_t phaseSampleState[GPU2D_PROFILE_PHASE_COUNT] = {};
#if NOODS_PROFILE_LEVEL >= 2
    uint64_t extendedModeCount[GPU2D_EXTENDED_MODE_COUNT] = {};
    uint64_t extendedModeTicks[GPU2D_EXTENDED_MODE_COUNT] = {};
    uint64_t extendedSetupTicks[GPU2D_EXTENDED_MODE_COUNT] = {};
    uint64_t extendedBodyTicks[GPU2D_EXTENDED_MODE_COUNT] = {};
    uint64_t extendedModeSamples[GPU2D_EXTENDED_MODE_COUNT] = {};
    uint32_t extendedModeSampleState[GPU2D_EXTENDED_MODE_COUNT] = {};
    Gpu2DExtendedCounters extendedCounters[GPU2D_EXTENDED_MODE_COUNT] = {};
#endif
};
#endif

class Gpu2D
{
    public:
        Gpu2D(Core *core, bool engine);

        void drawGbaScanline(int line);
        void drawScanline(int line);

        uint32_t *getFramebuffer() { return framebuffer; }
        uint32_t *getRawLine()     { return layers[0];   }

#ifdef NOODS_PROFILE
        void setProfileEnabled(bool enabled) { profileEnabled = enabled; }
        void resetProfile() { runProfile = Gpu2DRunProfile(); }
        const Gpu2DRunProfile &getProfile() const { return runProfile; }
#endif

        uint32_t readDispCnt()      { return dispCnt;      }
        uint16_t readBgCnt(int bg)  { return bgCnt[bg];    }
        uint16_t readWinIn()        { return winIn;        }
        uint16_t readWinOut()       { return winOut;       }
        uint16_t readBldCnt()       { return bldCnt;       }
        uint16_t readBldAlpha()     { return bldAlpha;     }
        uint16_t readMasterBright() { return masterBright; }

        void writeDispCnt(uint32_t mask, uint32_t value);
        void writeBgCnt(int bg, uint16_t mask, uint16_t value);
        void writeBgHOfs(int bg, uint16_t mask, uint16_t value);
        void writeBgVOfs(int bg, uint16_t mask, uint16_t value);
        void writeBgPA(int bg, uint16_t mask, uint16_t value);
        void writeBgPB(int bg, uint16_t mask, uint16_t value);
        void writeBgPC(int bg, uint16_t mask, uint16_t value);
        void writeBgPD(int bg, uint16_t mask, uint16_t value);
        void writeBgX(int bg, uint32_t mask, uint32_t value);
        void writeBgY(int bg, uint32_t mask, uint32_t value);
        void writeWinH(int win, uint16_t mask, uint16_t value);
        void writeWinV(int win, uint16_t mask, uint16_t value);
        void writeWinIn(uint16_t mask, uint16_t value);
        void writeWinOut(uint16_t mask, uint16_t value);
        void writeMosaic(uint16_t mask, uint16_t value);
        void writeBldCnt(uint16_t mask, uint16_t value);
        void writeBldAlpha(uint16_t mask, uint16_t value);
        void writeBldY(uint8_t value);
        void writeMasterBright(uint16_t mask, uint16_t value);

    private:
        Core *core;
        bool engine;

        uint32_t bgVramAddr, objVramAddr;
        uint8_t *palette, *oam;
        uint8_t **extPalettes;

        uint32_t framebuffer[256 * 192] = {};
        uint32_t layers[2][256] = {};
        int8_t priorities[2][256] = {};
        int8_t blendBits[2][256] = {};

        int internalX[2] = {};
        int internalY[2] = {};

        uint32_t dispCnt = 0;
        uint16_t bgCnt[4] = {};
        uint16_t bgHOfs[4] = {};
        uint16_t bgVOfs[4] = {};
        int16_t bgPA[2] = {};
        int16_t bgPB[2] = {};
        int16_t bgPC[2] = {};
        int16_t bgPD[2] = {};
        int32_t bgX[2] = {};
        int32_t bgY[2] = {};
        uint16_t winX1[2] = {};
        uint16_t winX2[2] = {};
        uint16_t winY1[2] = {};
        uint16_t winY2[2] = {};
        uint16_t winIn = 0;
        uint16_t winOut = 0;
        uint16_t mosaic = 0;
        uint16_t bldCnt = 0;
        uint16_t bldAlpha = 0;
        uint8_t bldY = 0;
        uint16_t masterBright = 0;

#ifdef NOODS_PROFILE
        bool profileEnabled = false;
        Gpu2DRunProfile runProfile;

        bool beginProfilePhase(Gpu2DProfilePhase phase, clock_t &start);
        void endProfilePhase(Gpu2DProfilePhase phase, clock_t start, bool sampled);
#if NOODS_PROFILE_LEVEL >= 2
        bool beginExtendedProfile(Gpu2DExtendedMode mode, clock_t &start);
        void endExtendedProfile(Gpu2DExtendedMode mode, clock_t start,
            clock_t setupEnd, bool sampled,
            const Gpu2DExtendedCounters &counters);
#endif

        class ProfileScope
        {
            public:
                ProfileScope(Gpu2D *gpu, Gpu2DProfilePhase phase):
                    gpu(gpu), phase(phase), sampled(gpu->beginProfilePhase(phase, start)) {}
                ~ProfileScope() { gpu->endProfilePhase(phase, start, sampled); }

            private:
                Gpu2D *gpu;
                Gpu2DProfilePhase phase;
                clock_t start = 0;
                bool sampled;
        };
#endif

        static uint32_t rgb5ToRgb6(uint32_t color);

        void drawBgPixel(int bg, int line, int x, uint32_t pixel);
        void drawObjPixel(int line, int x, uint32_t pixel, int8_t priority);

        void drawText(int bg, int line);
        void drawAffine(int bg, int line);
        void drawExtended(int bg, int line);
        void drawLarge(int bg, int line);
        void drawObjects(int line, bool window);
};

#endif // GPU_2D_H

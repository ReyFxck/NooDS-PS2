/*
    NooDS-PS2 bootstrap
    Copyright 2026 NooDS-PS2 contributors

    NooDS-PS2 is based on NooDS and is distributed under GPLv3 or later.
    This bootstrap is intentionally small: it validates the PS2 toolchain
    and host: filesystem before the emulator core is wired in.
*/

#include <cctype>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <debug.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <malloc.h>
#include <dmaKit.h>
#include <gsKit.h>
#include <new>
#include <string>

#include "../core.h"
#include "dynarec_r5900.h"
#include "../settings.h"

namespace
{
#define PS2_LOG(...) do { printf(__VA_ARGS__); scr_printf(__VA_ARGS__); } while (0)

constexpr const char *BASE_COMMIT = "392d9631c4b2a748b8dd460caa1643f79ffaca7f";
constexpr const char *BASE_DATE   = "2021-09-29";

constexpr const char *HOST_ROM      = "host:game.nds";
constexpr const char *HOST_BIOS9    = "host:bios9.bin";
constexpr const char *HOST_BIOS7    = "host:bios7.bin";
constexpr const char *HOST_FIRMWARE = "host:firmware.bin";

constexpr const char *DISC_ROM      = "cdrom0:\\SELFTEST.NDS;1";
constexpr const char *DISC_BIOS9    = "cdrom0:\\BIOS9.BIN;1";
constexpr const char *DISC_BIOS7    = "cdrom0:\\BIOS7.BIN;1";
constexpr const char *DISC_FIRMWARE = "cdrom0:\\FIRMWARE.BIN;1";

alignas(64) static char padBuffer[256];

bool initPs2Pad()
{
    int sio = SifLoadModule("rom0:SIO2MAN", 0, nullptr);
    int padman = SifLoadModule("rom0:PADMAN", 0, nullptr);

    PS2_LOG("[PS2] PAD modules: SIO2MAN=%d PADMAN=%d\n", sio, padman);

    if (padInit(0) == 0)
    {
        PS2_LOG("[PS2][WARN] padInit() failed; continuing without controller input.\n");
        return false;
    }

    if (padPortOpen(0, 0, padBuffer) == 0)
    {
        PS2_LOG("[PS2][WARN] padPortOpen(0,0) failed; continuing without controller input.\n");
        return false;
    }

    PS2_LOG("[PS2][OK] DualShock input backend opened on port 0 slot 0.\n");
    return true;
}

void setNdsKey(Core *core, int key, bool pressed)
{
    if (pressed)
        core->input.pressKey(key);
    else
        core->input.releaseKey(key);
}

void pollPadIntoNds(Core *core)
{
    int state = padGetState(0, 0);
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)
        return;

    static bool analogRequested = false;
    if (!analogRequested)
    {
        // Request DualShock mode once the controller is responsive. Do not
        // block waiting for the mode transition; later polls will pick it up.
        padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
        analogRequested = true;
        PS2_LOG("[PS2] Requested DualShock analog mode for right-stick touchscreen.\n");
        return;
    }

    struct padButtonStatus buttons = {};
    if (!padRead(0, 0, &buttons))
        return;

    uint16_t pressed = static_cast<uint16_t>(0xFFFFu ^ buttons.btns);

    setNdsKey(core, 0,  pressed & PAD_CIRCLE);   // DS A
    setNdsKey(core, 1,  pressed & PAD_CROSS);    // DS B
    setNdsKey(core, 10, pressed & PAD_TRIANGLE); // DS X
    setNdsKey(core, 11, pressed & PAD_SQUARE);   // DS Y

    setNdsKey(core, 2, pressed & PAD_SELECT);
    setNdsKey(core, 3, pressed & PAD_START);
    setNdsKey(core, 4, pressed & PAD_RIGHT);
    setNdsKey(core, 5, pressed & PAD_LEFT);
    setNdsKey(core, 6, pressed & PAD_UP);
    setNdsKey(core, 7, pressed & PAD_DOWN);
    setNdsKey(core, 8, pressed & PAD_R1);
    setNdsKey(core, 9, pressed & PAD_L1);

    // Right stick is an absolute stylus position.
    // Clamp to the DS touch area; R2 acts as pen-down.
    int touchX = buttons.rjoy_h;
    int touchY = (static_cast<int>(buttons.rjoy_v) * 192) / 256;
    if (touchX < 1) touchX = 1; else if (touchX > 254) touchX = 254;
    if (touchY < 1) touchY = 1; else if (touchY > 190) touchY = 190;

    if (pressed & PAD_R2)
    {
        core->spi.setTouch(touchX, touchY);
        core->input.pressScreen();
    }
    else
    {
        core->spi.clearTouch();
        core->input.releaseScreen();
    }
}

std::string chooseReadablePath(const char *preferred, const char *fallback)
{
    FILE *file = fopen(preferred, "rb");
    if (file)
    {
        fclose(file);
        return preferred;
    }

    file = fopen(fallback, "rb");
    if (file)
    {
        fclose(file);
        PS2_LOG("[PS2] Falling back to disc self-test file: %s\n", fallback);
        return fallback;
    }

    return preferred;
}


bool fileIsAllZero(const char *path, size_t expectedSize)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;

    uint8_t buffer[512];
    size_t total = 0;
    bool allZero = true;

    while (total < expectedSize)
    {
        size_t want = expectedSize - total;
        if (want > sizeof(buffer))
            want = sizeof(buffer);

        size_t got = fread(buffer, 1, want, file);
        if (!got)
            break;

        for (size_t i = 0; i < got; i++)
            if (buffer[i] != 0)
            {
                allZero = false;
                break;
            }

        total += got;
        if (!allZero)
            break;
    }

    fclose(file);
    return allZero && total == expectedSize;
}

long getFileSize(FILE *file)
{
    if (!file)
        return -1;

    long current = ftell(file);
    if (current < 0)
        current = 0;

    if (fseek(file, 0, SEEK_END) != 0)
        return -1;

    long size = ftell(file);
    fseek(file, current, SEEK_SET);
    return size;
}

bool probeFile(const char *label, const char *path, long minimumSize, bool required)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        PS2_LOG("[PS2][%s] %s: %s\n", required ? "ERROR" : "WARN", label, path);
        if (required)
            PS2_LOG("[PS2] Could not open the required file through host:.\n");
        return false;
    }

    long size = getFileSize(file);
    fclose(file);

    if (size < minimumSize)
    {
        PS2_LOG("[PS2][%s] %s is too small: %ld bytes (%s)\n",
            required ? "ERROR" : "WARN", label, size, path);
        return false;
    }

    PS2_LOG("[PS2][OK] %s: %s (%ld bytes)\n", label, path, size);
    return true;
}

bool probeNdsRom(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        PS2_LOG("[PS2][ERROR] ROM not found: %s\n", path);
        return false;
    }

    uint8_t header[0x20] = {};
    size_t read = fread(header, 1, sizeof(header), file);
    long size = getFileSize(file);
    fclose(file);

    if (read != sizeof(header) || size < 0x200)
    {
        PS2_LOG("[PS2][ERROR] ROM header is incomplete or file is too small: %s\n", path);
        return false;
    }

    char title[13] = {};
    char gameCode[5] = {};
    memcpy(title, header, 12);
    memcpy(gameCode, &header[12], 4);

    for (int i = 0; i < 12; i++)
        if (!std::isprint(static_cast<unsigned char>(title[i])))
            title[i] = ' ';

    for (int i = 0; i < 4; i++)
        if (!std::isprint(static_cast<unsigned char>(gameCode[i])))
            gameCode[i] = '?';

    PS2_LOG("[PS2][OK] NDS ROM: %s (%ld bytes)\n", path, size);
    PS2_LOG("[PS2]      title='%s' gameCode='%s'\n", title, gameCode);
    return true;
}

void logInterpreterProfile(const char *label, const InterpreterProfile &profile)
{
    PS2_LOG("[PS2][PROF] %s instr=%llu ARM=%llu THUMB=%llu | ARM dp=%llu mem=%llu multi=%llu branch=%llu sys=%llu | THUMB alu=%llu mem=%llu stack=%llu multi=%llu branch=%llu\n",
        label,
        static_cast<unsigned long long>(profile.instructions),
        static_cast<unsigned long long>(profile.armInstructions),
        static_cast<unsigned long long>(profile.thumbInstructions),
        static_cast<unsigned long long>(profile.armClass[0]),
        static_cast<unsigned long long>(profile.armClass[1]),
        static_cast<unsigned long long>(profile.armClass[2]),
        static_cast<unsigned long long>(profile.armClass[3]),
        static_cast<unsigned long long>(profile.armClass[4]),
        static_cast<unsigned long long>(profile.thumbClass[0]),
        static_cast<unsigned long long>(profile.thumbClass[1]),
        static_cast<unsigned long long>(profile.thumbClass[2]),
        static_cast<unsigned long long>(profile.thumbClass[3]),
        static_cast<unsigned long long>(profile.thumbClass[4]));

    InterpreterHotPc top[6] = {};
    for (size_t i = 0; i < 256; i++)
    {
        const InterpreterHotPc &candidate = profile.hotPc[i];
        if (!candidate.samples)
            continue;

        for (int slot = 0; slot < 6; slot++)
        {
            if (candidate.samples > top[slot].samples)
            {
                for (int move = 5; move > slot; move--)
                    top[move] = top[move - 1];
                top[slot] = candidate;
                break;
            }
        }
    }

    PS2_LOG("[PS2][PROF] %s hotPC(sample/64): %08lX:%lu %08lX:%lu %08lX:%lu %08lX:%lu %08lX:%lu %08lX:%lu\n",
        label,
        static_cast<unsigned long>(top[0].pc), static_cast<unsigned long>(top[0].samples),
        static_cast<unsigned long>(top[1].pc), static_cast<unsigned long>(top[1].samples),
        static_cast<unsigned long>(top[2].pc), static_cast<unsigned long>(top[2].samples),
        static_cast<unsigned long>(top[3].pc), static_cast<unsigned long>(top[3].samples),
        static_cast<unsigned long>(top[4].pc), static_cast<unsigned long>(top[4].samples),
        static_cast<unsigned long>(top[5].pc), static_cast<unsigned long>(top[5].samples));
}
}

int main(int argc, char **argv)
{
    // Make bring-up results visible in the PCSX2/PS2 video output as well as stdout.
    sceSifInitRpc(0);
    init_scr();
    bool padReady = initPs2Pad();
    PS2_LOG("\n");
    PS2_LOG("============================================================\n");
    PS2_LOG(" NooDS-PS2 POC bootstrap\n");
    PS2_LOG(" Base: %s (%s)\n", BASE_COMMIT, BASE_DATE);
#ifdef DEBUG
    PS2_LOG(" Build: DEBUG\n");
#else
    PS2_LOG(" Build: RELEASE\n");
#endif
    PS2_LOG("============================================================\n");

    std::string romPath = (argc > 1 && argv[1] && argv[1][0])
        ? std::string(argv[1])
        : chooseReadablePath(HOST_ROM, DISC_ROM);
    std::string bios9Path = chooseReadablePath(HOST_BIOS9, DISC_BIOS9);
    std::string bios7Path = chooseReadablePath(HOST_BIOS7, DISC_BIOS7);
    std::string firmwarePath = chooseReadablePath(HOST_FIRMWARE, DISC_FIRMWARE);

    PS2_LOG("[PS2] M0 bootstrap started.\n");
    PS2_LOG("[PS2] ROM path: %s\n", romPath.c_str());
    PS2_LOG("[PS2] Probing data files...\n");

    // Print the object size before any external file requirement so memory
    // investigations remain visible even when HostFS is unavailable.
    PS2_LOG("[PS2] sizeof(Core) = %u bytes\n", static_cast<unsigned int>(sizeof(Core)));

    bool romOk = probeNdsRom(romPath.c_str());
    bool bios9Ok = probeFile("ARM9 BIOS", bios9Path.c_str(), 0x1000, true);
    bool bios7Ok = probeFile("ARM7 BIOS", bios7Path.c_str(), 0x4000, true);
    bool firmwareOk = probeFile("Firmware", firmwarePath.c_str(), 0x40000, true);

    bool bios9Synthetic = bios9Ok && fileIsAllZero(bios9Path.c_str(), 0x1000);
    bool firmwareSynthetic = firmwareOk && fileIsAllZero(firmwarePath.c_str(), 0x40000);

    if (bios9Synthetic)
        PS2_LOG("[PS2][WARN] ARM9 BIOS is the zero-filled synthetic M2 resource.\n");
    if (firmwareSynthetic)
        PS2_LOG("[PS2][WARN] Firmware is the zero-filled synthetic bring-up resource.\n");

    if (!romOk)
    {
        PS2_LOG("\n[PS2] ROM probe FAILED.\n");
        PS2_LOG("[PS2] Tried HostFS first and the embedded disc self-test as fallback.\n");
        fflush(stdout);
        return 2;
    }

    PS2_LOG("\n[PS2] ROM read succeeded. M1 data path is alive.\n");
    PS2_LOG("[PS2] BIOS probes: ARM9=%s ARM7=%s firmware=%s\n",
        bios9Ok ? "OK" : "missing",
        bios7Ok ? "OK" : "missing",
        firmwareOk ? "OK" : "missing");

    if (!bios9Ok || !bios7Ok || !firmwareOk)
    {
        PS2_LOG("[PS2] M1 succeeded, but M2 core construction needs BIOS9, BIOS7 and 256 KiB firmware.\n");
        fflush(stdout);
        return 3;
    }

    // PS2 bring-up policy: keep all rendering on the EE thread until the
    // single-threaded path is proven stable. No game-specific behavior here.
    Settings::setDirectBoot(1);
    Settings::setFpsLimiter(0);
    Settings::setThreaded2D(0);
    Settings::setThreaded3D(0);
    Settings::setBios9Path(bios9Path);
    Settings::setBios7Path(bios7Path);
    Settings::setFirmwarePath(firmwarePath);

    PS2_LOG("[PS2] Constructing NooDS core (no frame execution yet)...\n");
    fflush(stdout);

    try
    {
        clock_t coreConstructStart = clock();
        Core *core = new Core(romPath, "");
        clock_t coreConstructTicks = clock() - coreConstructStart;
        PS2_LOG("[PS2][PROF] core construction: %.2f ms\n",
            static_cast<double>(coreConstructTicks) * 1000.0 / CLOCKS_PER_SEC);

        // The embedded disc image is a regression/self-test environment.
        // Freeze its RTC so RELEASE and DEBUG runs can be compared by hash.
        // HostFS/argv ROMs keep the normal host clock.
        if (romPath == DISC_ROM)
        {
            core->rtc.setFixedDateTime(0, 1, 1, 5, 0, 0, 0);
            PS2_LOG("[PS2] Deterministic self-test RTC: 2000-01-01 00:00:00.\n");
        }

        PS2_LOG("[PS2] sparse page tables: %u bytes, generation=%lu\n",
            static_cast<unsigned int>(core->memory.getPageTableBytes()),
            static_cast<unsigned long>(core->memory.getMapGeneration()));

        // Prepare, but do not activate, the R5900 dynarec. The interpreter
        // remains the only CPU execution path until compiled blocks are
        // validated against deterministic CPU tests.
        DynarecR5900 dynarec(&core->memory);
        if (dynarec.init())
        {
            uint32_t probeValue = 0;
            if (dynarec.runProbe(&probeValue))
            {
                PS2_LOG("[PS2][OK] R5900 dynarec probe executed: result=%08lX, cache=%u bytes.\n",
                    static_cast<unsigned long>(probeValue),
                    static_cast<unsigned int>(dynarec.codeCacheUsed()));

                if (dynarec.runArmAluSelfTest())
                {
                    PS2_LOG("[PS2][OK] ARM->R5900 ALU self-test: register/immediate MOV/ADD/SUB/AND/EOR/ORR passed.\n");

                    uint32_t arm9StateHash = 0;
                    if (dynarec.runArm9StateSelfTest(core->interpreter[0], &arm9StateHash))
                    {
                        PS2_LOG("[PS2][OK] ARM9 live-state dynarec gate passed: hash=%08lX.\n",
                            static_cast<unsigned long>(arm9StateHash));

                        uint32_t livePc = 0;
                        uint32_t liveOpcode = 0;
                        int pipelineGate = dynarec.runArm9PipelineSelfTest(
                            core->interpreter[0], &livePc, &liveOpcode);
                        if (pipelineGate > 0)
                        {
                            PS2_LOG("[PS2][OK] ARM9 next-opcode shadow gate passed: pc=%08lX opcode=%08lX.\n",
                                static_cast<unsigned long>(livePc),
                                static_cast<unsigned long>(liveOpcode));

                            uint32_t blockPc = 0;
                            uint32_t blockInstructions = 0;
                            uint32_t blockHash = 0;
                            int blockGate = dynarec.runArm9BlockShadowSelfTest(
                                core->interpreter[0], &blockPc, &blockInstructions, &blockHash);
                            if (blockGate > 0)
                            {
                                const R5900CompiledBlock *compiledBlock =
                                    dynarec.findBlock(blockPc, false);
                                PS2_LOG("[PS2][OK] ARM9 real-block shadow gate passed: pc=%08lX instructions=%lu blocks=%u cache=%u read=%04X write=%04X hash=%08lX.\n",
                                    static_cast<unsigned long>(blockPc),
                                    static_cast<unsigned long>(blockInstructions),
                                    static_cast<unsigned int>(dynarec.blockCount()),
                                    static_cast<unsigned int>(dynarec.codeCacheUsed()),
                                    compiledBlock ? compiledBlock->readMask : 0,
                                    compiledBlock ? compiledBlock->writeMask : 0,
                                    static_cast<unsigned long>(blockHash));

                                uint32_t diffPc = 0;
                                uint32_t diffInstructions = 0;
                                uint32_t diffHash = 0;
                                int diffGate = dynarec.runArm9InterpreterDifferentialSelfTest(
                                    core->interpreter[0], &diffPc, &diffInstructions, &diffHash);
                                if (diffGate > 0)
                                    PS2_LOG("[PS2][OK] ARM9 interpreter<->dynarec differential gate passed: pc=%08lX instructions=%lu hash=%08lX.\n",
                                        static_cast<unsigned long>(diffPc),
                                        static_cast<unsigned long>(diffInstructions),
                                        static_cast<unsigned long>(diffHash));
                                else if (diffGate == 0)
                                    PS2_LOG("[PS2] ARM9 interpreter<->dynarec differential gate skipped: no cached block.\n");
                                else
                                    PS2_LOG("[PS2][WARN] ARM9 interpreter<->dynarec differential mismatch; scheduler remains interpreter-only.\n");
                            }
                            else if (blockGate == 0)
                                PS2_LOG("[PS2] ARM9 real-block shadow gate: no straight-line ALU block available at current PC.\n");
                            else
                                PS2_LOG("[PS2][WARN] ARM9 real-block shadow gate mismatch; scheduler remains interpreter-only.\n");
                        }
                        else if (pipelineGate == 0)
                            PS2_LOG("[PS2] ARM9 next-opcode shadow gate: opcode is outside current ALU subset.\n");
                        else
                            PS2_LOG("[PS2][WARN] ARM9 next-opcode shadow gate mismatch: pc=%08lX opcode=%08lX.\n",
                                static_cast<unsigned long>(livePc),
                                static_cast<unsigned long>(liveOpcode));
                    }
                    else
                        PS2_LOG("[PS2][WARN] ARM9 live-state dynarec gate failed; interpreter remains active.\n");
                }
                else
                    PS2_LOG("[PS2][WARN] ARM->R5900 ALU self-test failed; interpreter remains active.\n");
            }
            else
                PS2_LOG("[PS2][WARN] R5900 dynarec probe failed; interpreter remains active.\n");
        }
        else
            PS2_LOG("[PS2][WARN] R5900 dynarec infrastructure unavailable; interpreter remains active.\n");
        PS2_LOG("[PS2][OK] M2: Core constructed, BIOS/firmware/ROM paths were accepted.\n");

        if (bios9Synthetic)
        {
            PS2_LOG("[PS2][BLOCKED] M3+ requires a real 4 KiB ARM9 BIOS dump.\n");
            PS2_LOG("[PS2] IRQ/SWI vectors execute BIOS9 code; the synthetic zero BIOS produced runaway ARM9 reads in the 30-frame test.\n");
            PS2_LOG("[PS2] Stopping safely after M2 instead of producing a misleading compatibility failure.\n");
            delete core;
            return 10;
        }

        if (firmwareSynthetic)
            PS2_LOG("[PS2][WARN] Continuing with synthetic firmware; real firmware will be required for compatibility validation.\n");

        // M3/M4 progression test. The first NSMB frame was proven to be
        // completely white (0xFFFFFFFF for all 98,304 pixels), so presenting
        // only that frame cannot validate visible game output. Run a bounded
        // number of frames, consuming every queued framebuffer immediately to
        // avoid queue growth in the PS2's 32MB RAM, and stop at the first frame
        // that contains any non-white pixel.
        constexpr size_t FRAME_PIXELS = 256 * 192 * 2;
        constexpr int MAX_BOOT_FRAMES = 30;
        uint32_t *frame = static_cast<uint32_t*>(
            memalign(128, FRAME_PIXELS * sizeof(uint32_t)));
        if (!frame)
            throw std::bad_alloc();

        clock_t m3Start = clock();
        uint32_t cyclesBefore = core->getGlobalCycles();
        PS2_LOG("[PS2] M3: running up to %d NDS frames until output is not all-white... cycles=%lu\n",
            MAX_BOOT_FRAMES, static_cast<unsigned long>(cyclesBefore));
        fflush(stdout);

        bool gotFrame = false;
        bool nonWhiteFrame = false;
        int selectedFrame = 0;
        uint32_t selectedHash = 0;
        uint32_t selectedWhite = 0;

        for (int frameNo = 1; frameNo <= MAX_BOOT_FRAMES; frameNo++)
        {
            if (padReady)
                pollPadIntoNds(core);
            core->runFrame();

            if (!core->gpu.getFrame(frame, false))
            {
                PS2_LOG("[PS2][ERROR] M4: frame %d executed but GPU had no completed framebuffer.\n", frameNo);
                free(frame);
                delete core;
                return 7;
            }

            gotFrame = true;

            uint32_t hash = 2166136261u; // FNV-1a over 32-bit pixels
            uint32_t white = 0;
            for (size_t i = 0; i < FRAME_PIXELS; i++)
            {
                hash ^= frame[i];
                hash *= 16777619u;
                if (frame[i] == 0xFFFFFFFFu)
                    white++;
            }

            selectedFrame = frameNo;
            selectedHash = hash;
            selectedWhite = white;

            if (frameNo == 1 || (frameNo % 5) == 0 || white != FRAME_PIXELS)
            {
                PS2_LOG("[PS2] M4: frame=%d hash=%08lX white=%lu/%u cycles=%lu\n",
                    frameNo,
                    static_cast<unsigned long>(hash),
                    static_cast<unsigned long>(white),
                    static_cast<unsigned int>(FRAME_PIXELS),
                    static_cast<unsigned long>(core->getGlobalCycles()));
                fflush(stdout);
            }

            if (white != FRAME_PIXELS)
            {
                nonWhiteFrame = true;
                break;
            }
        }

        if (!gotFrame)
        {
            PS2_LOG("[PS2][ERROR] M4: no framebuffer was retrieved.\n");
            free(frame);
            delete core;
            return 7;
        }

        uint32_t cyclesAfter = core->getGlobalCycles();
        PS2_LOG("[PS2][OK] M3: frame execution returned. cycles=%lu -> %lu\n",
            static_cast<unsigned long>(cyclesBefore),
            static_cast<unsigned long>(cyclesAfter));
        PS2_LOG("[PS2][PROF] M3 boot frames: %d frames in %.2f ms (%.2f ms/frame)\n",
            selectedFrame,
            static_cast<double>(clock() - m3Start) * 1000.0 / CLOCKS_PER_SEC,
            selectedFrame > 0
                ? (static_cast<double>(clock() - m3Start) * 1000.0 / CLOCKS_PER_SEC) / selectedFrame
                : 0.0);

        if (nonWhiteFrame)
        {
            PS2_LOG("[PS2][OK] M4: first non-white framebuffer at frame %d. hash=%08lX white=%lu/%u\n",
                selectedFrame,
                static_cast<unsigned long>(selectedHash),
                static_cast<unsigned long>(selectedWhite),
                static_cast<unsigned int>(FRAME_PIXELS));
        }
        else
        {
            PS2_LOG("[PS2][WARN] M4: all %d tested frames remained completely white. Last hash=%08lX\n",
                MAX_BOOT_FRAMES, static_cast<unsigned long>(selectedHash));
        }

        // M5: first real PS2 GS presentation path. Keep it intentionally
        // simple: one framebuffer, no Z-buffer, two 256x192 CT32 textures.
        // This proves the frontend path before attempting continuous emulation.
        PS2_LOG("[PS2] M5: initializing GS and presenting selected frame %d (%s)...\n",
            selectedFrame, nonWhiteFrame ? "non-white" : "still all-white");
        fflush(stdout);

        dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
            D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
        dmaKit_chan_init(DMA_CHANNEL_GIF);

        GSGLOBAL *gsGlobal = gsKit_init_global();
        if (!gsGlobal)
        {
            PS2_LOG("[PS2][ERROR] M5: gsKit_init_global() failed.\n");
            free(frame);
            delete core;
            return 8;
        }

        gsGlobal->Mode = GS_MODE_NTSC;
        gsGlobal->Interlace = GS_INTERLACED;
        gsGlobal->Field = GS_FIELD;
        gsGlobal->Width = 640;
        gsGlobal->Height = 448;
        gsGlobal->PSM = GS_PSM_CT32;
        // Streamed NDS frames must not render directly into the buffer that is
        // currently being scanned out. gsKit's own framebuffer streaming
        // example documents horizontal tearing when double buffering is off.
        gsGlobal->DoubleBuffering = GS_SETTING_ON;
        gsGlobal->ZBuffering = GS_SETTING_OFF;
        gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

        gsKit_init_screen(gsGlobal);
        // Rebuild the tiny render list every frame. Persistent mode is ideal
        // for static scenes, but a streamed framebuffer is safer as ONESHOT:
        // every back buffer is explicitly cleared and both sprites are drawn.
        gsKit_mode_switch(gsGlobal, GS_ONESHOT);
        gsKit_set_test(gsGlobal, GS_ZTEST_OFF);

        GSTEXTURE top = {};
        GSTEXTURE bottom = {};

        top.Width = bottom.Width = 256;
        top.Height = bottom.Height = 192;
        top.PSM = bottom.PSM = GS_PSM_CT32;
        top.Filter = bottom.Filter = GS_FILTER_NEAREST;
        static_assert(sizeof(uint32_t) == sizeof(u32), "PS2 GS texture word must be 32-bit");
        top.Mem = reinterpret_cast<u32*>(&frame[0]);
        bottom.Mem = reinterpret_cast<u32*>(&frame[256 * 192]);

        const int textureBytes = gsKit_texture_size(256, 192, GS_PSM_CT32);
        top.Vram = gsKit_vram_alloc(gsGlobal, textureBytes, GSKIT_ALLOC_USERBUFFER);
        bottom.Vram = gsKit_vram_alloc(gsGlobal, textureBytes, GSKIT_ALLOC_USERBUFFER);

        if (top.Vram == GSKIT_ALLOC_ERROR || bottom.Vram == GSKIT_ALLOC_ERROR)
        {
            PS2_LOG("[PS2][ERROR] M5: GS VRAM allocation failed.\n");
            free(frame);
            delete core;
            return 9;
        }

        gsKit_texture_upload(gsGlobal, &top);
        gsKit_texture_upload(gsGlobal, &bottom);

        const u64 blackColor = GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00);
        const u64 textureColor = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00);

        // Native DS pixels, centered in NTSC 640x448 with a small gap.
        // Top:    (192, 24) -> (448, 216)
        // Bottom: (192,232) -> (448, 424)
        auto queueNdsScreens = [&]()
        {
            gsKit_clear(gsGlobal, blackColor);
            gsKit_prim_sprite_texture(gsGlobal, &top,
                192.0f, 24.0f, 0.0f, 0.0f,
                448.0f, 216.0f, 256.0f, 192.0f,
                1, textureColor);
            gsKit_prim_sprite_texture(gsGlobal, &bottom,
                192.0f, 232.0f, 0.0f, 0.0f,
                448.0f, 424.0f, 256.0f, 192.0f,
                1, textureColor);
        };

        PS2_LOG("[PS2][OK] M5: both NDS screens uploaded to GS. Priming both display buffers with frame %d.\n",
            selectedFrame);
        fflush(stdout);

        // Paint both GS screen buffers before streaming. init_scr() owned the
        // display during bootstrap, so explicitly overwriting both buffers
        // prevents stale debug text from resurfacing on a later flip.
        for (int prime = 0; prime < 2; prime++)
        {
            queueNdsScreens();
            gsKit_queue_exec(gsGlobal);
            gsKit_sync_flip(gsGlobal);
        }

        // M5 stream test: keep emulation moving after the first visible frame.
        // This lets boot sequences (for example, a logo appearing later) become
        // visible instead of freezing the very first non-white frame forever.

        int frameNo = selectedFrame + 1;
        clock_t benchStart = clock();
        int benchFrames = 0;

        core->setProfileEnabled(true);
        core->resetProfile();

        clock_t profCoreTicks = 0;
        clock_t profGetFrameTicks = 0;
        clock_t profUploadTicks = 0;
        clock_t profPresentTicks = 0;
        int profFrames = 0;
        uint32_t profLastRomHits = core->cartridgeNds.getRomCacheHits();
        uint32_t profLastRomMisses = core->cartridgeNds.getRomCacheMisses();

        while (1)
        {
            if (padReady)
                pollPadIntoNds(core);

            clock_t profTick = clock();
            core->runFrame();
            profCoreTicks += clock() - profTick;
            benchFrames++;
            profFrames++;

            profTick = clock();
            bool frameReady = core->gpu.getFrame(frame, false);
            profGetFrameTicks += clock() - profTick;

            if (!frameReady)
            {
                PS2_LOG("[PS2][ERROR] M5: frame %d executed but no framebuffer was ready.\n", frameNo);
                break;
            }

            // Report each screen independently at a low cadence. One DS screen
            // contains exactly 49,152 pixels, which makes all-white halves easy
            // to distinguish from a fully-white frame.
            if ((frameNo % 60) == 0)
            {
                clock_t now = clock();
                double elapsed = static_cast<double>(now - benchStart) / CLOCKS_PER_SEC;
                double emuFps = (elapsed > 0.0) ? (benchFrames / elapsed) : 0.0;

                PS2_LOG("[PS2] interpreter benchmark: %d frames in %.3f s = %.2f NDS fps\n",
                    benchFrames, elapsed, emuFps);

                double profCoreMs = static_cast<double>(profCoreTicks) * 1000.0 / CLOCKS_PER_SEC;
                double profGetMs = static_cast<double>(profGetFrameTicks) * 1000.0 / CLOCKS_PER_SEC;
                double profUploadMs = static_cast<double>(profUploadTicks) * 1000.0 / CLOCKS_PER_SEC;
                double profPresentMs = static_cast<double>(profPresentTicks) * 1000.0 / CLOCKS_PER_SEC;
                double profKnownMs = profCoreMs + profGetMs + profUploadMs + profPresentMs;
                double profFrameDiv = profFrames > 0 ? static_cast<double>(profFrames) : 1.0;

                PS2_LOG("[PS2][PROF] %d frames: core=%.2fms/f getFrame=%.2fms/f uploadGS=%.2fms/f present=%.2fms/f known=%.2fms/f\n",
                    profFrames,
                    profCoreMs / profFrameDiv,
                    profGetMs / profFrameDiv,
                    profUploadMs / profFrameDiv,
                    profPresentMs / profFrameDiv,
                    profKnownMs / profFrameDiv);

                const CoreRunProfile &runProfile = core->getRunProfile();
                PS2_LOG("[PS2][PROF] scheduler: ARM9=%llu ARM7=%llu tasks=%llu schedules=%llu maxDepth=%lu\n",
                    static_cast<unsigned long long>(runProfile.arm9Opcodes),
                    static_cast<unsigned long long>(runProfile.arm7Opcodes),
                    static_cast<unsigned long long>(runProfile.taskExecutions),
                    static_cast<unsigned long long>(runProfile.scheduleCalls),
                    static_cast<unsigned long>(runProfile.maxTaskDepth));

                uint32_t currentRomHits = core->cartridgeNds.getRomCacheHits();
                uint32_t currentRomMisses = core->cartridgeNds.getRomCacheMisses();
                uint32_t windowHits = currentRomHits - profLastRomHits;
                uint32_t windowMisses = currentRomMisses - profLastRomMisses;
                uint32_t windowTotal = windowHits + windowMisses;
                double windowHitRate = windowTotal
                    ? (100.0 * static_cast<double>(windowHits) / windowTotal)
                    : 100.0;
                PS2_LOG("[PS2][PROF] ROM window: hits=%lu misses=%lu hitRate=%.2f%% cache=%lu bytes\n",
                    static_cast<unsigned long>(windowHits),
                    static_cast<unsigned long>(windowMisses),
                    windowHitRate,
                    static_cast<unsigned long>(core->cartridgeNds.getRomCacheBytes()));
                profLastRomHits = currentRomHits;
                profLastRomMisses = currentRomMisses;

                logInterpreterProfile("ARM9", core->interpreter[0].getProfile());
                logInterpreterProfile("ARM7", core->interpreter[1].getProfile());

                core->resetProfile();
                profCoreTicks = 0;
                profGetFrameTicks = 0;
                profUploadTicks = 0;
                profPresentTicks = 0;
                profFrames = 0;

                uint32_t topHash = 2166136261u;
                uint32_t bottomHash = 2166136261u;
                uint32_t topWhite = 0;
                uint32_t bottomWhite = 0;

                for (size_t i = 0; i < 256 * 192; i++)
                {
                    topHash ^= frame[i];
                    topHash *= 16777619u;
                    if (frame[i] == 0xFFFFFFFFu)
                        topWhite++;

                    bottomHash ^= frame[256 * 192 + i];
                    bottomHash *= 16777619u;
                    if (frame[256 * 192 + i] == 0xFFFFFFFFu)
                        bottomWhite++;
                }

                PS2_LOG("[PS2] M5 stream: frame=%d top(hash=%08lX white=%lu/49152) bottom(hash=%08lX white=%lu/49152) cycles=%lu\n",
                    frameNo,
                    static_cast<unsigned long>(topHash),
                    static_cast<unsigned long>(topWhite),
                    static_cast<unsigned long>(bottomHash),
                    static_cast<unsigned long>(bottomWhite),
                    static_cast<unsigned long>(core->getGlobalCycles()));
                if ((frameNo % 300) == 0)
                {
                    PS2_LOG("[PS2] ROM cache: hits=%lu misses=%lu bytes=%lu\n",
                        static_cast<unsigned long>(core->cartridgeNds.getRomCacheHits()),
                        static_cast<unsigned long>(core->cartridgeNds.getRomCacheMisses()),
                        static_cast<unsigned long>(core->cartridgeNds.getRomCacheBytes()));
                }

                fflush(stdout);
            }

            // Texture uploads happen immediately in gsKit. Upload first,
            // then build a fresh ONESHOT render queue targeting the back buffer.
            // Execute the queue before flipping, matching gsKit's framebuffer
            // streaming example and avoiding scanout/render overlap.
            profTick = clock();
            gsKit_texture_upload(gsGlobal, &top);
            gsKit_texture_upload(gsGlobal, &bottom);
            profUploadTicks += clock() - profTick;

            profTick = clock();
            queueNdsScreens();
            gsKit_queue_exec(gsGlobal);
            gsKit_sync_flip(gsGlobal);
            profPresentTicks += clock() - profTick;

            frameNo++;
        }
    }
    catch (const std::bad_alloc &)
    {
        PS2_LOG("[PS2][ERROR] Allocation failed during core construction or first-frame execution: PS2 RAM budget exhausted.\n");
        fflush(stdout);
        return 4;
    }
    catch (int code)
    {
        PS2_LOG("[PS2][ERROR] NooDS core threw error code %d during construction.\n", code);
        fflush(stdout);
        return 5;
    }
    catch (...)
    {
        PS2_LOG("[PS2][ERROR] Unknown exception while constructing the NooDS core.\n");
        fflush(stdout);
        return 6;
    }

    fflush(stdout);
    return 0;
}

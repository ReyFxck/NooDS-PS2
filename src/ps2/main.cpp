/*
    NooDS-PS2 bootstrap
    Copyright 2026 NooDS-PS2 contributors

    NooDS-PS2 is based on NooDS and is distributed under GPLv3 or later.
    This bootstrap is intentionally small: it validates the PS2 toolchain
    and host: filesystem before the emulator core is wired in.
*/

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <debug.h>
#include <sifrpc.h>
#include <new>
#include <string>

#include "../core.h"
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
}

int main(int argc, char **argv)
{
    // Make bring-up results visible in the PCSX2/PS2 video output as well as stdout.
    sceSifInitRpc(0);
    init_scr();
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
        Core *core = new Core(romPath, "");
        PS2_LOG("[PS2][OK] M2: Core constructed, BIOS/firmware/ROM paths were accepted.\n");
        PS2_LOG("[PS2] ARM9/ARM7 frame execution is intentionally deferred to M3.\n");
        delete core;
    }
    catch (const std::bad_alloc &)
    {
        PS2_LOG("[PS2][ERROR] Core allocation failed: PS2 RAM budget exhausted.\n");
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

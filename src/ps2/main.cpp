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
#include <new>
#include <string>

#include "../core.h"
#include "../settings.h"

namespace
{
constexpr const char *BASE_COMMIT = "392d9631c4b2a748b8dd460caa1643f79ffaca7f";
constexpr const char *BASE_DATE   = "2021-09-29";

constexpr const char *DEFAULT_ROM      = "host:game.nds";
constexpr const char *DEFAULT_BIOS9    = "host:bios9.bin";
constexpr const char *DEFAULT_BIOS7    = "host:bios7.bin";
constexpr const char *DEFAULT_FIRMWARE = "host:firmware.bin";

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
        printf("[PS2][%s] %s: %s\n", required ? "ERROR" : "WARN", label, path);
        if (required)
            printf("[PS2] Could not open the required file through host:.\n");
        return false;
    }

    long size = getFileSize(file);
    fclose(file);

    if (size < minimumSize)
    {
        printf("[PS2][%s] %s is too small: %ld bytes (%s)\n",
            required ? "ERROR" : "WARN", label, size, path);
        return false;
    }

    printf("[PS2][OK] %s: %s (%ld bytes)\n", label, path, size);
    return true;
}

bool probeNdsRom(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        printf("[PS2][ERROR] ROM not found: %s\n", path);
        return false;
    }

    uint8_t header[0x20] = {};
    size_t read = fread(header, 1, sizeof(header), file);
    long size = getFileSize(file);
    fclose(file);

    if (read != sizeof(header) || size < 0x200)
    {
        printf("[PS2][ERROR] ROM header is incomplete or file is too small: %s\n", path);
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

    printf("[PS2][OK] NDS ROM: %s (%ld bytes)\n", path, size);
    printf("[PS2]      title='%s' gameCode='%s'\n", title, gameCode);
    return true;
}
}

int main(int argc, char **argv)
{
    printf("\n");
    printf("============================================================\n");
    printf(" NooDS-PS2 POC bootstrap\n");
    printf(" Base: %s (%s)\n", BASE_COMMIT, BASE_DATE);
#ifdef DEBUG
    printf(" Build: DEBUG\n");
#else
    printf(" Build: RELEASE\n");
#endif
    printf("============================================================\n");

    const char *romPath = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : DEFAULT_ROM;

    printf("[PS2] M0 bootstrap started.\n");
    printf("[PS2] ROM path: %s\n", romPath);
    printf("[PS2] Probing host: filesystem...\n");

    bool romOk = probeNdsRom(romPath);

    // These are currently probes only. Core integration comes in M2.
    bool bios9Ok = probeFile("ARM9 BIOS", DEFAULT_BIOS9, 0x1000, true);
    bool bios7Ok = probeFile("ARM7 BIOS", DEFAULT_BIOS7, 0x4000, true);
    bool firmwareOk = probeFile("Firmware", DEFAULT_FIRMWARE, 0x40000, true);

    if (!romOk)
    {
        printf("\n[PS2] host: probe FAILED.\n");
        printf("[PS2] In PCSX2, enable Host Filesystem and place game.nds next to the ELF,\n");
        printf("[PS2] or pass an explicit path such as host:roms/test.nds.\n");
        fflush(stdout);
        return 2;
    }

    printf("\n[PS2] host: ROM read succeeded. M1 filesystem path is alive.\n");
    printf("[PS2] BIOS probes: ARM9=%s ARM7=%s firmware=%s\n",
        bios9Ok ? "OK" : "missing",
        bios7Ok ? "OK" : "missing",
        firmwareOk ? "OK" : "missing");

    if (!bios9Ok || !bios7Ok || !firmwareOk)
    {
        printf("[PS2] M1 succeeded, but M2 core construction needs BIOS9, BIOS7 and 256 KiB firmware.\n");
        fflush(stdout);
        return 3;
    }

    // PS2 bring-up policy: keep all rendering on the EE thread until the
    // single-threaded path is proven stable. No game-specific behavior here.
    Settings::setDirectBoot(1);
    Settings::setFpsLimiter(0);
    Settings::setThreaded2D(0);
    Settings::setThreaded3D(0);
    Settings::setBios9Path(DEFAULT_BIOS9);
    Settings::setBios7Path(DEFAULT_BIOS7);
    Settings::setFirmwarePath(DEFAULT_FIRMWARE);

    printf("[PS2] sizeof(Core) = %u bytes\n", static_cast<unsigned int>(sizeof(Core)));
    printf("[PS2] Constructing NooDS core (no frame execution yet)...\n");
    fflush(stdout);

    try
    {
        Core *core = new Core(std::string(romPath), "");
        printf("[PS2][OK] M2: Core constructed, BIOS/firmware/ROM paths were accepted.\n");
        printf("[PS2] ARM9/ARM7 frame execution is intentionally deferred to M3.\n");
        delete core;
    }
    catch (const std::bad_alloc &)
    {
        printf("[PS2][ERROR] Core allocation failed: PS2 RAM budget exhausted.\n");
        fflush(stdout);
        return 4;
    }
    catch (int code)
    {
        printf("[PS2][ERROR] NooDS core threw error code %d during construction.\n", code);
        fflush(stdout);
        return 5;
    }
    catch (...)
    {
        printf("[PS2][ERROR] Unknown exception while constructing the NooDS core.\n");
        fflush(stdout);
        return 6;
    }

    fflush(stdout);
    return 0;
}

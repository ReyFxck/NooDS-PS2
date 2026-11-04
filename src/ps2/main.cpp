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
#include <dirent.h>
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
#include "ps2_file.h"
#include "../settings.h"

#ifndef NOODS_BUILD_REVISION
#define NOODS_BUILD_REVISION "unknown"
#endif

#ifndef NOODS_BUILD_STATE
#define NOODS_BUILD_STATE "unknown"
#endif

#ifndef NOODS_PROFILE_LEVEL
#define NOODS_PROFILE_LEVEL 0
#endif

namespace
{
#define PS2_LOG(...) do { printf(__VA_ARGS__); } while (0)

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

bool pathReadable(const char *path)
{
    if (Ps2File::isHostPath(path))
        return Ps2File::canRead(path);

    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    fclose(file);
    return true;
}

std::string makeHostFilePath(const char *leaf)
{
    return std::string("host:") + (leaf ? leaf : "");
}

bool equalsIgnoreCase(const char *left, const char *right)
{
    if (!left || !right)
        return false;

    while (*left && *right)
    {
        if (std::tolower(static_cast<unsigned char>(*left)) !=
            std::tolower(static_cast<unsigned char>(*right)))
            return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

const char *hostEntryLeaf(const char *name)
{
    if (!name)
        return "";

    const char *leaf = name;
    for (const char *cursor = name; *cursor; cursor++)
        if (*cursor == '/' || *cursor == '\\')
            leaf = cursor + 1;
    return leaf;
}

bool hasNdsExtension(const char *name)
{
    if (!name)
        return false;

    name = hostEntryLeaf(name);
    size_t length = std::strlen(name);
    if (length < 4 || name[length - 4] != '.')
        return false;
    return std::tolower(static_cast<unsigned char>(name[length - 3])) == 'n' &&
        std::tolower(static_cast<unsigned char>(name[length - 2])) == 'd' &&
        std::tolower(static_cast<unsigned char>(name[length - 1])) == 's';
}

std::string normalizeRomArgument(const char *argument)
{
    if (!argument || !argument[0])
        return HOST_ROM;

    std::string path(argument);
    if (path.find(':') == std::string::npos)
        return makeHostFilePath(argument);
    return path;
}

std::string findHostFile(const char *preferredLeaf, bool allowAnyNds)
{
    std::string exact = makeHostFilePath(preferredLeaf);
    if (pathReadable(exact.c_str()))
    {
        PS2_LOG("[PS2][HOSTFS][OK] Found exact file: %s\n", exact.c_str());
        return exact;
    }

    DIR *directory = opendir("host:");
    if (!directory)
    {
        PS2_LOG("[PS2][HOSTFS][MISS] Could not open host: directory while looking for %s.\n",
            preferredLeaf);
        return exact;
    }

    std::string selected;
    while (dirent *entry = readdir(directory))
    {
        // NetherSX2's SAF-backed hostfs can return d_name values such as
        // "primary:PS2/GAME.NDS". Reusing that whole string would duplicate
        // the tree path and make IopHLE reject it as outside the ELF folder.
        const char *leaf = hostEntryLeaf(entry->d_name);
        if (!leaf[0] || !std::strcmp(leaf, ".") || !std::strcmp(leaf, ".."))
            continue;

        const bool preferred = equalsIgnoreCase(leaf, preferredLeaf);
        if (!preferred && !(allowAnyNds && hasNdsExtension(leaf)))
            continue;

        std::string candidate = makeHostFilePath(leaf);
        PS2_LOG("[PS2][HOSTFS] Candidate raw='%s' leaf='%s' -> %s\n",
            entry->d_name, leaf, candidate.c_str());

        if (!pathReadable(candidate.c_str()))
        {
            PS2_LOG("[PS2][HOSTFS][MISS] Candidate was listed but could not be opened: %s\n",
                candidate.c_str());
            continue;
        }

        if (preferred)
        {
            closedir(directory);
            PS2_LOG("[PS2][HOSTFS][OK] Resolved %s case-insensitively: %s\n",
                preferredLeaf, candidate.c_str());
            return candidate;
        }

        if (selected.empty() || candidate < selected)
            selected = candidate;
    }
    closedir(directory);

    if (!selected.empty())
    {
        PS2_LOG("[PS2][HOSTFS][OK] Auto-selected NDS ROM: %s\n", selected.c_str());
        return selected;
    }

    PS2_LOG("[PS2][HOSTFS][MISS] No readable match for %s.\n", preferredLeaf);
    return exact;
}

std::string findHostRomPath()
{
    PS2_LOG("[PS2][HOSTFS] Probing host: root (ELF launch directory when supported).\n");
    return findHostFile("game.nds", true);
}

std::string chooseReadablePath(const char *label, const char *preferred, const char *fallback)
{
    if (pathReadable(preferred))
    {
        PS2_LOG("[PS2][FILE][OK] %s: %s\n", label, preferred);
        return preferred;
    }

    PS2_LOG("[PS2][FILE][MISS] %s: %s\n", label, preferred);

    if (fallback && pathReadable(fallback))
    {
        PS2_LOG("[PS2][DISC] Using fallback %s: %s\n", label, fallback);
        return fallback;
    }

    return preferred;
}


bool fileIsAllZero(const char *path, size_t expectedSize)
{
    uint8_t buffer[512];
    size_t total = 0;
    bool allZero = true;

    if (Ps2File::isHostPath(path))
    {
        int fd = Ps2File::openRead(path);
        if (fd < 0)
            return false;

        while (total < expectedSize)
        {
            size_t want = expectedSize - total;
            if (want > sizeof(buffer))
                want = sizeof(buffer);
            int got = Ps2File::read(fd, buffer, want);
            if (got <= 0)
                break;

            for (int i = 0; i < got; i++)
                if (buffer[i] != 0)
                {
                    allZero = false;
                    break;
                }

            total += static_cast<size_t>(got);
            if (!allZero)
                break;
        }

        Ps2File::close(fd);
        return allZero && total == expectedSize;
    }

    FILE *file = fopen(path, "rb");
    if (!file)
        return false;

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
    long size = -1;

    if (Ps2File::isHostPath(path))
    {
        int fd = Ps2File::openRead(path);
        if (fd >= 0)
        {
            size = Ps2File::size(fd);
            Ps2File::close(fd);
        }
    }
    else
    {
        FILE *file = fopen(path, "rb");
        if (file)
        {
            size = getFileSize(file);
            fclose(file);
        }
    }

    if (size < 0)
    {
        PS2_LOG("[PS2][%s] %s: %s\n", required ? "ERROR" : "WARN", label, path);
        if (required)
            PS2_LOG("[PS2] Could not open the required file.\n");
        return false;
    }

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
    uint8_t header[0x20] = {};
    size_t read = 0;
    long size = -1;

    if (Ps2File::isHostPath(path))
    {
        int fd = Ps2File::openRead(path);
        if (fd >= 0)
        {
            int got = Ps2File::read(fd, header, sizeof(header));
            read = got > 0 ? static_cast<size_t>(got) : 0;
            size = Ps2File::size(fd);
            Ps2File::close(fd);
        }
    }
    else
    {
        FILE *file = fopen(path, "rb");
        if (file)
        {
            read = fread(header, 1, sizeof(header), file);
            size = getFileSize(file);
            fclose(file);
        }
    }

    if (size < 0)
    {
        PS2_LOG("[PS2][ERROR] ROM not found: %s\n", path);
        return false;
    }

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

#ifdef NOODS_PROFILE
void collectTopHotPc(const InterpreterProfile &profile, InterpreterHotPc *top,
    size_t count)
{
    if (!top || !count)
        return;
    for (size_t i = 0; i < count; i++)
        top[i] = InterpreterHotPc();

    for (size_t i = 0; i < 256; i++)
    {
        const InterpreterHotPc &candidate = profile.hotPc[i];
        if (!candidate.samples)
            continue;

        for (size_t slot = 0; slot < count; slot++)
        {
            if (candidate.samples > top[slot].samples)
            {
                for (size_t move = count - 1; move > slot; move--)
                    top[move] = top[move - 1];
                top[slot] = candidate;
                break;
            }
        }
    }
}

const char *armInstructionKindName(Ps2ArmAlu::InstructionKind kind)
{
    switch (kind)
    {
        case Ps2ArmAlu::KIND_DATA:            return "data";
        case Ps2ArmAlu::KIND_MULTIPLY:        return "multiply";
        case Ps2ArmAlu::KIND_MEMORY:          return "memory";
        case Ps2ArmAlu::KIND_MULTI_MEMORY:    return "multi";
        case Ps2ArmAlu::KIND_BRANCH:          return "branch";
        case Ps2ArmAlu::KIND_BRANCH_EXCHANGE: return "branch-exchange";
        case Ps2ArmAlu::KIND_SYSTEM:          return "system";
        default:                              return "other";
    }
}

const char *ndsMemoryClass(uint32_t address)
{
    switch (address >> 24)
    {
        case 0x00: return "ITCM/low";
        case 0x02: return "main-RAM";
        case 0x03: return "WRAM";
        case 0x04: return "MMIO";
        case 0x05: return "palette";
        case 0x06: return "VRAM";
        case 0x07: return "OAM";
        case 0x08: case 0x09: return "ROM";
        case 0xFF: return "BIOS/high";
        default:   return "unknown";
    }
}

void logMemoryProfile(const char *label, const MemoryCpuProfile &profile)
{
    PS2_LOG("[PS2][PROF] memory %s sampled/64: read=%llu direct=%llu fallback=%llu write=%llu direct=%llu fallback=%llu pageHit=%llu | MMIO=%llu/%llu VRAM=%llu/%llu palette=%llu/%llu TCM=%llu/%llu unaligned=%llu/%llu crossPage=%llu/%llu\n",
        label,
        static_cast<unsigned long long>(profile.sampledReads),
        static_cast<unsigned long long>(profile.directReads),
        static_cast<unsigned long long>(profile.fallbackReads),
        static_cast<unsigned long long>(profile.sampledWrites),
        static_cast<unsigned long long>(profile.directWrites),
        static_cast<unsigned long long>(profile.fallbackWrites),
        static_cast<unsigned long long>(profile.pageCacheHits),
        static_cast<unsigned long long>(profile.mmioReads),
        static_cast<unsigned long long>(profile.mmioWrites),
        static_cast<unsigned long long>(profile.vramReads),
        static_cast<unsigned long long>(profile.vramWrites),
        static_cast<unsigned long long>(profile.paletteReads),
        static_cast<unsigned long long>(profile.paletteWrites),
        static_cast<unsigned long long>(profile.tcmReads),
        static_cast<unsigned long long>(profile.tcmWrites),
        static_cast<unsigned long long>(profile.unalignedReads),
        static_cast<unsigned long long>(profile.unalignedWrites),
        static_cast<unsigned long long>(profile.crossPageReads),
        static_cast<unsigned long long>(profile.crossPageWrites));

    uint8_t mmioOrder[8] = {};
    for (uint8_t i = 0; i < 8; i++)
        mmioOrder[i] = i;
    for (uint8_t i = 0; i < 8; i++)
    {
        for (uint8_t j = i + 1; j < 8; j++)
        {
            if (profile.hotMmio[mmioOrder[j]].samples >
                profile.hotMmio[mmioOrder[i]].samples)
            {
                uint8_t swap = mmioOrder[i];
                mmioOrder[i] = mmioOrder[j];
                mmioOrder[j] = swap;
            }
        }
    }
    for (uint8_t rank = 0; rank < 8; rank++)
    {
        const MemoryHotMmio &hot = profile.hotMmio[mmioOrder[rank]];
        if (!hot.samples)
            break;
        PS2_LOG("[PS2][PROF] memory %s MMIO#%u address=%08lX samples=%lu read=%lu write=%lu\n",
            label,
            rank,
            static_cast<unsigned long>(hot.address),
            static_cast<unsigned long>(hot.samples),
            static_cast<unsigned long>(hot.reads),
            static_cast<unsigned long>(hot.writes));
    }
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
    collectTopHotPc(profile, top, 6);

    PS2_LOG("[PS2][PROF] %s hotPC(sample/64): %08lX:%08lX:%lu %08lX:%08lX:%lu %08lX:%08lX:%lu %08lX:%08lX:%lu %08lX:%08lX:%lu %08lX:%08lX:%lu\n",
        label,
        static_cast<unsigned long>(top[0].pc), static_cast<unsigned long>(top[0].opcode), static_cast<unsigned long>(top[0].samples),
        static_cast<unsigned long>(top[1].pc), static_cast<unsigned long>(top[1].opcode), static_cast<unsigned long>(top[1].samples),
        static_cast<unsigned long>(top[2].pc), static_cast<unsigned long>(top[2].opcode), static_cast<unsigned long>(top[2].samples),
        static_cast<unsigned long>(top[3].pc), static_cast<unsigned long>(top[3].opcode), static_cast<unsigned long>(top[3].samples),
        static_cast<unsigned long>(top[4].pc), static_cast<unsigned long>(top[4].opcode), static_cast<unsigned long>(top[4].samples),
        static_cast<unsigned long>(top[5].pc), static_cast<unsigned long>(top[5].opcode), static_cast<unsigned long>(top[5].samples));

    for (int i = 0; i < 6 && top[i].samples; i++)
    {
        const char *kind = "other";
        if (top[i].thumb)
        {
            kind = "thumb";
        }
        else
        {
            Ps2ArmAlu::InstructionInfo info;
            Ps2ArmAlu::inspect(top[i].opcode, top[i].pc, &info);
            kind = armInstructionKindName(info.kind);
        }

        PS2_LOG("[PS2][PROF] %s hot#%d pc=%08lX op=%08lX next=%08lX mode=%s kind=%s samples=%lu cpsr=%08lX r%u=%08lX r%u=%08lX r%u=%08lX mem=%08lX branch=%08lX nextEvent=%lu\n",
            label,
            i,
            static_cast<unsigned long>(top[i].pc),
            static_cast<unsigned long>(top[i].opcode),
            static_cast<unsigned long>(top[i].nextOpcode),
            top[i].thumb ? "THUMB" : "ARM",
            kind,
            static_cast<unsigned long>(top[i].samples),
            static_cast<unsigned long>(top[i].cpsr),
            static_cast<unsigned>(top[i].rn), static_cast<unsigned long>(top[i].rnValue),
            static_cast<unsigned>(top[i].rd), static_cast<unsigned long>(top[i].rdValue),
            static_cast<unsigned>(top[i].rm), static_cast<unsigned long>(top[i].rmValue),
            static_cast<unsigned long>(top[i].memoryAddress),
            static_cast<unsigned long>(top[i].branchTarget),
            static_cast<unsigned long>(top[i].nextEventCycles));
    }
}

#if NOODS_PROFILE_LEVEL >= 2
size_t logDynarecHotBlocks(DynarecR5900 &dynarec,
    const InterpreterProfile &interpreterProfile,
    uint32_t *activeHotPcs = nullptr, size_t activeHotCapacity = 0)
{
    size_t activeHotCount = 0;
    InterpreterHotPc top[6] = {};
    collectTopHotPc(interpreterProfile, top, 6);

    // Hot-PC counts are sorted by frequency, not address. Start the detailed
    // snapshot at the earliest sampled instruction in the same local region,
    // so a tight loop is shown from its actual arithmetic setup rather than
    // from whichever middle instruction won the sampler.
    uint32_t detailPc = top[0].pc;
    for (int hot = 1; hot < 6 && top[hot].samples; hot++)
    {
        uint32_t distance = (top[hot].pc > top[0].pc)
            ? top[hot].pc - top[0].pc : top[0].pc - top[hot].pc;
        if (!top[hot].thumb && distance <= 0x100u && top[hot].pc < detailPc)
            detailPc = top[hot].pc;
    }

    for (int hot = 0; hot < 6 && top[hot].samples; hot++)
    {
        R5900BlockAnalysis analysis;
        if (!dynarec.analyzeArmBlock(top[hot].pc, &analysis, 24))
        {
            PS2_LOG("[PS2][JIT] hotblock pc=%08lX support=0/0 firstUnsupported=unmapped reason=memory\n",
                static_cast<unsigned long>(top[hot].pc));
            continue;
        }

        const R5900AnalyzedInstruction *unsupported = nullptr;
        if (analysis.firstUnsupported < analysis.instructionCount)
            unsupported = &analysis.instructions[analysis.firstUnsupported];
        const char *reason = unsupported
            ? Ps2ArmAlu::rejectionReasonName(unsupported->reason)
            : "supported";

        PS2_LOG("[PS2][JIT] hotblock pc=%08lX support=%u/%u firstUnsupported=%08lX:%08lX reason=%s\n",
            static_cast<unsigned long>(analysis.startPc),
            analysis.supportedInstructions,
            analysis.instructionCount,
            static_cast<unsigned long>(unsupported ? unsupported->pc : 0),
            static_cast<unsigned long>(unsupported ? unsupported->opcode : 0),
            reason);

        if (activeHotPcs && activeHotCount < activeHotCapacity &&
            !top[hot].thumb && top[hot].samples >= 32 &&
            analysis.instructionCount >= 3 &&
            analysis.instructionCount <= 16 &&
            analysis.supportedInstructions == analysis.instructionCount &&
            !analysis.writesMemory &&
            analysis.instructions[analysis.instructionCount - 1]
                .info.terminatesBlock)
        {
            activeHotPcs[activeHotCount++] = analysis.startPc;
        }

        // One bounded detailed snapshot per window; the remaining hot PCs get
        // only the compact support line above.
        if (top[hot].pc != detailPc)
            continue;

        bool hasMemory = false;
        for (uint8_t i = 0; i < analysis.instructionCount; i++)
        {
            Ps2ArmAlu::InstructionKind kind = analysis.instructions[i].info.kind;
            hasMemory |= kind == Ps2ArmAlu::KIND_MEMORY ||
                kind == Ps2ArmAlu::KIND_MULTI_MEMORY;
        }

        uint32_t sampledMemoryAddress = 0xFFFFFFFFu;
        for (int sample = 0; sample < 6 && top[sample].samples; sample++)
        {
            if (top[sample].pc >= analysis.startPc &&
                top[sample].pc < analysis.endPc &&
                top[sample].memoryAddress != 0xFFFFFFFFu)
            {
                sampledMemoryAddress = top[sample].memoryAddress;
                break;
            }
        }
        const char *memoryClass = sampledMemoryAddress != 0xFFFFFFFFu
            ? ndsMemoryClass(sampledMemoryAddress)
            : (hasMemory ? "address-unsampled" : "none");

        uint8_t sampledHalfwordReads = 0;
        uint8_t directHalfwordReads = 0;
        uint8_t sampledHalfwordWrites = 0;
        uint8_t directHalfwordWrites = 0;
        uint8_t sampledWordReads = 0;
        uint8_t directWordReads = 0;
        uint8_t sampledWordWrites = 0;
        uint8_t directWordWrites = 0;
        for (int sample = 0; sample < 6 && top[sample].samples; sample++)
        {
            if (top[sample].pc < analysis.startPc ||
                top[sample].pc >= analysis.endPc)
                continue;

            Ps2ArmAlu::DecodedHalfwordTransfer halfTransfer;
            if (Ps2ArmAlu::decodeHalfwordTransfer(top[sample].opcode,
                &halfTransfer) &&
                top[sample].memoryAddress != 0xFFFFFFFFu)
            {
                bool direct = dynarec.isDirectArmHalfwordAccess(
                    top[sample].opcode, top[sample].memoryAddress);
                if (halfTransfer.load)
                {
                    sampledHalfwordReads++;
                    directHalfwordReads += direct ? 1u : 0u;
                }
                else
                {
                    sampledHalfwordWrites++;
                    directHalfwordWrites += direct ? 1u : 0u;
                }
                continue;
            }

            Ps2ArmAlu::DecodedSingleTransfer wordTransfer;
            if (!Ps2ArmAlu::decodeSingleTransfer(top[sample].opcode,
                &wordTransfer))
                continue;

            uint32_t address = top[sample].memoryAddress;
            if (address == 0xFFFFFFFFu && wordTransfer.registerOffset)
            {
                uint32_t offset = 0;
                if (!Ps2ArmAlu::applyImmediateShift(top[sample].rmValue,
                    wordTransfer.shiftType, wordTransfer.shiftAmount, &offset))
                    continue;
                uint32_t base = top[sample].rnValue;
                uint32_t adjusted = wordTransfer.addOffset
                    ? base + offset : base - offset;
                address = wordTransfer.preIndex ? adjusted : base;
            }
            if (address == 0xFFFFFFFFu)
                continue;

            bool direct = dynarec.isDirectArmWordAccess(
                top[sample].opcode, address);
            if (wordTransfer.load)
            {
                sampledWordReads++;
                directWordReads += direct ? 1u : 0u;
            }
            else
            {
                sampledWordWrites++;
                directWordWrites += direct ? 1u : 0u;
            }
        }

        PS2_LOG("[PS2][HOTBLOCK] cpu=ARM9 pc=%08lX end=%08lX sampledExec=%lu estExec=%lu guestInstr=%u guestCycles=%u branch=%08lX backEdge=%u mem=%08lX memClass=%s dynarecSupported=%u/%u halfDirect=R%u/%u,W%u/%u wordDirect=R%u/%u,W%u/%u read=%04X write=%04X nextEvent=%lu\n",
            static_cast<unsigned long>(analysis.startPc),
            static_cast<unsigned long>(analysis.endPc),
            static_cast<unsigned long>(top[hot].samples),
            static_cast<unsigned long>(top[hot].samples * 64u),
            analysis.instructionCount,
            analysis.instructionCount,
            static_cast<unsigned long>(analysis.branchTarget),
            analysis.backEdge ? 1u : 0u,
            static_cast<unsigned long>(sampledMemoryAddress),
            memoryClass,
            analysis.supportedInstructions,
            analysis.instructionCount,
            directHalfwordReads,
            sampledHalfwordReads,
            directHalfwordWrites,
            sampledHalfwordWrites,
            directWordReads,
            sampledWordReads,
            directWordWrites,
            sampledWordWrites,
            analysis.readMask,
            analysis.writeMask,
            static_cast<unsigned long>(top[hot].nextEventCycles));

        for (uint8_t i = 0; i < analysis.instructionCount; i++)
        {
            const R5900AnalyzedInstruction &instruction = analysis.instructions[i];
            char decoded[112] = {};
            Ps2ArmAlu::format(instruction.opcode, instruction.pc,
                decoded, sizeof(decoded));
            PS2_LOG("[PS2][HOTBLOCK] +%02u pc=%08lX op=%08lX %-42s cond=%X S=%u flagsR=%u flagsW=%u Rn=%u Rd=%u Rm=%u Rs=%u shift=%u/%u imm=%08lX read=%04X write=%04X branch=%08lX back=%u jit=%s\n",
                i,
                static_cast<unsigned long>(instruction.pc),
                static_cast<unsigned long>(instruction.opcode),
                decoded,
                instruction.info.condition,
                instruction.info.writesFlags ? 1u : 0u,
                instruction.info.readsFlags ? 1u : 0u,
                instruction.info.writesFlags ? 1u : 0u,
                instruction.info.rn,
                instruction.info.rd,
                instruction.info.rm,
                instruction.info.rs,
                instruction.info.shiftType,
                instruction.info.registerShift
                    ? instruction.info.rs : instruction.info.shiftAmount,
                static_cast<unsigned long>(instruction.info.immediateValue),
                instruction.info.readMask,
                instruction.info.writeMask,
                static_cast<unsigned long>(instruction.info.branchTarget),
                instruction.info.backEdge ? 1u : 0u,
                Ps2ArmAlu::rejectionReasonName(instruction.reason));
        }
    }

    const R5900DynarecProfile &profile = dynarec.getProfile();
    double coverage = profile.guestInstructions
        ? 100.0 * static_cast<double>(profile.supportedInstructions) /
            static_cast<double>(profile.guestInstructions)
        : 0.0;
    double averageBlock = profile.compileAttempts
        ? static_cast<double>(profile.guestInstructions) /
            static_cast<double>(profile.compileAttempts)
        : 0.0;
    PS2_LOG("[PS2][JIT] shadow attempts=%llu success=%llu rejected=%llu lookupHit=%llu miss=%llu guest=%llu supported=%llu coverage=%.2f%% avgBlock=%.2f largest=%lu hostWords=0 cache=%u invalidations=%llu fallback=%llu\n",
        static_cast<unsigned long long>(profile.compileAttempts),
        static_cast<unsigned long long>(profile.compileSuccess),
        static_cast<unsigned long long>(profile.compileRejected),
        static_cast<unsigned long long>(profile.lookupHits),
        static_cast<unsigned long long>(profile.lookupMisses),
        static_cast<unsigned long long>(profile.guestInstructions),
        static_cast<unsigned long long>(profile.supportedInstructions),
        coverage,
        averageBlock,
        static_cast<unsigned long>(profile.largestBlock),
        static_cast<unsigned int>(dynarec.codeCacheUsed()),
        static_cast<unsigned long long>(profile.mapInvalidations),
        static_cast<unsigned long long>(profile.fallbackCount));
    PS2_LOG("[PS2][JIT] exits: opcode=%llu condition=%llu flags=%llu shift=%llu memory=%llu branch=%llu pc=%llu system=%llu other=%llu\n",
        static_cast<unsigned long long>(profile.rejectReason[Ps2ArmAlu::REJECT_UNSUPPORTED_OPCODE]),
        static_cast<unsigned long long>(profile.rejectReason[Ps2ArmAlu::REJECT_CONDITION]),
        static_cast<unsigned long long>(profile.rejectReason[Ps2ArmAlu::REJECT_FLAGS]),
        static_cast<unsigned long long>(profile.rejectReason[Ps2ArmAlu::REJECT_SHIFT]),
        static_cast<unsigned long long>(profile.rejectReason[Ps2ArmAlu::REJECT_MEMORY]),
        static_cast<unsigned long long>(profile.rejectReason[Ps2ArmAlu::REJECT_BRANCH]),
        static_cast<unsigned long long>(profile.rejectReason[Ps2ArmAlu::REJECT_PC]),
        static_cast<unsigned long long>(profile.rejectReason[Ps2ArmAlu::REJECT_SYSTEM]),
        static_cast<unsigned long long>(profile.rejectReason[Ps2ArmAlu::REJECT_OTHER]));
    PS2_LOG("[PS2][JIT] active attempts=%llu exec=%llu guest=%llu cycles=%llu compileMiss=%llu budgetFallback=%llu writeFallback=%llu verifyFallback=%llu stateFallback=%llu\n",
        static_cast<unsigned long long>(profile.activeAttempts),
        static_cast<unsigned long long>(profile.activeExecutions),
        static_cast<unsigned long long>(profile.activeGuestInstructions),
        static_cast<unsigned long long>(profile.activeCycles),
        static_cast<unsigned long long>(profile.activeCompileMisses),
        static_cast<unsigned long long>(profile.activeBudgetFallbacks),
        static_cast<unsigned long long>(profile.activeWriteFallbacks),
        static_cast<unsigned long long>(profile.activeVerifyFallbacks),
        static_cast<unsigned long long>(profile.activeStateFallbacks));

    uint8_t fallbackOrder[16] = {};
    for (uint8_t i = 0; i < 16; i++)
        fallbackOrder[i] = i;
    for (uint8_t i = 0; i < 16; i++)
    {
        for (uint8_t j = i + 1; j < 16; j++)
        {
            if (profile.topFallback[fallbackOrder[j]].count >
                profile.topFallback[fallbackOrder[i]].count)
            {
                uint8_t swap = fallbackOrder[i];
                fallbackOrder[i] = fallbackOrder[j];
                fallbackOrder[j] = swap;
            }
        }
    }
    for (uint8_t rank = 0; rank < 16; rank++)
    {
        const R5900FallbackProfile &fallback =
            profile.topFallback[fallbackOrder[rank]];
        if (!fallback.count)
            break;
        char decoded[112] = {};
        Ps2ArmAlu::format(fallback.opcode, fallback.pc, decoded,
            sizeof(decoded));
        PS2_LOG("[PS2][JIT] fallback#%u pc=%08lX op=%08lX count=%lu decode=%s reason=%s\n",
            rank,
            static_cast<unsigned long>(fallback.pc),
            static_cast<unsigned long>(fallback.opcode),
            static_cast<unsigned long>(fallback.count),
            decoded,
            Ps2ArmAlu::rejectionReasonName(
                Ps2ArmAlu::rejectionReason(fallback.opcode)));
    }

    return activeHotCount;
}
#endif

void logGpu2DProfile(const char *label, const Gpu2DRunProfile &profile)
{
    auto averageUs = [&](Gpu2DProfilePhase phase) -> double
    {
        uint64_t samples = profile.phaseSamples[phase];
        if (!samples)
            return 0.0;
        return static_cast<double>(profile.phaseTicks[phase]) *
            1000000.0 / CLOCKS_PER_SEC / static_cast<double>(samples);
    };
    auto estimatedMs = [&](Gpu2DProfilePhase phase) -> double
    {
        return averageUs(phase) * static_cast<double>(profile.phaseCount[phase]) / 1000.0;
    };

    PS2_LOG("[PS2][PROF] %s count/avg-us/est-ms: clear=%llu/%.2f/%.2f obj=%llu/%.2f/%.2f text=%llu/%.2f/%.2f\n",
        label,
        static_cast<unsigned long long>(profile.phaseCount[GPU2D_PROFILE_CLEAR]),
        averageUs(GPU2D_PROFILE_CLEAR), estimatedMs(GPU2D_PROFILE_CLEAR),
        static_cast<unsigned long long>(profile.phaseCount[GPU2D_PROFILE_OBJECTS]),
        averageUs(GPU2D_PROFILE_OBJECTS), estimatedMs(GPU2D_PROFILE_OBJECTS),
        static_cast<unsigned long long>(profile.phaseCount[GPU2D_PROFILE_TEXT]),
        averageUs(GPU2D_PROFILE_TEXT), estimatedMs(GPU2D_PROFILE_TEXT));
    PS2_LOG("[PS2][PROF] %s count/avg-us/est-ms: affine=%llu/%.2f/%.2f extended=%llu/%.2f/%.2f large=%llu/%.2f/%.2f\n",
        label,
        static_cast<unsigned long long>(profile.phaseCount[GPU2D_PROFILE_AFFINE]),
        averageUs(GPU2D_PROFILE_AFFINE), estimatedMs(GPU2D_PROFILE_AFFINE),
        static_cast<unsigned long long>(profile.phaseCount[GPU2D_PROFILE_EXTENDED]),
        averageUs(GPU2D_PROFILE_EXTENDED), estimatedMs(GPU2D_PROFILE_EXTENDED),
        static_cast<unsigned long long>(profile.phaseCount[GPU2D_PROFILE_LARGE]),
        averageUs(GPU2D_PROFILE_LARGE), estimatedMs(GPU2D_PROFILE_LARGE));
    PS2_LOG("[PS2][PROF] %s count/avg-us/est-ms: compose=%llu/%.2f/%.2f output=%llu/%.2f/%.2f bright=%llu/%.2f/%.2f\n",
        label,
        static_cast<unsigned long long>(profile.phaseCount[GPU2D_PROFILE_COMPOSE]),
        averageUs(GPU2D_PROFILE_COMPOSE), estimatedMs(GPU2D_PROFILE_COMPOSE),
        static_cast<unsigned long long>(profile.phaseCount[GPU2D_PROFILE_OUTPUT]),
        averageUs(GPU2D_PROFILE_OUTPUT), estimatedMs(GPU2D_PROFILE_OUTPUT),
        static_cast<unsigned long long>(profile.phaseCount[GPU2D_PROFILE_BRIGHTNESS]),
        averageUs(GPU2D_PROFILE_BRIGHTNESS), estimatedMs(GPU2D_PROFILE_BRIGHTNESS));

#if NOODS_PROFILE_LEVEL >= 2
    const char *modeNames[GPU2D_EXTENDED_MODE_COUNT] =
        { "direct", "indexed", "tiled" };
    for (int i = 0; i < GPU2D_EXTENDED_MODE_COUNT; i++)
    {
        Gpu2DExtendedMode mode = static_cast<Gpu2DExtendedMode>(i);
        uint64_t samples = profile.extendedModeSamples[mode];
        if (!profile.extendedModeCount[mode])
            continue;

        double divisor = samples ? static_cast<double>(samples) : 1.0;
        double totalUs = static_cast<double>(profile.extendedModeTicks[mode]) *
            1000000.0 / CLOCKS_PER_SEC / divisor;
        double setupUs = static_cast<double>(profile.extendedSetupTicks[mode]) *
            1000000.0 / CLOCKS_PER_SEC / divisor;
        double bodyUs = static_cast<double>(profile.extendedBodyTicks[mode]) *
            1000000.0 / CLOCKS_PER_SEC / divisor;
        const Gpu2DExtendedCounters &counters = profile.extendedCounters[mode];

        PS2_LOG("[PS2][PROF] %s extended-%s: calls=%llu samples=%llu avg-us=%.2f setup=%.2f body=%.2f sampledPixels=%llu wrap=%llu reject=%llu vram=%llu tileHit=%llu pageHit=%llu palette=%llu transparent=%llu writes=%llu\n",
            label, modeNames[mode],
            static_cast<unsigned long long>(profile.extendedModeCount[mode]),
            static_cast<unsigned long long>(samples),
            totalUs, setupUs, bodyUs,
            static_cast<unsigned long long>(counters.pixels),
            static_cast<unsigned long long>(counters.wrapped),
            static_cast<unsigned long long>(counters.rejected),
            static_cast<unsigned long long>(counters.vramReads),
            static_cast<unsigned long long>(counters.tileCacheHits),
            static_cast<unsigned long long>(counters.directPageHits),
            static_cast<unsigned long long>(counters.paletteReads),
            static_cast<unsigned long long>(counters.transparent),
            static_cast<unsigned long long>(counters.writes));
    }
#endif
}

const char *romIoCallerName(RomIoCaller caller)
{
    switch (caller)
    {
        case ROM_IO_HEADER:      return "ROM-header";
        case ROM_IO_DIRECT_ARM9: return "direct-boot-ARM9";
        case ROM_IO_DIRECT_ARM7: return "direct-boot-ARM7";
        case ROM_IO_CACHE_MISS:  return "ROM-cache-miss";
        case ROM_IO_FIRMWARE:    return "firmware";
        case ROM_IO_BIOS:        return "BIOS";
        case ROM_IO_RUNTIME:     return "runtime-cartridge";
        default:                 return "other";
    }
}

void logRomIoDetail(const char *source, const RomIoProfile &profile)
{
    PS2_LOG("[PS2][PROF] ROM latency hist: <1=%llu 1-5=%llu 5-10=%llu 10-50=%llu 50-100=%llu 100-250=%llu >250=%llu | size <=2K=%llu <=16K=%llu <=64K=%llu <=256K=%llu >256K=%llu\n",
        static_cast<unsigned long long>(profile.latencyHistogram[0]),
        static_cast<unsigned long long>(profile.latencyHistogram[1]),
        static_cast<unsigned long long>(profile.latencyHistogram[2]),
        static_cast<unsigned long long>(profile.latencyHistogram[3]),
        static_cast<unsigned long long>(profile.latencyHistogram[4]),
        static_cast<unsigned long long>(profile.latencyHistogram[5]),
        static_cast<unsigned long long>(profile.latencyHistogram[6]),
        static_cast<unsigned long long>(profile.sizeHistogram[0]),
        static_cast<unsigned long long>(profile.sizeHistogram[1]),
        static_cast<unsigned long long>(profile.sizeHistogram[2]),
        static_cast<unsigned long long>(profile.sizeHistogram[3]),
        static_cast<unsigned long long>(profile.sizeHistogram[4]));

    for (int i = 0; i < ROM_IO_CALLER_COUNT; i++)
    {
        if (!profile.callerReads[i])
            continue;
        PS2_LOG("[PS2][PROF] ROM caller=%s reads=%llu totalMs=%.2f\n",
            romIoCallerName(static_cast<RomIoCaller>(i)),
            static_cast<unsigned long long>(profile.callerReads[i]),
            static_cast<double>(profile.callerTicks[i]) * 1000.0 / CLOCKS_PER_SEC);
    }

    for (uint8_t i = 0; i < profile.slowReadCount; i++)
    {
        const RomSlowRead &slow = profile.slowReads[i];
        PS2_LOG("[PS2][IO-SLOW] source=%s caller=%s offset=%lld requested=%llu got=%llu seekNeeded=%u seekMs=%.2f readMs=%.2f cache=%s sequential=%u posBefore=%lld posAfter=%lld\n",
            source,
            romIoCallerName(slow.caller),
            static_cast<long long>(slow.offset),
            static_cast<unsigned long long>(slow.requested),
            static_cast<unsigned long long>(slow.got),
            slow.seekNeeded ? 1u : 0u,
            static_cast<double>(slow.seekTicks) * 1000.0 / CLOCKS_PER_SEC,
            static_cast<double>(slow.readTicks) * 1000.0 / CLOCKS_PER_SEC,
            slow.cacheMiss ? "miss" : "bypass",
            slow.sequential ? 1u : 0u,
            static_cast<long long>(slow.offset),
            static_cast<long long>(slow.offset + static_cast<int64_t>(slow.got)));
    }
}
#endif
}

int main(int argc, char **argv)
{
    // Keep diagnostics on stdout/emulator logs. The old init_scr() bootstrap
    // held a debug-text framebuffer on screen while slow NDS boot frames ran.
    // The GS frontend now takes over after the first emulated frame instead.
    sceSifInitRpc(0);
    const char *launchPath = (argc > 0 && argv && argv[0] && argv[0][0])
        ? argv[0] : nullptr;
    bool fileXioReady = Ps2File::init(launchPath);
    PS2_LOG("[PS2][HOSTFS] fileXio backend: %s\n",
        fileXioReady ? "ready" : "inactive/unavailable");
    bool padReady = initPs2Pad();
    PS2_LOG("\n");
    PS2_LOG("============================================================\n");
    PS2_LOG(" NooDS-PS2 POC bootstrap\n");
    PS2_LOG(" Base: %s (%s)\n", BASE_COMMIT, BASE_DATE);
    PS2_LOG(" Source HEAD: %.12s (%s)\n", NOODS_BUILD_REVISION, NOODS_BUILD_STATE);
#ifdef DEBUG
    PS2_LOG(" Build: DEBUG%s\n",
#ifdef NOODS_PROFILE
        "+PROFILE"
#else
        ""
#endif
    );
    PS2_LOG(" Compiler: -Og -gdwarf-2 -fno-rtti\n");
#else
    PS2_LOG(" Build: RELEASE%s\n",
#ifdef NOODS_PROFILE
        "+PROFILE"
#else
        ""
#endif
    );
    PS2_LOG(" Compiler: -O2 -fno-rtti -ffunction-sections -fdata-sections\n");
#endif
#ifdef NOODS_PROFILE
    PS2_LOG(" Instrumentation: PROFILE level %d\n", NOODS_PROFILE_LEVEL);
#else
    PS2_LOG(" Instrumentation: disabled\n");
#endif
#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
    PS2_LOG(" Dynarec: ARM-ALU-v15 sparse-state-hotpath (profile L2)\n");
#else
    PS2_LOG(" Dynarec: ARM-ALU-v10 shadow-gated\n");
#endif
    PS2_LOG("============================================================\n");

    if (argc > 0 && argv[0] && argv[0][0])
        PS2_LOG("[PS2][HOSTFS] ELF argv[0]: %s\n", argv[0]);

    std::string requestedRom;
    if (argc > 1 && argv[1] && argv[1][0])
    {
        requestedRom = normalizeRomArgument(argv[1]);
        PS2_LOG("[PS2][HOSTFS] ROM argument: %s -> %s\n",
            argv[1], requestedRom.c_str());
    }
    else
    {
        requestedRom = findHostRomPath();
    }

    std::string resolvedBios9 = findHostFile("bios9.bin", false);
    std::string resolvedBios7 = findHostFile("bios7.bin", false);
    std::string resolvedFirmware = findHostFile("firmware.bin", false);

    std::string romPath = chooseReadablePath("NDS ROM", requestedRom.c_str(), DISC_ROM);
    std::string bios9Path = chooseReadablePath("ARM9 BIOS", resolvedBios9.c_str(), DISC_BIOS9);
    std::string bios7Path = chooseReadablePath("ARM7 BIOS", resolvedBios7.c_str(), DISC_BIOS7);
    std::string firmwarePath = chooseReadablePath("Firmware", resolvedFirmware.c_str(), DISC_FIRMWARE);

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
        PS2_LOG("[PS2] Tried the requested/HostFS path first and disc fallback second.\n");
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
#ifdef NOODS_PROFILE
        clock_t coreConstructStart = clock();
#endif
        Core *core = new Core(romPath, "");
#ifdef NOODS_PROFILE
        clock_t coreConstructTicks = clock() - coreConstructStart;
        const RomIoProfile &constructRomIo = core->cartridgeNds.getRomIoProfile();
        uint64_t constructRomIoTicks = constructRomIo.seekTicks + constructRomIo.readTicks;
        PS2_LOG("[PS2][PROF] core construction: %.2f ms\n",
            static_cast<double>(coreConstructTicks) * 1000.0 / CLOCKS_PER_SEC);
        PS2_LOG("[PS2][PROF] construction ROM I/O: %.2f ms reads=%llu seeks=%llu bytes=%lluKiB maxRead=%.2fms maxSeek=%.2fms short=%llu\n",
            static_cast<double>(constructRomIoTicks) * 1000.0 / CLOCKS_PER_SEC,
            static_cast<unsigned long long>(constructRomIo.readCalls),
            static_cast<unsigned long long>(constructRomIo.seekCalls),
            static_cast<unsigned long long>(constructRomIo.readBytes / 1024u),
            static_cast<double>(constructRomIo.maxReadTicks) * 1000.0 / CLOCKS_PER_SEC,
            static_cast<double>(constructRomIo.maxSeekTicks) * 1000.0 / CLOCKS_PER_SEC,
            static_cast<unsigned long long>(constructRomIo.shortReads));
        logRomIoDetail(core->cartridgeNds.getRomPath().c_str(), constructRomIo);
        core->cartridgeNds.resetRomIoProfile();
#endif

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
        bool arm9DeferredGateEligible = false;
        bool arm9RealBlockGatePassed = false;
        bool arm9DifferentialGatePassed = false;
        bool arm9GateHardFailure = false;
        uint32_t arm9DeferredGateAttempts = 0;
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
                    PS2_LOG("[PS2][OK] ARM->R5900 self-test: 23 base operations + 11 condition/NZCV + 3 branch + 2 direct LDRH/STRH cases passed.\n");

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
                            arm9DeferredGateEligible = true;
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
                                arm9RealBlockGatePassed = true;
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
                                {
                                    arm9DifferentialGatePassed = true;
                                    PS2_LOG("[PS2][OK] ARM9 interpreter<->dynarec differential gate passed: pc=%08lX instructions=%lu hash=%08lX.\n",
                                        static_cast<unsigned long>(diffPc),
                                        static_cast<unsigned long>(diffInstructions),
                                        static_cast<unsigned long>(diffHash));
                                }
                                else if (diffGate == 0)
                                    PS2_LOG("[PS2] ARM9 interpreter<->dynarec differential gate skipped: current cached block is not differential-safe.\n");
                                else
                                {
                                    arm9GateHardFailure = true;
                                    PS2_LOG("[PS2][WARN] ARM9 interpreter<->dynarec differential mismatch; scheduler remains interpreter-only.\n");
                                }
                            }
                            else if (blockGate == 0)
                                PS2_LOG("[PS2] ARM9 real-block shadow gate: no straight-line ALU block available at current PC.\n");
                            else
                            {
                                arm9GateHardFailure = true;
                                PS2_LOG("[PS2][WARN] ARM9 real-block shadow gate mismatch; scheduler remains interpreter-only.\n");
                            }
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

#if defined(NOODS_PROFILE) && NOODS_PROFILE_LEVEL >= 2
        bool arm9ActiveExecutorEnabled = false;
        auto enableArm9ActiveExecutor = [&]()
        {
            if (arm9ActiveExecutorEnabled || !arm9DeferredGateEligible ||
                !arm9RealBlockGatePassed || !arm9DifferentialGatePassed ||
                arm9GateHardFailure)
                return;

            core->setArm9BlockRunner(
                [&dynarec](Interpreter &cpu, uint32_t cycleBudget,
                    uint32_t *guestInstructions) -> int
                {
                    return dynarec.runArm9ActiveBlock(cpu, cycleBudget,
                        guestInstructions);
                });
            arm9ActiveExecutorEnabled = true;
            PS2_LOG("[PS2][OK] ARM9 selective active dynarec armed: only sampled hot entry PCs can call the backend.\n");
        };
#else
        bool arm9ActiveHoldLogged = false;
        auto enableArm9ActiveExecutor = [&]()
        {
            if (arm9ActiveHoldLogged || !arm9DeferredGateEligible ||
                !arm9RealBlockGatePassed || !arm9DifferentialGatePassed ||
                arm9GateHardFailure)
                return;

            arm9ActiveHoldLogged = true;
            PS2_LOG("[PS2][OK] ARM9 active dynarec gates passed; non-L2 builds remain shadow-only.\n");
        };
#endif

        // If boot happened to land on a fully differential-safe block, the
        // initial gates may already be sufficient to enable live execution.
        enableArm9ActiveExecutor();

        auto runDeferredArm9Gate = [&]()
        {
            if (!arm9DeferredGateEligible ||
                (arm9RealBlockGatePassed && arm9DifferentialGatePassed) ||
                arm9GateHardFailure || arm9DeferredGateAttempts >= 16)
                return;

            arm9DeferredGateAttempts++;

            uint32_t blockPc = 0;
            uint32_t blockInstructions = 0;
            uint32_t blockHash = 0;
            int blockGate = dynarec.runArm9BlockShadowSelfTest(
                core->interpreter[0], &blockPc, &blockInstructions, &blockHash);
            if (blockGate == 0)
            {
                if (arm9DeferredGateAttempts == 1 || arm9DeferredGateAttempts == 4 ||
                    arm9DeferredGateAttempts == 8 || arm9DeferredGateAttempts == 16)
                {
                    PS2_LOG("[PS2] ARM9 deferred gate sample %lu/16: current frame-boundary PC has no differential-safe block yet.\n",
                        static_cast<unsigned long>(arm9DeferredGateAttempts));
                }
                return;
            }
            if (blockGate < 0)
            {
                arm9GateHardFailure = true;
                PS2_LOG("[PS2][WARN] ARM9 real-block shadow gate mismatch during deferred sample %lu; scheduler remains interpreter-only.\n",
                    static_cast<unsigned long>(arm9DeferredGateAttempts));
                return;
            }

            arm9RealBlockGatePassed = true;
            const R5900CompiledBlock *compiledBlock = dynarec.findBlock(blockPc, false);
            PS2_LOG("[PS2][OK] ARM9 real-block shadow gate passed: pc=%08lX instructions=%lu blocks=%u cache=%u read=%04X write=%04X hash=%08lX source=deferred.\n",
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
            {
                arm9DifferentialGatePassed = true;
                PS2_LOG("[PS2][OK] ARM9 interpreter<->dynarec differential gate passed: pc=%08lX instructions=%lu hash=%08lX source=deferred.\n",
                    static_cast<unsigned long>(diffPc),
                    static_cast<unsigned long>(diffInstructions),
                    static_cast<unsigned long>(diffHash));
                enableArm9ActiveExecutor();
            }
            else if (diffGate == 0)
            {
                PS2_LOG("[PS2] ARM9 interpreter<->dynarec differential gate skipped: deferred block has memory-write/direct-access constraints; retrying a later frame boundary.\n");
            }
            else
            {
                arm9GateHardFailure = true;
                PS2_LOG("[PS2][WARN] ARM9 interpreter<->dynarec differential mismatch during deferred sample %lu; scheduler remains interpreter-only.\n",
                    static_cast<unsigned long>(arm9DeferredGateAttempts));
            }
        };

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

        // Retrieve only the first completed framebuffer before handing video
        // to gsKit. Older bring-up code waited up to 30 NDS frames for a
        // non-white image, leaving the debug text visible for many seconds on
        // slow games. Continuous streaming below will naturally show later
        // boot/logo frames as soon as the game produces them.
        constexpr size_t FRAME_PIXELS = 256 * 192 * 2;
        constexpr int MAX_BOOT_FRAMES = 1;
        uint32_t *frame = static_cast<uint32_t*>(
            memalign(128, FRAME_PIXELS * sizeof(uint32_t)));
        if (!frame)
            throw std::bad_alloc();

#ifdef NOODS_PROFILE
        clock_t m3Start = clock();
#endif
        uint32_t cyclesBefore = core->getGlobalCycles();
        PS2_LOG("[PS2] M3: running first NDS frame before GS streaming... cycles=%lu\n",
            static_cast<unsigned long>(cyclesBefore));
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
#ifdef NOODS_PROFILE
        clock_t m3ElapsedTicks = clock() - m3Start;
        PS2_LOG("[PS2][PROF] M3 boot frames: %d frames in %.2f ms (%.2f ms/frame)\n",
            selectedFrame,
            static_cast<double>(m3ElapsedTicks) * 1000.0 / CLOCKS_PER_SEC,
            selectedFrame > 0
                ? (static_cast<double>(m3ElapsedTicks) * 1000.0 / CLOCKS_PER_SEC) / selectedFrame
                : 0.0);
#endif

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

        // Both DS screens are contiguous in EE memory. Upload them as one
        // 256x384 texture so gsKit only builds/waits for one GIF DMA transfer
        // per NDS frame instead of two independent 256x192 transfers.
        GSTEXTURE stacked = {};
        stacked.Width = 256;
        stacked.Height = 384;
        stacked.PSM = GS_PSM_CT32;
        stacked.Filter = GS_FILTER_NEAREST;
        static_assert(sizeof(uint32_t) == sizeof(u32), "PS2 GS texture word must be 32-bit");
        stacked.Mem = reinterpret_cast<u32*>(&frame[0]);

        const int textureBytes = gsKit_texture_size(256, 384, GS_PSM_CT32);
        stacked.Vram = gsKit_vram_alloc(gsGlobal, textureBytes, GSKIT_ALLOC_USERBUFFER);

        if (stacked.Vram == GSKIT_ALLOC_ERROR)
        {
            PS2_LOG("[PS2][ERROR] M5: GS VRAM allocation failed.\n");
            free(frame);
            delete core;
            return 9;
        }

        gsKit_texture_upload(gsGlobal, &stacked);

        const u64 blackColor = GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x00, 0x00);
        const u64 textureColor = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00);

        // Native DS pixels, centered in NTSC 640x448 with a small gap.
        // Top:    (192, 24) -> (448, 216)
        // Bottom: (192,232) -> (448, 424)
        auto queueNdsScreens = [&]()
        {
            gsKit_clear(gsGlobal, blackColor);
            gsKit_prim_sprite_texture(gsGlobal, &stacked,
                192.0f, 24.0f, 0.0f, 0.0f,
                448.0f, 216.0f, 256.0f, 192.0f,
                1, textureColor);
            gsKit_prim_sprite_texture(gsGlobal, &stacked,
                192.0f, 232.0f, 0.0f, 192.0f,
                448.0f, 424.0f, 256.0f, 384.0f,
                1, textureColor);
        };

        PS2_LOG("[PS2][OK] M5: both NDS screens uploaded to GS. Priming both display buffers with frame %d.\n",
            selectedFrame);
        fflush(stdout);

        // Paint both GS screen buffers before streaming so the first visible
        // scanout is deterministic and both buffers contain NDS output.
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

#ifdef NOODS_PROFILE
        core->setProfileEnabled(true);
        core->resetProfile();
        dynarec.resetProfile();

        clock_t profCoreTicks = 0;
        clock_t profGetFrameTicks = 0;
        clock_t profUploadTicks = 0;
        clock_t profPresentTicks = 0;
        clock_t profWindowStart = clock();
        int profFrames = 0;
        uint32_t profLastRomHits = core->cartridgeNds.getRomCacheHits();
        uint32_t profLastRomMisses = core->cartridgeNds.getRomCacheMisses();
        uint64_t profLastRomReadBytes = core->cartridgeNds.getRomCacheReadBytes();
        uint32_t profLastRomSeeks = core->cartridgeNds.getRomCacheSeekCount();
        uint32_t profLastSequentialMisses = core->cartridgeNds.getRomCacheSequentialMisses();
#endif

        while (1)
        {
            if (padReady)
                pollPadIntoNds(core);

#ifdef NOODS_PROFILE
            clock_t profTick = clock();
#endif
            core->runFrame();
#ifdef NOODS_PROFILE
            profCoreTicks += clock() - profTick;
#endif
            benchFrames++;
#ifdef NOODS_PROFILE
            profFrames++;
            profTick = clock();
#endif
            bool frameReady = core->gpu.getFrame(frame, false);
#ifdef NOODS_PROFILE
            profGetFrameTicks += clock() - profTick;
#endif

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

#ifdef NOODS_PROFILE
                double profCoreMs = static_cast<double>(profCoreTicks) * 1000.0 / CLOCKS_PER_SEC;
                double profGetMs = static_cast<double>(profGetFrameTicks) * 1000.0 / CLOCKS_PER_SEC;
                double profUploadMs = static_cast<double>(profUploadTicks) * 1000.0 / CLOCKS_PER_SEC;
                double profPresentMs = static_cast<double>(profPresentTicks) * 1000.0 / CLOCKS_PER_SEC;
                double profKnownMs = profCoreMs + profGetMs + profUploadMs + profPresentMs;
                double profFrameDiv = profFrames > 0 ? static_cast<double>(profFrames) : 1.0;

                double profWindowSeconds =
                    static_cast<double>(clock() - profWindowStart) / CLOCKS_PER_SEC;
                double profWindowFps = profWindowSeconds > 0.0
                    ? profFrames / profWindowSeconds : 0.0;

                PS2_LOG("[PS2][PROF] %d-frame window: %.3fs = %.2f fps | core=%.2fms/f getFrame=%.2fms/f uploadGS=%.2fms/f present=%.2fms/f known=%.2fms/f\n",
                    profFrames,
                    profWindowSeconds,
                    profWindowFps,
                    profCoreMs / profFrameDiv,
                    profGetMs / profFrameDiv,
                    profUploadMs / profFrameDiv,
                    profPresentMs / profFrameDiv,
                    profKnownMs / profFrameDiv);

                const CoreRunProfile &runProfile = core->getRunProfile();
                PS2_LOG("[PS2][PROF] scheduler: ARM9=%llu ARM7=%llu tasks=%llu schedules=%llu maxDepth=%lu legacyEstimatedMoves=%llu cursorCompactions=%llu cursorMoves=%llu blockFilterHit=%llu blockExec=%llu blockDrop=%llu blockBudgetSkip=%llu\n",
                    static_cast<unsigned long long>(runProfile.arm9Opcodes),
                    static_cast<unsigned long long>(runProfile.arm7Opcodes),
                    static_cast<unsigned long long>(runProfile.taskExecutions),
                    static_cast<unsigned long long>(runProfile.scheduleCalls),
                    static_cast<unsigned long>(runProfile.maxTaskDepth),
                    static_cast<unsigned long long>(runProfile.estimatedLegacyFrontMoves),
                    static_cast<unsigned long long>(runProfile.queueCompactions),
                    static_cast<unsigned long long>(runProfile.queueCompactionMoves),
                    static_cast<unsigned long long>(runProfile.arm9BlockFilterHits),
                    static_cast<unsigned long long>(runProfile.arm9BlockExecutions),
                    static_cast<unsigned long long>(runProfile.arm9BlockFilterDrops),
                    static_cast<unsigned long long>(runProfile.arm9BlockBudgetSkips));

                double schedulerInsertUs = runProfile.schedulerInsertSamples
                    ? static_cast<double>(runProfile.schedulerInsertTicks) *
                        1000000.0 / CLOCKS_PER_SEC /
                        static_cast<double>(runProfile.schedulerInsertSamples)
                    : 0.0;
                double schedulerPopUs = runProfile.schedulerPopSamples
                    ? static_cast<double>(runProfile.schedulerPopTicks) *
                        1000000.0 / CLOCKS_PER_SEC /
                        static_cast<double>(runProfile.schedulerPopSamples)
                    : 0.0;
                double schedulerCompactionMs =
                    static_cast<double>(runProfile.queueCompactionTicks) *
                    1000.0 / CLOCKS_PER_SEC;
                double schedulerEstimatedMs =
                    (schedulerInsertUs * runProfile.scheduleCalls +
                        schedulerPopUs * runProfile.taskExecutions) / 1000.0 +
                    schedulerCompactionMs;
                PS2_LOG("[PS2][PROF] scheduler-cost: est=%.3fms/f insert=%.3fus(%llu samples) pop=%.3fus(%llu samples) compact=%.3fms ties=%llu\n",
                    schedulerEstimatedMs / profFrameDiv,
                    schedulerInsertUs,
                    static_cast<unsigned long long>(runProfile.schedulerInsertSamples),
                    schedulerPopUs,
                    static_cast<unsigned long long>(runProfile.schedulerPopSamples),
                    schedulerCompactionMs,
                    static_cast<unsigned long long>(runProfile.schedulerTieInserts));

                PS2_LOG("[PS2][PROF] task-count: core=%llu gpuDraw=%llu gpuTiming=%llu gpu3d=%llu spu=%llu dma=%llu timer=%llu irq=%llu other=%llu\n",
                    static_cast<unsigned long long>(runProfile.taskClassCount[TASK_PROFILE_CORE]),
                    static_cast<unsigned long long>(runProfile.taskClassCount[TASK_PROFILE_GPU_DRAW]),
                    static_cast<unsigned long long>(runProfile.taskClassCount[TASK_PROFILE_GPU_TIMING]),
                    static_cast<unsigned long long>(runProfile.taskClassCount[TASK_PROFILE_GPU3D]),
                    static_cast<unsigned long long>(runProfile.taskClassCount[TASK_PROFILE_SPU]),
                    static_cast<unsigned long long>(runProfile.taskClassCount[TASK_PROFILE_DMA]),
                    static_cast<unsigned long long>(runProfile.taskClassCount[TASK_PROFILE_TIMER]),
                    static_cast<unsigned long long>(runProfile.taskClassCount[TASK_PROFILE_IRQ]),
                    static_cast<unsigned long long>(runProfile.taskClassCount[TASK_PROFILE_OTHER]));

                auto sampledTaskUs = [&](TaskProfileClass profileClass) -> double
                {
                    uint64_t samples = runProfile.taskClassSamples[profileClass];
                    if (!samples)
                        return 0.0;
                    return static_cast<double>(runProfile.taskClassTicks[profileClass]) *
                        1000000.0 / CLOCKS_PER_SEC / static_cast<double>(samples);
                };

                PS2_LOG("[PS2][PROF] task-sample-us: core=%.2f gpuDraw=%.2f gpuTiming=%.2f gpu3d=%.2f spu=%.2f dma=%.2f timer=%.2f irq=%.2f other=%.2f (per-class 1/64 sampled)\n",
                    sampledTaskUs(TASK_PROFILE_CORE),
                    sampledTaskUs(TASK_PROFILE_GPU_DRAW),
                    sampledTaskUs(TASK_PROFILE_GPU_TIMING),
                    sampledTaskUs(TASK_PROFILE_GPU3D),
                    sampledTaskUs(TASK_PROFILE_SPU),
                    sampledTaskUs(TASK_PROFILE_DMA),
                    sampledTaskUs(TASK_PROFILE_TIMER),
                    sampledTaskUs(TASK_PROFILE_IRQ),
                    sampledTaskUs(TASK_PROFILE_OTHER));

                const GpuRunProfile &gpuProfile = core->gpu.getProfile();
                auto sampledGpuUs = [&](GpuProfilePhase phase) -> double
                {
                    uint64_t samples = gpuProfile.phaseSamples[phase];
                    if (!samples)
                        return 0.0;
                    return static_cast<double>(gpuProfile.phaseTicks[phase]) *
                        1000000.0 / CLOCKS_PER_SEC / static_cast<double>(samples);
                };

                PS2_LOG("[PS2][PROF] gpu-phase count/avg-us: 2dA=%llu/%.2f 2dB=%llu/%.2f capture=%llu/%.2f render3d=%llu/%.2f vblank=%llu/%.2f\n",
                    static_cast<unsigned long long>(gpuProfile.phaseCount[GPU_PROFILE_2D_A]),
                    sampledGpuUs(GPU_PROFILE_2D_A),
                    static_cast<unsigned long long>(gpuProfile.phaseCount[GPU_PROFILE_2D_B]),
                    sampledGpuUs(GPU_PROFILE_2D_B),
                    static_cast<unsigned long long>(gpuProfile.phaseCount[GPU_PROFILE_CAPTURE]),
                    sampledGpuUs(GPU_PROFILE_CAPTURE),
                    static_cast<unsigned long long>(gpuProfile.phaseCount[GPU_PROFILE_3D_RENDER]),
                    sampledGpuUs(GPU_PROFILE_3D_RENDER),
                    static_cast<unsigned long long>(gpuProfile.phaseCount[GPU_PROFILE_VBLANK]),
                    sampledGpuUs(GPU_PROFILE_VBLANK));

                logGpu2DProfile("gpu2dA", core->gpu2D[0].getProfile());
                logGpu2DProfile("gpu2dB", core->gpu2D[1].getProfile());

                uint32_t currentRomHits = core->cartridgeNds.getRomCacheHits();
                uint32_t currentRomMisses = core->cartridgeNds.getRomCacheMisses();
                uint32_t windowHits = currentRomHits - profLastRomHits;
                uint32_t windowMisses = currentRomMisses - profLastRomMisses;
                uint32_t windowTotal = windowHits + windowMisses;
                double windowHitRate = windowTotal
                    ? (100.0 * static_cast<double>(windowHits) / windowTotal)
                    : 100.0;
                uint64_t currentRomReadBytes = core->cartridgeNds.getRomCacheReadBytes();
                uint32_t currentRomSeeks = core->cartridgeNds.getRomCacheSeekCount();
                uint32_t currentSequentialMisses =
                    core->cartridgeNds.getRomCacheSequentialMisses();
                uint64_t windowReadBytes = currentRomReadBytes - profLastRomReadBytes;
                uint32_t windowSeeks = currentRomSeeks - profLastRomSeeks;
                uint32_t windowSequentialMisses =
                    currentSequentialMisses - profLastSequentialMisses;
                PS2_LOG("[PS2][PROF] ROM window: hits=%lu misses=%lu hitRate=%.2f%% seeks=%lu read=%lluKiB seqMiss=%lu maxSeq=%lu cache=%luKiB page=%luKiB\n",
                    static_cast<unsigned long>(windowHits),
                    static_cast<unsigned long>(windowMisses),
                    windowHitRate,
                    static_cast<unsigned long>(windowSeeks),
                    static_cast<unsigned long long>(windowReadBytes / 1024u),
                    static_cast<unsigned long>(windowSequentialMisses),
                    static_cast<unsigned long>(core->cartridgeNds.getRomCacheSequentialRunMax()),
                    static_cast<unsigned long>(core->cartridgeNds.getRomCacheBytes() / 1024u),
                    static_cast<unsigned long>(core->cartridgeNds.getRomCachePageSize() / 1024u));

                const RomIoProfile &romIo = core->cartridgeNds.getRomIoProfile();
                uint64_t romIoTicks = romIo.seekTicks + romIo.readTicks;
                double romIoMs = static_cast<double>(romIoTicks) * 1000.0 / CLOCKS_PER_SEC;
                double coreExIoMs = (profCoreMs > romIoMs) ? (profCoreMs - romIoMs) : 0.0;
                PS2_LOG("[PS2][PROF] ROM I/O: total=%.2fms (%.2fms/f) coreExIO=%.2fms/f reads=%llu seeks=%llu requested=%lluKiB got=%lluKiB maxRead=%.2fms maxSeek=%.2fms short=%llu\n",
                    romIoMs,
                    romIoMs / profFrameDiv,
                    coreExIoMs / profFrameDiv,
                    static_cast<unsigned long long>(romIo.readCalls),
                    static_cast<unsigned long long>(romIo.seekCalls),
                    static_cast<unsigned long long>(romIo.requestedBytes / 1024u),
                    static_cast<unsigned long long>(romIo.readBytes / 1024u),
                    static_cast<double>(romIo.maxReadTicks) * 1000.0 / CLOCKS_PER_SEC,
                    static_cast<double>(romIo.maxSeekTicks) * 1000.0 / CLOCKS_PER_SEC,
                    static_cast<unsigned long long>(romIo.shortReads));
                logRomIoDetail(core->cartridgeNds.getRomPath().c_str(), romIo);

                profLastRomHits = currentRomHits;
                profLastRomMisses = currentRomMisses;
                profLastRomReadBytes = currentRomReadBytes;
                profLastRomSeeks = currentRomSeeks;
                profLastSequentialMisses = currentSequentialMisses;

                const InterpreterProfile &arm9Profile =
                    core->interpreter[0].getProfile();
                logInterpreterProfile("ARM9", arm9Profile);
#if NOODS_PROFILE_LEVEL >= 2
                uint32_t selectiveHotPcs[3] = {};
                size_t selectiveHotCount = logDynarecHotBlocks(
                    dynarec, arm9Profile, selectiveHotPcs, 3);
#endif
                const InterpreterProfile &arm7Profile =
                    core->interpreter[1].getProfile();
                logInterpreterProfile("ARM7", arm7Profile);
                const MemoryProfile &memoryProfile = core->memory.getProfile();
                logMemoryProfile("ARM9", memoryProfile.cpu[0]);
                logMemoryProfile("ARM7", memoryProfile.cpu[1]);

                InterpreterHotPc summaryHot[1] = {};
                collectTopHotPc(arm9Profile, summaryHot, 1);
                const R5900DynarecProfile &jitProfile = dynarec.getProfile();
                double jitCoverage = jitProfile.guestInstructions
                    ? 100.0 * static_cast<double>(jitProfile.supportedInstructions) /
                        static_cast<double>(jitProfile.guestInstructions)
                    : 0.0;
                double gpu2dAMs = sampledGpuUs(GPU_PROFILE_2D_A) *
                    static_cast<double>(gpuProfile.phaseCount[GPU_PROFILE_2D_A]) /
                    1000.0 / profFrameDiv;
                double gpu2dBMs = sampledGpuUs(GPU_PROFILE_2D_B) *
                    static_cast<double>(gpuProfile.phaseCount[GPU_PROFILE_2D_B]) /
                    1000.0 / profFrameDiv;
                double gpu3dMs = sampledGpuUs(GPU_PROFILE_3D_RENDER) *
                    static_cast<double>(gpuProfile.phaseCount[GPU_PROFILE_3D_RENDER]) /
                    1000.0 / profFrameDiv;
                double spuMs = sampledTaskUs(TASK_PROFILE_SPU) *
                    static_cast<double>(runProfile.taskClassCount[TASK_PROFILE_SPU]) /
                    1000.0 / profFrameDiv;

                PS2_LOG("[PS2][SUMMARY] fps=%.2f coreMs=%.2f coreExIoMs=%.2f romIoMs=%.2f arm9Instr=%llu arm7Instr=%llu gpu2dAMs=%.2f gpu2dBMs=%.2f gpu3dMs=%.2f spuMs=%.2f schedulerMs=%.3f getFrameMs=%.2f gsUploadMs=%.2f presentMs=%.2f hotPc=%08lX hotOpcode=%08lX dynarecCoveragePct=%.2f dynarecActiveGuest=%llu\n",
                    profWindowFps,
                    profCoreMs / profFrameDiv,
                    coreExIoMs / profFrameDiv,
                    romIoMs / profFrameDiv,
                    static_cast<unsigned long long>(runProfile.arm9Opcodes),
                    static_cast<unsigned long long>(arm7Profile.instructions),
                    gpu2dAMs,
                    gpu2dBMs,
                    gpu3dMs,
                    spuMs,
                    schedulerEstimatedMs / profFrameDiv,
                    profGetMs / profFrameDiv,
                    profUploadMs / profFrameDiv,
                    profPresentMs / profFrameDiv,
                    static_cast<unsigned long>(summaryHot[0].pc),
                    static_cast<unsigned long>(summaryHot[0].opcode),
                    jitCoverage,
                    static_cast<unsigned long long>(
                        jitProfile.activeGuestInstructions));

                // Retry the correctness gate only after a complete profile window.
                // Its temporary interpreter execution is then discarded by the
                // profile reset below and cannot distort the measured window.
                runDeferredArm9Gate();

#if NOODS_PROFILE_LEVEL >= 2
                if (arm9ActiveExecutorEnabled && selectiveHotCount)
                {
                    core->addArm9BlockHotPcs(selectiveHotPcs,
                        selectiveHotCount);
                    PS2_LOG("[PS2][JIT] selective hot filter add count=%u pcs=%08lX,%08lX,%08lX\n",
                        static_cast<unsigned>(selectiveHotCount),
                        static_cast<unsigned long>(selectiveHotPcs[0]),
                        static_cast<unsigned long>(selectiveHotPcs[1]),
                        static_cast<unsigned long>(selectiveHotPcs[2]));
                }
#endif

                core->resetProfile();
                dynarec.resetProfile();
                core->cartridgeNds.resetRomIoProfile();
                profCoreTicks = 0;
                profGetFrameTicks = 0;
                profUploadTicks = 0;
                profPresentTicks = 0;
                profWindowStart = clock();
                profFrames = 0;
#endif
#ifndef NOODS_PROFILE
                // RELEASE/DEBUG have no profile-window reset, so retry the
                // gate at the same 60-frame cadence without enabling execution.
                runDeferredArm9Gate();
#endif

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
#ifdef NOODS_PROFILE
            profTick = clock();
#endif
            gsKit_texture_upload(gsGlobal, &stacked);
#ifdef NOODS_PROFILE
            profUploadTicks += clock() - profTick;
            profTick = clock();
#endif
            queueNdsScreens();
            gsKit_queue_exec(gsGlobal);
            gsKit_sync_flip(gsGlobal);
#ifdef NOODS_PROFILE
            profPresentTicks += clock() - profTick;
#endif

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

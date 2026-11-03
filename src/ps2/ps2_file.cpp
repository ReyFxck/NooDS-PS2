/*
    NooDS-PS2 platform file backend
    Copyright 2026 NooDS-PS2 contributors

    Uses PS2SDK fileXio directly for HostFS. This intentionally avoids
    newlib fopen() for Android SAF-backed host: paths: NetherSX2 can enumerate
    those directories yet reject relative stdio opens. fileXio is the same
    PS2-side transport used by established PS2 homebrew HostFS browsers.
*/

#include "ps2_file.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <string>

#include <sifrpc.h>
#include <sbv_patches.h>
#define NEWLIB_PORT_AWARE
#include <io_common.h>
#include <fileXio.h>
#include <fileXio_rpc.h>
#undef NEWLIB_PORT_AWARE
#include <ps2_filesystem_driver.h>

namespace
{
std::string gLaunchPath;
bool gReady = false;

bool buildSafRetryPath(const char *guestPath, std::string &out)
{
    if (!guestPath || std::strncmp(guestPath, "host:", 5) != 0)
        return false;
    if (gLaunchPath.compare(0, 15, "host:content://") != 0)
        return false;

    const char *uri = gLaunchPath.c_str() + 5;
    const char *lastEncodedSlash = nullptr;
    for (const char *p = uri; p[0] && p[1] && p[2]; ++p)
    {
        if (p[0] == '%' && p[1] == '2' && (p[2] == 'F' || p[2] == 'f'))
            lastEncodedSlash = p;
    }
    if (!lastEncodedSlash)
        return false;

    out.assign("host:");
    out.append(uri, static_cast<size_t>(lastEncodedSlash - uri));
    out += "%2F";

    const char *relative = guestPath + 5;
    while (*relative == '/' || *relative == '\\')
        ++relative;

    static const char hex[] = "0123456789ABCDEF";
    for (; *relative; ++relative)
    {
        unsigned char ch = static_cast<unsigned char>(*relative);
        if (ch == '/' || ch == '\\')
        {
            out += "%2F";
        }
        else if ((ch >= 'A' && ch <= 'Z') ||
                 (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') ||
                 ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            out += static_cast<char>(ch);
        }
        else
        {
            out += '%';
            out += hex[(ch >> 4) & 0x0F];
            out += hex[ch & 0x0F];
        }
    }
    return true;
}

int openWithSafRetry(const char *path, int flags, int mode)
{
    if (!gReady || !path)
        return -1;

    int fd = fileXioOpen(path, flags, mode);
    if (fd >= 0 || !Ps2File::isHostPath(path))
        return fd;

    std::string retry;
    if (!buildSafRetryPath(path, retry))
        return fd;

    std::printf("[PS2][HOSTFS][fileXio] relative open failed=%d; retry=%s\n",
        fd, retry.c_str());
    int retryFd = fileXioOpen(retry.c_str(), flags, mode);
    std::printf("[PS2][HOSTFS][fileXio] SAF retry result=%d\n", retryFd);
    return retryFd;
}
}

namespace Ps2File
{
bool init(const char *launchPath)
{
    gLaunchPath = (launchPath && launchPath[0]) ? launchPath : "";
    gReady = false;

    // ISO/CDVD boots keep their existing stdio path and do not need to touch
    // the IOP fileXio stack. HostFS is the only path that needs this backend.
    if (gLaunchPath.compare(0, 5, "host:") != 0)
    {
        std::printf("[PS2][HOSTFS][fileXio] not a host:-launched ELF; backend idle.\n");
        return false;
    }

    sceSifInitRpc(0);

    // ps2_drivers embeds iomanX/fileXio IRX images. Retail LOADFILE needs the
    // standard SBV LoadModuleBuffer patch before those modules can be started.
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    int status = init_fileXio_driver();
    gReady = status >= 0;
    std::printf("[PS2][HOSTFS][fileXio] init status=%d ready=%u launch=%s\n",
        status, gReady ? 1u : 0u, gLaunchPath.c_str());
    return gReady;
}

bool isReady()
{
    return gReady;
}

bool isHostPath(const char *path)
{
    return path && std::strncmp(path, "host:", 5) == 0;
}

int openRead(const char *path)
{
    return openWithSafRetry(path, FIO_O_RDONLY, 0);
}

int openWrite(const char *path)
{
    return openWithSafRetry(path,
        FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
}

int close(int fd)
{
    if (fd < 0)
        return -1;
    return fileXioClose(fd);
}

int read(int fd, void *buffer, size_t size)
{
    if (fd < 0 || !buffer)
        return -1;

    size_t total = 0;
    unsigned char *out = static_cast<unsigned char*>(buffer);
    while (total < size)
    {
        size_t remain = size - total;
        int chunk = remain > static_cast<size_t>(INT_MAX)
            ? INT_MAX : static_cast<int>(remain);
        int got = fileXioRead(fd, out + total, chunk);
        if (got <= 0)
            return total ? static_cast<int>(total) : got;
        total += static_cast<size_t>(got);
        if (got < chunk)
            break;
    }
    return static_cast<int>(total);
}

int write(int fd, const void *buffer, size_t size)
{
    if (fd < 0 || (!buffer && size))
        return -1;

    size_t total = 0;
    const unsigned char *in = static_cast<const unsigned char*>(buffer);
    while (total < size)
    {
        size_t remain = size - total;
        int chunk = remain > static_cast<size_t>(INT_MAX)
            ? INT_MAX : static_cast<int>(remain);
        int wrote = fileXioWrite(fd, in + total, chunk);
        if (wrote <= 0)
            return total ? static_cast<int>(total) : wrote;
        total += static_cast<size_t>(wrote);
        if (wrote < chunk)
            break;
    }
    return static_cast<int>(total);
}

long seek(int fd, long offset, int origin)
{
    if (fd < 0)
        return -1;
    return static_cast<long>(fileXioLseek(fd, static_cast<int>(offset), origin));
}

long tell(int fd)
{
    return seek(fd, 0, SEEK_CUR);
}

long size(int fd)
{
    if (fd < 0)
        return -1;

    long current = tell(fd);
    if (current < 0)
        current = 0;
    long end = seek(fd, 0, SEEK_END);
    if (end >= 0)
        seek(fd, current, SEEK_SET);
    return end;
}

bool canRead(const char *path)
{
    int fd = openRead(path);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

bool readExact(const char *path, void *buffer, size_t size)
{
    int fd = openRead(path);
    if (fd < 0)
        return false;
    int got = read(fd, buffer, size);
    close(fd);
    return got >= 0 && static_cast<size_t>(got) == size;
}
}

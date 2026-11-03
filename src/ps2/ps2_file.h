/*
    NooDS-PS2 platform file backend
    Copyright 2026 NooDS-PS2 contributors

    PS2-only file access kept out of the portable NooDS core interface.
*/

#ifndef NOODS_PS2_FILE_H
#define NOODS_PS2_FILE_H

#include <cstddef>

namespace Ps2File
{
    // Initializes the PS2 fileXio stack only for host:-launched ELFs and
    // remembers argv[0] so Android SAF HostFS can retry sibling document URIs.
    bool init(const char *launchPath);
    bool isReady();
    bool isHostPath(const char *path);

    int openRead(const char *path);
    int openWrite(const char *path);
    int close(int fd);
    int read(int fd, void *buffer, size_t size);
    int write(int fd, const void *buffer, size_t size);
    long seek(int fd, long offset, int origin);
    long tell(int fd);
    long size(int fd);

    bool canRead(const char *path);
    bool readExact(const char *path, void *buffer, size_t size);
}

#endif

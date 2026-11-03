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
#ifdef NOODS_PS2_PORTABLE_TEST
    // Host CI defines NOODS_PS2 to exercise PS2-specific cache/memory paths,
    // but it does not link the EE/IOP fileXio stack. Keep those tests portable
    // while making every HostFS operation explicitly unavailable.
    inline bool init(const char *) { return false; }
    inline bool isReady() { return false; }
    inline bool isHostPath(const char *) { return false; }
    inline int openRead(const char *) { return -1; }
    inline int openWrite(const char *) { return -1; }
    inline int close(int) { return -1; }
    inline int read(int, void *, size_t) { return -1; }
    inline int write(int, const void *, size_t) { return -1; }
    inline long seek(int, long, int) { return -1; }
    inline long tell(int) { return -1; }
    inline long size(int) { return -1; }
    inline bool canRead(const char *) { return false; }
    inline bool readExact(const char *, void *, size_t) { return false; }
#else
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
#endif
}

#endif

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <queue>
#include <string>
#include <vector>
#include <unistd.h>

// Set up a polygon directly while exercising the real renderer.
#define private public
#include "../src/gpu_3d.h"
#undef private
#include "../src/core.h"
#include "../src/settings.h"

static std::string fixture(int size)
{
    char path[] = "/tmp/noods-3d-alpha-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && ftruncate(fd, size) == 0 && close(fd) == 0);
    return path;
}

int main()
{
    const std::string b9 = fixture(0x1000), b7 = fixture(0x4000), fw = fixture(0x40000);
    Settings::setBios9Path(b9); Settings::setBios7Path(b7); Settings::setFirmwarePath(fw);
    Settings::setThreaded2D(0); Settings::setThreaded3D(0);
    Core *core = new Core();
    unlink(b9.c_str()); unlink(b7.c_str()); unlink(fw.c_str());
    core->interpreter[0].halt(7); core->interpreter[1].halt(7);

    core->memory.write<uint8_t>(0, 0x4000240, 0x83); // texture slot 0
    core->memory.write<uint8_t>(0, 0x4000244, 0x83); // palette slot 0
    uint8_t *texture = core->memory.getTex3D()[0];
    uint8_t *palette = core->memory.getPal3D()[0];
    assert(texture && palette);
    std::memset(texture, 0, 64);
    palette[0] = 0xFF; palette[1] = 0x7F;
    Vertex vertices[4] = {};
    const int x[] = {0, 255, 255, 0}, y[] = {0, 0, 192, 192};
    for (int i = 0; i < 4; ++i)
    {
        vertices[i].x = x[i]; vertices[i].y = y[i]; vertices[i].z = 0x100000;
        vertices[i].w = 1024; vertices[i].color = 0x3FFFF;
    }
    core->gpu3D.polygonCountOut = 1;
    _Polygon &polygon = core->gpu3D.getPolygons()[0];
    polygon = _Polygon(); polygon.size = 4; polygon.vertices = vertices;
    polygon.clockwise = true; polygon.alpha = 63; polygon.id = 1;
    polygon.textureFmt = 4; polygon.sizeS = 8; polygon.sizeT = 8;
    polygon.transparent0 = true;
    auto &renderer = core->gpu3DRenderer;
    renderer.writeClearColor(~0u, 0); renderer.writeClearDepth(0xFFFFu, 0x7FFFu);
    renderer.writeDisp3DCnt(0xFFFFu, 0);

    for (int mode = 0; mode <= 2; ++mode)
    {
        polygon.mode = mode;
        renderer.drawScanline(0); renderer.drawScanline(1);
        const uint32_t pixel = renderer.getLine(0)[42];
        if (mode == 1) // Decal retains polygon alpha even for a transparent texel.
            assert(((pixel >> 18) & 63u) == 63u && (pixel & 0x3FFFFu) != 0);
        else // Modulation and toon must leave transparent pixels untouched.
            assert((pixel & 0xFFFFFFu) == 0);
    }
    polygon.mode = 0; polygon.transparent0 = false;
    renderer.drawScanline(0); renderer.drawScanline(1);
    assert(((renderer.getLine(0)[42] >> 18) & 63u) == 63u);

    // A partially transparent A3I5 texel can round to zero only after
    // multiplication; it must still use the original shading decision.
    polygon.textureFmt = 1; polygon.alpha = 1;
    std::memset(texture, 0x20, 64); // alpha3 = 1, palette index 0
    renderer.drawScanline(0); renderer.drawScanline(1);
    assert((renderer.getLine(0)[42] & 0xFFFFFFu) == 0);
    polygon.alpha = 63;
    renderer.drawScanline(0); renderer.drawScanline(1);
    assert(((renderer.getLine(0)[42] >> 18) & 63u) == 9u);
    delete core;
}

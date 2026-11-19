/*
    VU1 GPU 3D Offload Interface for NooDS-PS2
    Manages VU1 microcode execution for vertex transformation and triangle setup
*/

#ifndef VU1_G3D_H
#define VU1_G3D_H

#include <cstdint>
#include <cstring>

class Core;

namespace VU1G3D
{
    // VU1 Memory Map (matches vu1_g3d.s)
    constexpr uint32_t VU1_MEM_BASE = 0x11000000;  // VU1 Data Memory
    constexpr uint32_t VU1_MICROMEM_BASE = 0x11000000;  // VU1 Micro Memory (same physical)
    
    constexpr uint32_t VU1_MTX_STACK = 0x0100;
    constexpr uint32_t VU1_CLIP_MTX = 0x0200;
    constexpr uint32_t VU1_VIEWPORT = 0x0300;
    constexpr uint32_t VU1_VTX_IN = 0x0400;
    constexpr uint32_t VU1_VTX_OUT = 0x1000;
    constexpr uint32_t VU1_VTX_SCREEN = 0x1080;
    constexpr uint32_t VU1_TRI_OUT = 0x1C00;
    constexpr uint32_t VU1_SCRATCH = 0x2000;

    // Microcode program offsets
    constexpr uint32_t PROG_MTX_MUL = 0x0000;
    constexpr uint32_t PROG_VTX_XFORM = 0x0080;
    constexpr uint32_t PROG_TRI_CLIP = 0x0180;

    // Max vertices/polygons
    constexpr int MAX_VERTICES = 192;
    constexpr int MAX_POLYGONS = 64;

    // Vertex format in VU1 (16 bytes = 4 floats)
    struct VertexIn
    {
        float x, y, z, w;
    };

    // VU1 outputs clip-space vertices (no perspective divide, no viewport transform)
    // This matches the original GPU 3D pipeline where swapBuffers() does perspective divide
    struct VertexOut
    {
        float x, y, z, w;  // Clip space (x, y, z, w) - perspective divide happens in swapBuffers()
    };

    // GS Triangle Packet (simplified)
    struct GSTriPacket
    {
        uint64_t giftag;       // GIF tag
        uint32_t rgq;          // RGBAQ
        uint32_t xyz2_0;       // Vertex 0 XYZ2
        uint32_t xyz2_1;       // Vertex 1 XYZ2
        uint32_t xyz2_2;       // Vertex 2 XYZ2
        uint32_t uv_0;         // Vertex 0 UV
        uint32_t uv_1;         // Vertex 1 UV
        uint32_t uv_2;         // Vertex 2 UV
    };

    // Initialize VU1 for GPU 3D
    int init(Core* core);

    // Shutdown VU1
    void shutdown();

    // Check if VU1 offload is available
    bool isAvailable();

    // Matrix operations
    void loadMatrix4x4(int stackIndex, const float* matrix);  // Column-major
    void multiplyMatrix(int dstIndex, int srcAIndex, int srcBIndex);
    void setClipMatrix(const float* matrix);  // Column-major
    void setViewport(float width, float height, float x, float y, float zScale);

    // Vertex transformation (clip matrix * vertex, NO perspective divide)
// Returns number of vertices transformed
    int transformVertices(const VertexIn* vertices, int count, VertexOut* output);

    // Triangle clipping and GS packet generation
    // Returns number of packets generated
    int clipAndGeneratePackets(const VertexOut* vertices, int vtxCount,
                               const uint16_t* indices, int triCount,
                               GSTriPacket* packets);

    // DMA helpers
    void dmaToVU1(uint32_t vu1Addr, const void* eeAddr, uint32_t size);
    void dmaFromVU1(void* eeAddr, uint32_t vu1Addr, uint32_t size);

    // Execute microcode program
    void executeProgram(uint32_t programOffset);
    void waitForVU1();

    // Statistics
    struct Stats
    {
        uint64_t matrixMulCycles = 0;
        uint64_t vertexXformCycles = 0;
        uint64_t triClipCycles = 0;
        uint64_t dmaCycles = 0;
    };
    Stats getStats();
    void resetStats();
}

#endif // VU1_G3D_H
/*
    VU1 GPU 3D Offload Implementation for NooDS-PS2
*/

#include "vu1_g3d.h"
#include <kernel.h>
#include <dma.h>
#include <libvux.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

// VU1 Memory-mapped registers (EE physical addresses)
#define VU1_MEM_BASE        0x11000000  // VU1 Data Memory (16KB)
#define VU1_MICROMEM_BASE   0x11004000  // VU1 Micro Memory (16KB)

#define VU1_STAT_REG        (*(volatile uint32_t*)0x10003C00)  // VU1 Status
#define VU1_FBRST_REG       (*(volatile uint32_t*)0x10003C10)  // VU1 FIFO Busy Reset
#define VU1_MSAR_REG        (*(volatile uint32_t*)0x10003C30)  // VU1 Microprogram Start Address
#define VU1_MSAR            VU1_MSAR_REG
#define VU1_STAT            VU1_STAT_REG
#define VU1_FBRST           VU1_FBRST_REG

// VU1 Microcode programs - assembled from .vsm files with dvp-as
// These are proper VU1 microcode binaries (not MIPS assembly!)

// Matrix Multiply Program (46 instruction pairs = 184 bytes)
// C = A * B (4x4 column-major): A at 0x0100, B at 0x0140, result at 0x0180
// Compiled from VCL via openvcl -> VSM -> dvp-as
static const uint32_t vu1_mtx_mul_program[] = {
    0x10000000, 0x000002FF, 0x01E00100, 0x000002FF, 0x01E40140, 0x000002FF, 0x01E10110, 0x000002FF,
    0x01E20120, 0x000002FF, 0x01E30130, 0x000002FF, 0x01E50150, 0x01E401BC, 0x01E60160, 0x01E408BD,
    0x01E70170, 0x01E410BE, 0x8000033C, 0x01E41A0B, 0x8000033C, 0x01E501BC, 0x8000033C, 0x01E508BD,
    0x8000033C, 0x01E510BE, 0x03E04180, 0x01E51A4B, 0x8000033C, 0x01E601BC, 0x8000033C, 0x01E608BD,
    0x8000033C, 0x01E610BE, 0x03E04990, 0x01E61A8B, 0x8000033C, 0x01E701BC, 0x8000033C, 0x01E708BD,
    0x8000033C, 0x01E710BE, 0x03E051A0, 0x01E71ACB, 0x8000033C, 0x000002FF, 0x8000033C, 0x000002FF,
    0x8000033C, 0x000002FF, 0x03E059B0, 0x000002FF, 0x8000033C, 0x000002FF, 0x8000033C, 0x000002FF,
    0x8000033C, 0x400002FF, 0x8000033C, 0x000002FF,
};
static const uint32_t vu1_mtx_mul_size = sizeof(vu1_mtx_mul_program);

// Vertex Transform Program (32 instruction pairs = 128 bytes)
// Clip space vertex transform (no perspective divide, no viewport)
// Compiled from VCL via openvcl -> VSM -> dvp-as
static const uint32_t vu1_vtx_xform_program[] = {
    0x10000000, 0x000002FF, 0x08210320, 0x000002FF, 0x01EC0200, 0x000002FF, 0x01ED0210, 0x000002FF,
    0x01EE0220, 0x000002FF, 0x01EF0230, 0x000002FF, 0x10020000, 0x000002FF, 0x8000033C, 0x000002FF,
    0x50011012, 0x000002FF, 0x8000033C, 0x000002FF, 0x8000033C, 0x000002FF, 0x01F41400, 0x000002FF,
    0x8000033C, 0x000002FF, 0x8000033C, 0x000002FF, 0x8000033C, 0x000002FF, 0x8000033C, 0x01F461BC,
    0x8000033C, 0x01F468BD, 0x8000033C, 0x01F470BE, 0x8000033C, 0x01F47D4B, 0x8000033C, 0x000002FF,
    0x8000033C, 0x000002FF, 0x8000033C, 0x000002FF, 0x03E2A800, 0x000002FF, 0x10021010, 0x000002FF,
    0x8000033C, 0x000002FF, 0x520117ED, 0x000002FF, 0x8000033C, 0x000002FF, 0x8000033C, 0x000002FF,
    0x8000033C, 0x000002FF, 0x8000033C, 0x400002FF, 0x8000033C, 0x000002FF, 0x00000000, 0x00000000,
};
static const uint32_t vu1_vtx_xform_size = sizeof(vu1_vtx_xform_program);

// Triangle Clip Program (placeholder, 2 instruction pairs = 8 bytes)
static const uint32_t vu1_tri_clip_program[] = {
    0x8000033C, 0x000002FF, 0x8000033C, 0x000002FF,
};
static const uint32_t vu1_tri_clip_size = sizeof(vu1_tri_clip_program);

namespace VU1G3D
{
    static Core* g_core = nullptr;
    static bool g_initialized = false;
    static bool g_vu1_available = false;

    static Stats g_stats;
    static int mulDebugCount = 0;

    // Direct memory copy to VU1 (using aligned 128-bit loads/stores)
    static inline void vu1MemCopy(void* vu1Dest, const void* eeSrc, uint32_t size)
    {
        // Use 128-bit copies for alignment
        uint32_t* dest = (uint32_t*)vu1Dest;
        const uint32_t* src = (const uint32_t*)eeSrc;
        uint32_t qwords = size / 16;
        uint32_t rem = size % 16;

        for (uint32_t i = 0; i < qwords; i++)
        {
            // 4 x 32-bit = 128-bit
            dest[i*4 + 0] = src[i*4 + 0];
            dest[i*4 + 1] = src[i*4 + 1];
            dest[i*4 + 2] = src[i*4 + 2];
            dest[i*4 + 3] = src[i*4 + 3];
        }

        // Handle remainder bytes
        if (rem)
        {
            uint8_t* d = (uint8_t*)(dest + qwords*4);
            const uint8_t* s = (const uint8_t*)(src + qwords*4);
            for (uint32_t i = 0; i < rem; i++)
                d[i] = s[i];
        }
    }

    static inline void vu1MemClear()
    {
        // Clear VU1 data memory (16KB = 4096 words)
        uint32_t* p = (uint32_t*)VU1_MEM_BASE;
        for (int i = 0; i < 4096; i++)
            p[i] = 0;
    }

    int init(Core* core)
    {
        if (g_initialized)
            return 0;

        g_core = core;

        // Reset VU1
        VU1_STAT = 0;
        VU1_FBRST = 1;
        // Small delay after FBRST set
        for (volatile int i = 0; i < 1000; i++);
        VU1_FBRST = 0;
        // Small delay after FBRST clear
        for (volatile int i = 0; i < 1000; i++);

        // Clear VU1 memory
        vu1MemClear();

        // Upload microcode programs to VU1 Micro Memory
        // Program 0: Matrix Multiply at 0x0000
        vu1MemCopy((void*)(VU1_MICROMEM_BASE + PROG_MTX_MUL), vu1_mtx_mul_program, vu1_mtx_mul_size);

        // Program 1: Vertex Transform at 0x0080
        vu1MemCopy((void*)(VU1_MICROMEM_BASE + PROG_VTX_XFORM), vu1_vtx_xform_program, vu1_vtx_xform_size);

        // Program 2: Triangle Clip at 0x0180
        vu1MemCopy((void*)(VU1_MICROMEM_BASE + PROG_TRI_CLIP), vu1_tri_clip_program, vu1_tri_clip_size);

        // Initialize matrix stack to identity
        float identity[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };
        for (int i = 0; i < 16; i++)
        {
            loadMatrix4x4(i, identity);
        }

        // Initialize clip matrix to identity
        setClipMatrix(identity);

        // Default viewport (256x192)
        setViewport(256.0f, 192.0f, 0.0f, 0.0f, 1.0f);

        g_vu1_available = true;
        g_initialized = true;
        return 0;
    }

    void shutdown()
    {
        if (!g_initialized)
            return;

        g_vu1_available = false;
        g_initialized = false;
    }

    bool isAvailable()
    {
        return g_vu1_available;
    }

    void loadMatrix4x4(int stackIndex, const float* matrix)
    {
        if (stackIndex < 0 || stackIndex >= 16)
            return;

        uint32_t vu1Addr = VU1_MTX_STACK + stackIndex * 64;
        vu1MemCopy((void*)(VU1_MEM_BASE + vu1Addr), matrix, 64);
    }

    void multiplyMatrix(int dstIndex, int srcAIndex, int srcBIndex)
    {
        if (dstIndex < 0 || dstIndex >= 16 ||
            srcAIndex < 0 || srcAIndex >= 16 ||
            srcBIndex < 0 || srcBIndex >= 16)
            return;

        // Copy source matrices to temp locations for the microcode
        // Microcode expects A at 0x0100, B at 0x0140, result at 0x0180
        uint32_t srcA = VU1_MTX_STACK + srcAIndex * 64;
        uint32_t srcB = VU1_MTX_STACK + srcBIndex * 64;

        // DEBUG: Read back input matrices before multiply
        if (mulDebugCount < 3)
        {
            float mtxA[16], mtxB[16];
            vu1MemCopy(mtxA, (void*)(VU1_MEM_BASE + srcA), 64);
            vu1MemCopy(mtxB, (void*)(VU1_MEM_BASE + srcB), 64);
            printf("[VU1DEBUG] multiplyMatrix(%d,%d,%d) inputs:\n", dstIndex, srcAIndex, srcBIndex);
            printf("  A (col-major): [%.4f,%.4f,%.4f,%.4f] [%.4f,%.4f,%.4f,%.4f] [%.4f,%.4f,%.4f,%.4f] [%.4f,%.4f,%.4f,%.4f]\n",
                mtxA[0],mtxA[1],mtxA[2],mtxA[3], mtxA[4],mtxA[5],mtxA[6],mtxA[7],
                mtxA[8],mtxA[9],mtxA[10],mtxA[11], mtxA[12],mtxA[13],mtxA[14],mtxA[15]);
            printf("  B (col-major): [%.4f,%.4f,%.4f,%.4f] [%.4f,%.4f,%.4f,%.4f] [%.4f,%.4f,%.4f,%.4f] [%.4f,%.4f,%.4f,%.4f]\n",
                mtxB[0],mtxB[1],mtxB[2],mtxB[3], mtxB[4],mtxB[5],mtxB[6],mtxB[7],
                mtxB[8],mtxB[9],mtxB[10],mtxB[11], mtxB[12],mtxB[13],mtxB[14],mtxB[15]);
            mulDebugCount++;
        }

        // Copy to microcode expected locations
        vu1MemCopy((void*)(VU1_MEM_BASE + 0x0100), (void*)(VU1_MEM_BASE + srcA), 64);
        vu1MemCopy((void*)(VU1_MEM_BASE + 0x0140), (void*)(VU1_MEM_BASE + srcB), 64);

        // Execute matrix multiply program
        // DEBUG: Check VU1 status before execution
        if (mulDebugCount < 3)
        {
            printf("[VU1DEBUG] Before execute: VU1_STAT=0x%08X, MSAR=0x%08X\n", VU1_STAT, VU1_MSAR);
        }
        executeProgram(PROG_MTX_MUL);
        // DEBUG: Check VU1 status after start
        if (mulDebugCount < 3)
        {
            printf("[VU1DEBUG] After start: VU1_STAT=0x%08X\n", VU1_STAT);
        }
        waitForVU1();
        // DEBUG: Check VU1 status after wait
        if (mulDebugCount < 3)
        {
            printf("[VU1DEBUG] After wait: VU1_STAT=0x%08X\n", VU1_STAT);
        }

        // DEBUG: Read back result from microcode output location
        if (mulDebugCount <= 3)
        {
            float result[16];
            vu1MemCopy(result, (void*)(VU1_MEM_BASE + 0x0180), 64);
            printf("[VU1DEBUG] multiplyMatrix result at 0x0180:\n");
            printf("  [%.4f,%.4f,%.4f,%.4f] [%.4f,%.4f,%.4f,%.4f] [%.4f,%.4f,%.4f,%.4f] [%.4f,%.4f,%.4f,%.4f]\n",
                result[0],result[1],result[2],result[3], result[4],result[5],result[6],result[7],
                result[8],result[9],result[10],result[11], result[12],result[13],result[14],result[15]);
        }

        // Copy result back to destination
        uint32_t dst = VU1_MTX_STACK + dstIndex * 64;
        vu1MemCopy((void*)(VU1_MEM_BASE + dst), (void*)(VU1_MEM_BASE + 0x0180), 64);
    }

    void setClipMatrix(const float* matrix)
    {
        vu1MemCopy((void*)(VU1_MEM_BASE + VU1_CLIP_MTX), matrix, 64);
    }

    void setViewport(float width, float height, float x, float y, float zScale)
    {
        // Viewport params in VU1 memory:
        // [width/2, height/2, x_offs, y_offs, zScale, 1.0, 0, 0]
        float params[8] = {
            width * 0.5f,
            height * 0.5f,
            x + width * 0.5f,
            y + height * 0.5f,
            zScale,
            1.0f,
            0.0f,
            0.0f
        };
        vu1MemCopy((void*)(VU1_MEM_BASE + VU1_VIEWPORT), params, 32);
    }

    int transformVertices(const VertexIn* vertices, int count, VertexOut* output)
    {
        if (!g_vu1_available || count <= 0 || count > MAX_VERTICES)
            return 0;

        // Copy vertices to VU1 input buffer
        vu1MemCopy((void*)(VU1_MEM_BASE + VU1_VTX_IN), vertices, count * sizeof(VertexIn));

        // Set vertex count in VU1 memory (at VIEWPORT + 32)
        uint32_t vtxCount[4] = { (uint32_t)count, 0, 0, 0 };
        vu1MemCopy((void*)(VU1_MEM_BASE + VU1_VIEWPORT + 32), vtxCount, 16);

        // Execute vertex transform program
        executeProgram(PROG_VTX_XFORM);
        waitForVU1();

        // Copy results back (clip space vertices at VU1_VTX_OUT)
        vu1MemCopy(output, (void*)(VU1_MEM_BASE + VU1_VTX_OUT), count * sizeof(VertexOut));

        return count;
    }

    int clipAndGeneratePackets(const VertexOut* vertices, int vtxCount,
                               const uint16_t* indices, int triCount,
                               GSTriPacket* packets)
    {
        if (!g_vu1_available || triCount <= 0 || triCount > MAX_POLYGONS)
            return 0;

        // TODO: Implement triangle clipping and GS packet generation
        // For now, return 0 (fallback to software)
        return 0;
    }

    void dmaToVU1(uint32_t vu1Addr, const void* eeAddr, uint32_t size)
    {
        vu1MemCopy((void*)(VU1_MEM_BASE + vu1Addr), eeAddr, size);
    }

    void dmaFromVU1(void* eeAddr, uint32_t vu1Addr, uint32_t size)
    {
        // Copy from VU1 to CPU (reverse of vu1MemCopy)
        uint32_t* dest = (uint32_t*)eeAddr;
        const uint32_t* src = (const uint32_t*)(VU1_MEM_BASE + vu1Addr);
        uint32_t qwords = size / 16;
        uint32_t rem = size % 16;

        for (uint32_t i = 0; i < qwords; i++)
        {
            dest[i*4 + 0] = src[i*4 + 0];
            dest[i*4 + 1] = src[i*4 + 1];
            dest[i*4 + 2] = src[i*4 + 2];
            dest[i*4 + 3] = src[i*4 + 3];
        }

        if (rem)
        {
            uint8_t* d = (uint8_t*)(dest + qwords*4);
            const uint8_t* s = (uint8_t*)(src + qwords*4);
            for (uint32_t i = 0; i < rem; i++)
                d[i] = s[i];
        }
    }

    void executeProgram(uint32_t programOffset)
    {
        // Start VU1 microcode at programOffset (byte offset → instruction address)
        // MSAR register expects instruction index (byte_offset / 4)
        VU1_MSAR = programOffset >> 2;
        // Write RUN bit directly (single write, not read-modify-write)
        VU1_STAT = 0x100;
        // DEBUG: Read back immediately
        if (mulDebugCount < 3)
        {
            uint32_t stat = VU1_STAT;
            uint32_t msar = VU1_MSAR;
            printf("[VU1DEBUG] executeProgram: wrote STAT=0x100, read STAT=0x%08lX, MSAR=0x%08lX\n", (unsigned long)stat, (unsigned long)msar);
        }
    }

    void waitForVU1()
    {
        // Wait for VU1 to finish
        while (VU1_STAT & 0x100)
        {
            // VU1 is running
        }
    }

    Stats getStats()
    {
        return g_stats;
    }

    void resetStats()
    {
        g_stats = Stats{};
    }
}
/*
    VU1 Microcode for NooDS-PS2 GPU 3D Offload (Executable programs only)
    
    VU1 Memory Layout (16 KB = 0x4000 bytes):
    0x0000-0x00FF:  VU1 Microcode (256 instructions max)
    0x0100-0x01FF:  Matrix stack (16 matrices * 64 bytes = 1024 bytes)
    0x0200-0x02FF:  Clip matrix (64 bytes)
    0x0300-0x03FF:  Viewport params (64 bytes)
    0x0400-0x0FFF:  Vertex buffer IN (3072 bytes = 192 vertices * 16 bytes)
    0x1000-0x1BFF:  Vertex buffer OUT (3072 bytes)
    0x1C00-0x1FFF:  Triangle packets OUT (1024 bytes = 64 triangles)
    0x2000-0x3FFF:  Scratch / temp
    
    VU1 Registers (VF0-VF31):
    VF0-VF3:  Matrix A columns (for mul)
    VF4-VF7:  Matrix B columns
    VF8-VF11: Result matrix columns
    VF12-VF15: Clip matrix columns
    VF16-VF19: Viewport params
    VF20-VF23: Vertex IN/OUT
    VF24-VF27: Clipping planes
    VF28-VF31: Temp / constants
*/

// ============================================================
// VU1 MICROCODE PROGRAMS (executable, in .text section)
// ============================================================

.section .text
.align 4

// Program 0: Matrix Multiply (4x4 * 4x4) - Column Major
// Input:  Matrix A at 0x0100, Matrix B at 0x0140
// Output: Result at 0x0180
// VF0-VF3 = A cols, VF4-VF7 = B cols, VF8-VF11 = Result cols

.global vu1_mtx_mul_start
vu1_mtx_mul_start:
    # Load Matrix A (4 columns, 16 bytes each = 64 bytes)
    lqc2    vf0, 0x0100(zero)    # A col 0
    lqc2    vf1, 0x0110(zero)    # A col 1
    lqc2    vf2, 0x0120(zero)    # A col 2
    lqc2    vf3, 0x0130(zero)    # A col 3

    # Load Matrix B (4 columns)
    lqc2    vf4, 0x0140(zero)    # B col 0
    lqc2    vf5, 0x0150(zero)    # B col 1
    lqc2    vf6, 0x0160(zero)    # B col 2
    lqc2    vf7, 0x0170(zero)    # B col 3

    # Result col 0 = A * B_col0
    vmulq.xyzw vf8, vf0, vf4[x]  # A_col0 * B00
    vmaddq.xyzw vf8, vf1, vf4[y] # + A_col1 * B10
    vmaddq.xyzw vf8, vf2, vf4[z] # + A_col2 * B20
    vmaddq.xyzw vf8, vf3, vf4[w] # + A_col3 * B30

    # Result col 1 = A * B_col1
    vmulq.xyzw vf9, vf0, vf5[x]
    vmaddq.xyzw vf9, vf1, vf5[y]
    vmaddq.xyzw vf9, vf2, vf5[z]
    vmaddq.xyzw vf9, vf3, vf5[w]

    # Result col 2 = A * B_col2
    vmulq.xyzw vf10, vf0, vf6[x]
    vmaddq.xyzw vf10, vf1, vf6[y]
    vmaddq.xyzw vf10, vf2, vf6[z]
    vmaddq.xyzw vf10, vf3, vf6[w]

    # Result col 3 = A * B_col3
    vmulq.xyzw vf11, vf0, vf7[x]
    vmaddq.xyzw vf11, vf1, vf7[y]
    vmaddq.xyzw vf11, vf2, vf7[z]
    vmaddq.xyzw vf11, vf3, vf7[w]

    # Store result
    sqc2    vf8, 0x0180(zero)
    sqc2    vf9, 0x0190(zero)
    sqc2    vf10, 0x01A0(zero)
    sqc2    vf11, 0x01B0(zero)

    nop
    nop

.global vu1_mtx_mul_end
vu1_mtx_mul_end:

// Program 1: Vertex Transform (Clip Matrix * Vertex)
// Input:  Clip matrix at 0x0200, Vertices at 0x0400 (count at 0x0320)
// Output: Clip space vertices at 0x1000 (x, y, z, w) - NO perspective divide
// Process up to 192 vertices

.global vu1_vtx_xform_start
vu1_vtx_xform_start:
    # Load clip matrix (4 columns)
    lqc2    vf12, 0x0200(zero)   # Clip col 0
    lqc2    vf13, 0x0210(zero)   # Clip col 1
    lqc2    vf14, 0x0220(zero)   # Clip col 2
    lqc2    vf15, 0x0230(zero)   # Clip col 3

    # Load vertex count (at 0x0300+32 = 0x0320)
    lqc2    vi00, 0x0320(zero)   # vi00.x = vertex_count
    iaddiu  vi01, vi00, 0        # vi01 = vertex_count
    iaddiu  vi02, zero, 0        # vi02 = vertex index (0)

    # Loop over vertices
vtx_loop:
    beq     vi01, vi02, vtx_done # if index >= count, exit
    nop

    # Load vertex (x, y, z, w) - 16 bytes
    lqc2    vf20, 0x0400(vi02)   # vertex in (x, y, z, w)

    # Transform: v' = Clip * v (clip space, keep w for later perspective divide)
    vmulq.xyzw vf21, vf12, vf20[x]  # col0 * v.x
    vmaddq.xyzw vf21, vf13, vf20[y] # + col1 * v.y
    vmaddq.xyzw vf21, vf14, vf20[z] # + col2 * v.z
    vmaddq.xyzw vf21, vf15, vf20[w] # + col3 * v.w

    # Store clip space vertex (x, y, z, w) - NO perspective divide yet
    sqc2    vf21, 0x1000(vi02)

    iaddiu  vi02, vi02, 16       # Next vertex (16 bytes)
    j       vtx_loop
    nop

vtx_done:
    nop
    nop

.global vu1_vtx_xform_end
vu1_vtx_xform_end:

// Program 2: Triangle Clipping + GS Packet Generation (placeholder)
.global vu1_tri_clip_start
vu1_tri_clip_start:
    nop
    nop

.global vu1_tri_clip_end
vu1_tri_clip_end:

// ============================================================
// PROGRAM SIZE CONSTANTS (for C++ to calculate sizes)
// ============================================================
.global VU1_MTX_MUL_SIZE
VU1_MTX_MUL_SIZE = vu1_mtx_mul_end - vu1_mtx_mul_start

.global VU1_VTX_XFORM_SIZE
VU1_VTX_XFORM_SIZE = vu1_vtx_xform_end - vu1_vtx_xform_start

.global VU1_TRI_CLIP_SIZE
VU1_TRI_CLIP_SIZE = vu1_tri_clip_end - vu1_tri_clip_start
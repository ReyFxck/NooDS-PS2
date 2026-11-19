/*
    VU0 Matrix Microcode for NooDS-PS2
    Offloads 4x4 matrix operations (multiply, vector transform) to VU0
*/

#include <kernel.h>
#include <dmacman.h>
#include <vu0.h>
#include <vu0mac.h>
#include <vu0dma.h>
#include <libvu0.h>
#include <stdio.h>
#include <string.h>

#include "../../defines.h"
#include "vu_matrix.h"

// VU0 microcode for 4x4 matrix multiplication (fixed-point 20.12)
// Input: Two 4x4 matrices in VU0 data memory
// Output: Result matrix in VU0 data memory
static VU0_ALIGN(16) uint32_t vu0_matrix_mul_program[] = {
    // VU0 microcode for matrix multiplication
    // This is a placeholder - actual microcode would be written in VU assembly
    0x00000000, // NOP
    0x00000000, // NOP
    0x00000000, // NOP
    0x00000000, // NOP
};

// VU0 data memory layout for matrix operations
// Offset 0x000-0x03F: Matrix A (4x4, 16 elements, 4 bytes each = 64 bytes)
// Offset 0x040-0x07F: Matrix B (4x4)
// Offset 0x080-0x0BF: Result matrix (4x4)
// Offset 0x0C0-0x0FF: Temporary storage

#define VU0_MATRIX_A_OFFSET    0x000
#define VU0_MATRIX_B_OFFSET    0x040
#define VU0_MATRIX_R_OFFSET    0x080
#define VU0_TEMP_OFFSET        0x0C0

// VU0 microcode program address
#define VU0_PROGRAM_ADDR       0x0000

static bool vu0_initialized = false;
static int vu0_matrix_mul_ms = 0;

// Initialize VU0 for matrix operations
int vu0MatrixInit()
{
    if (vu0_initialized)
        return 0;

    // Reset VU0
    *vu0_MUL = 0;
    *vu0_DIV = 0;
    *vu0_IFS = 0;
    *vu0_IFC = 0;
    *vu0_FBRST = 1;
    *vu0_FBRST = 0;

    // Clear VU0 memory
    vu0MemClear();

    // Upload microcode program
    vu0MemCopy((void*)VU0_PROGRAM_ADDR, vu0_matrix_mul_program, sizeof(vu0_matrix_mul_program));

    vu0_initialized = true;
    return 0;
}

// Convert fixed-point 20.12 to float
static inline float fixedToFloat(int32_t val)
{
    return (float)val / 4096.0f;
}

// Convert float to fixed-point 20.12
static inline int32_t floatToFixed(float val)
{
    return (int32_t)(val * 4096.0f + (val >= 0 ? 0.5f : -0.5f));
}

// Multiply two 4x4 matrices using VU0 (fixed-point 20.12)
// a, b, result: pointers to 16 int32_t elements (row-major)
int vu0MatrixMultiply4x4(const int32_t *a, const int32_t *b, int32_t *result)
{
    if (!vu0_initialized)
        vu0MatrixInit();

    // Convert matrices to float and upload to VU0
    // VU0 expects column-major format for efficient vector operations
    float matA[16], matB[16];
    
    for (int i = 0; i < 16; i++) {
        matA[i] = fixedToFloat(a[i]);
        matB[i] = fixedToFloat(b[i]);
    }

    // Transpose to column-major for VU0
    float matA_col[16], matB_col[16];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            matA_col[col * 4 + row] = matA[row * 4 + col];
            matB_col[col * 4 + row] = matB[row * 4 + col];
        }
    }

    // Upload matrices to VU0 data memory
    vu0MemCopy((void*)VU0_MATRIX_A_OFFSET, matA_col, 64);
    vu0MemCopy((void*)VU0_MATRIX_B_OFFSET, matB_col, 64);

    // Execute microcode program
    // TODO: Implement actual VU0 microcode execution
    // For now, fall back to CPU implementation
    
    // CPU fallback (same as original Matrix::operator*)
    Matrix tempA, tempB;
    for (int i = 0; i < 16; i++) {
        tempA.data[i] = a[i];
        tempB.data[i] = b[i];
    }
    Matrix resultM = tempA * tempB;
    for (int i = 0; i < 16; i++) {
        result[i] = resultM.data[i];
    }

    return 0;
}

// Transform a vertex by a 4x4 matrix using VU0
// vertex: 4 components (x, y, z, w) in fixed-point 20.12
// matrix: 4x4 matrix in fixed-point 20.12 (row-major)
// result: 4 components output
int vu0TransformVertex(const int32_t *vertex, const int32_t *matrix, int32_t *result)
{
    if (!vu0_initialized)
        vu0MatrixInit();

    // CPU fallback (same as original Vertex::operator*)
    result[0] = ((int64_t)vertex[0] * matrix[0]  + (int64_t)vertex[1] * matrix[4]  + 
                 (int64_t)vertex[2] * matrix[8]  + (int64_t)vertex[3] * matrix[12]) >> 12;
    result[1] = ((int64_t)vertex[0] * matrix[1]  + (int64_t)vertex[1] * matrix[5]  + 
                 (int64_t)vertex[2] * matrix[9]  + (int64_t)vertex[3] * matrix[13]) >> 12;
    result[2] = ((int64_t)vertex[0] * matrix[2]  + (int64_t)vertex[1] * matrix[6]  + 
                 (int64_t)vertex[2] * matrix[10] + (int64_t)vertex[3] * matrix[14]) >> 12;
    result[3] = ((int64_t)vertex[0] * matrix[3]  + (int64_t)vertex[1] * matrix[7]  + 
                 (int64_t)vertex[2] * matrix[11] + (int64_t)vertex[3] * matrix[15]) >> 12;

    return 0;
}

// Batch transform multiple vertices by the same matrix
int vu0TransformVertices(const int32_t *vertices, int count, const int32_t *matrix, int32_t *results)
{
    for (int i = 0; i < count; i++) {
        vu0TransformVertex(&vertices[i * 4], matrix, &results[i * 4]);
    }
    return 0;
}

// Get VU0 timing statistics
int vu0GetMatrixMulTime()
{
    return vu0_matrix_mul_ms;
}
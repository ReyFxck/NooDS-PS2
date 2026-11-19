/*
    VU0 Matrix Microcode Interface for NooDS-PS2
    Provides hardware-accelerated 4x4 matrix operations using VU0
*/

#ifndef VU_MATRIX_H
#define VU_MATRIX_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize VU0 for matrix operations
int vu0MatrixInit();

// Multiply two 4x4 matrices using VU0 (fixed-point 20.12)
// Matrices are in row-major format: [row0_col0, row0_col1, row0_col2, row0_col3, row1_col0, ...]
// a, b, result: pointers to 16 int32_t elements
int vu0MatrixMultiply4x4(const int32_t *a, const int32_t *b, int32_t *result);

// Transform a vertex by a 4x4 matrix using VU0
// vertex: 4 components (x, y, z, w) in fixed-point 20.12
// matrix: 4x4 matrix in fixed-point 20.12 (row-major)
// result: 4 components output
int vu0TransformVertex(const int32_t *vertex, const int32_t *matrix, int32_t *result);

// Batch transform multiple vertices by the same matrix
// vertices: array of vertices (4 components each)
// count: number of vertices
// matrix: 4x4 matrix (row-major)
// results: output array (4 components per vertex)
int vu0TransformVertices(const int32_t *vertices, int count, const int32_t *matrix, int32_t *results);

// Get VU0 timing statistics (microseconds)
int vu0GetMatrixMulTime();

#ifdef __cplusplus
}
#endif

#endif // VU_MATRIX_H
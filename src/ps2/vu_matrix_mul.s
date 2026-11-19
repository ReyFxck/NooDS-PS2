/*
    VU0 Microcode for 4x4 Matrix Multiplication
    Fixed-point 20.12 format (int32_t)
    
    This microcode runs on VU0 and performs C = A * B
    where A, B, C are 4x4 matrices in column-major format.
    
    Input:  VU0 data memory at MATRIX_A (0x000) - 16 floats (column-major)
            VU0 data memory at MATRIX_B (0x040) - 16 floats (column-major)
    Output: VU0 data memory at MATRIX_R (0x080) - 16 floats (column-major)
    
    VU0 has 32 vector registers (VF0-VF31), each 128-bit (4 floats)
    We use VF0-VF3 for matrix A columns, VF4-VF7 for matrix B columns
    VF8-VF11 for result columns
*/

.section .text
.align 4

// VU0 microcode entry point
.global vu0_matrix_mul_start
vu0_matrix_mul_start:

    // Load matrix A columns into VF0-VF3
    // A is stored in column-major: col0, col1, col2, col3
    lqc2    vf0, 0x000(zero)    // Load column 0 of A
    lqc2    vf1, 0x010(zero)    // Load column 1 of A
    lqc2    vf2, 0x020(zero)    // Load column 2 of A
    lqc2    vf3, 0x030(zero)    // Load column 3 of A

    // Load matrix B columns into VF4-VF7
    lqc2    vf4, 0x040(zero)    // Load column 0 of B
    lqc2    vf5, 0x050(zero)    // Load column 1 of B
    lqc2    vf6, 0x060(zero)    // Load column 2 of B
    lqc2    vf7, 0x070(zero)    // Load column 3 of B

    // Matrix multiplication: C = A * B
    // For column-major: C_col[j] = A * B_col[j]
    // C_col[j][i] = sum_k A[i][k] * B[k][j]
    // In column-major: C_col[j] = A_row0 * B_col[j][0] + A_row1 * B_col[j][1] + ...
    
    // We need to compute each result column
    // Result column 0 = A * B_col0
    // Result column 1 = A * B_col1
    // Result column 2 = A * B_col2
    // Result column 3 = A * B_col3
    
    // Use vf8-vf11 for result columns
    // Initialize to zero
    vmulq.xyzw vf8, vf0, vf0[w]  // vf8 = 0 (multiply by w=0)
    vmulq.xyzw vf9, vf0, vf0[w]  // vf9 = 0
    vmulq.xyzw vf10, vf0, vf0[w] // vf10 = 0
    vmulq.xyzw vf11, vf0, vf0[w] // vf11 = 0

    // Multiply-accumulate: result_col = sum_k A_col[k] * B_col[j][k]
    // But A is stored as columns, so we need rows of A.
    // Better approach: Transpose A on the fly or use different layout.
    
    // For column-major A and B:
    // C_col[j] = A * B_col[j] = sum_i A_col[i] * B_col[j][i]
    // where A_col[i] is column i of A, and B_col[j][i] is element (i,j) of B
    
    // Extract B matrix elements (rows) from column-major storage
    // B_col0 = (B00, B10, B20, B30)
    // B_col1 = (B01, B11, B21, B31)
    // B_col2 = (B02, B12, B22, B32)
    // B_col3 = (B03, B13, B23, B33)
    
    // We need rows of B for the multiplication
    // Use vf12-vf15 for B rows
    
    // Transpose B from column-major to row-major in registers
    // Row 0 of B = (B00, B01, B02, B03) = (vf4.x, vf5.x, vf6.x, vf7.x)
    vmove.xyzw vf12, vf4
    vmove.xyzw vf13, vf5
    vmove.xyzw vf14, vf6
    vmove.xyzw vf15, vf7
    
    // Now we need to shuffle to get rows
    // This is complex - let's use a different approach
    
    // Actually, for C = A * B with column-major:
    // C[i][j] = sum_k A[i][k] * B[k][j]
    // In column-major storage:
    // A_col[k] = (A[0][k], A[1][k], A[2][k], A[3][k])
    // B_col[j] = (B[0][j], B[1][j], B[2][j], B[3][j])
    // C_col[j] = (C[0][j], C[1][j], C[2][j], C[3][j])
    // C_col[j] = A_col[0]*B[0][j] + A_col[1]*B[1][j] + A_col[2]*B[2][j] + A_col[3]*B[3][j]
    //          = vf0 * vf4[j] + vf1 * vf5[j] + vf2 * vf6[j] + vf3 * vf7[j]
    
    // So for each result column j:
    // result_col = vf0 * B[0][j] + vf1 * B[1][j] + vf2 * B[2][j] + vf3 * B[3][j]
    // Where B[0][j] is the x component of vf4 for j=0, vf5 for j=1, etc.
    
    // Let's compute result column 0 (j=0):
    // B[0][0] = vf4.x, B[1][0] = vf4.y, B[2][0] = vf4.z, B[3][0] = vf4.w
    vmulq.xyzw vf8, vf0, vf4[x]  // vf8 = A_col0 * B00
    vmaddq.xyzw vf8, vf1, vf4[y] // vf8 += A_col1 * B10
    vmaddq.xyzw vf8, vf2, vf4[z] // vf8 += A_col2 * B20
    vmaddq.xyzw vf8, vf3, vf4[w] // vf8 += A_col3 * B30
    
    // Result column 1 (j=1):
    // B[0][1] = vf5.x, B[1][1] = vf5.y, B[2][1] = vf5.z, B[3][1] = vf5.w
    vmulq.xyzw vf9, vf0, vf5[x]
    vmaddq.xyzw vf9, vf1, vf5[y]
    vmaddq.xyzw vf9, vf2, vf5[z]
    vmaddq.xyzw vf9, vf3, vf5[w]
    
    // Result column 2 (j=2):
    vmulq.xyzw vf10, vf0, vf6[x]
    vmaddq.xyzw vf10, vf1, vf6[y]
    vmaddq.xyzw vf10, vf2, vf6[z]
    vmaddq.xyzw vf10, vf3, vf6[w]
    
    // Result column 3 (j=3):
    vmulq.xyzw vf11, vf0, vf7[x]
    vmaddq.xyzw vf11, vf1, vf7[y]
    vmaddq.xyzw vf11, vf2, vf7[z]
    vmaddq.xyzw vf11, vf3, vf7[w]
    
    // Store result columns to VU0 memory
    sqc2    vf8, 0x080(zero)    // Store result column 0
    sqc2    vf9, 0x090(zero)    // Store result column 1
    sqc2    vf10, 0x0A0(zero)   // Store result column 2
    sqc2    vf11, 0x0B0(zero)   // Store result column 3
    
    // End program
    nop
    nop

// Microcode size
.global vu0_matrix_mul_size
vu0_matrix_mul_size = . - vu0_matrix_mul_start

// Data offsets
.equ MATRIX_A_OFFSET, 0x000
.equ MATRIX_B_OFFSET, 0x040
.equ MATRIX_R_OFFSET, 0x080
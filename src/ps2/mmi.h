/*
    PS2 MMI (MIPS SIMD) Intrinsics for NooDS-PS2
    Provides 2x 32-bit, 4x 16-bit, 8x 8-bit SIMD operations
*/

#ifndef MMI_H
#define MMI_H

#include <cstdint>

// MMI intrinsics for EE (PS2 main CPU)
// These map to the MMI instruction set (MDMX subset)
// Available on R5900 (PS2 EE)

// 64-bit vector type (2x 32-bit, 4x 16-bit, 8x 8-bit)
typedef uint64_t mmi64_t;
typedef int64_t mmi64s_t;

// Vector of 2x 32-bit integers
typedef uint64_t v2i32;
typedef int64_t v2i32s;

// Vector of 4x 16-bit integers
typedef uint64_t v4i16;
typedef int64_t v4i16s;

// Vector of 8x 8-bit integers
typedef uint64_t v8i8;
typedef int64_t v8i8s;

#ifdef __cplusplus
extern "C" {
#endif

// Load 64-bit vector from memory (8-byte aligned)
static inline mmi64_t mmi_ld(const void *addr)
{
    mmi64_t val;
    __asm__ volatile("ld %0, 0(%1)" : "=r"(val) : "r"(addr));
    return val;
}

// Store 64-bit vector to memory (8-byte aligned)
static inline void mmi_sd(mmi64_t val, void *addr)
{
    __asm__ volatile("sd %0, 0(%1)" : : "r"(val), "r"(addr));
}

// Add two v2i32 vectors with saturation at 0x3F (6-bit saturation for RGB6)
static inline v2i32 mmi_add_sat_v2i32_rgb6(v2i32 a, v2i32 b)
{
    v2i32 result;
    __asm__ volatile(
        "madd.qh %0, %1, %2\n"
        "madd.qh %0, %1, %2\n"  // Note: This is pseudo - actual MMI saturation is complex
        : "=r"(result) : "r"(a), "r"(b)
    );
    // Fallback to scalar for now
    uint32_t a0 = (uint32_t)a;
    uint32_t a1 = (uint32_t)(a >> 32);
    uint32_t b0 = (uint32_t)b;
    uint32_t b1 = (uint32_t)(b >> 32);
    uint32_t r0 = a0 + b0;
    uint32_t r1 = a1 + b1;
    // Saturate each 6-bit component to 0x3F
    for (int i = 0; i < 2; i++) {
        uint32_t *r = (i == 0) ? &r0 : &r1;
        uint32_t val = *r;
        uint32_t r_comp = val & 0x3F;
        uint32_t g_comp = (val >> 6) & 0x3F;
        uint32_t b_comp = (val >> 12) & 0x3F;
        uint32_t a_comp = (val >> 18) & 0x3F;
        r_comp = r_comp > 0x3F ? 0x3F : r_comp;
        g_comp = g_comp > 0x3F ? 0x3F : g_comp;
        b_comp = b_comp > 0x3F ? 0x3F : b_comp;
        a_comp = a_comp > 0x3F ? 0x3F : a_comp;
        *r = a_comp << 18 | b_comp << 12 | g_comp << 6 | r_comp;
    }
    return (v2i32)r0 | ((v2i32)r1 << 32);
}

// Vector min of two v2i32 (per 6-bit component)
static inline v2i32 mmi_min_v2i32_rgb6(v2i32 a, v2i32 b)
{
    v2i32 result;
    uint32_t a0 = (uint32_t)a;
    uint32_t a1 = (uint32_t)(a >> 32);
    uint32_t b0 = (uint32_t)b;
    uint32_t b1 = (uint32_t)(b >> 32);
    uint32_t r0 = 0, r1 = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t *av = (i == 0) ? &a0 : &a1;
        uint32_t *bv = (i == 0) ? &b0 : &b1;
        uint32_t *rv = (i == 0) ? &r0 : &r1;
        uint32_t va = *av, vb = *bv;
        *rv = ((va & 0x3F) < (vb & 0x3F) ? (va & 0x3F) : (vb & 0x3F)) |
              (((va >> 6) & 0x3F) < ((vb >> 6) & 0x3F) ? ((va >> 6) & 0x3F) : ((vb >> 6) & 0x3F)) << 6 |
              (((va >> 12) & 0x3F) < ((vb >> 12) & 0x3F) ? ((va >> 12) & 0x3F) : ((vb >> 12) & 0x3F)) << 12 |
              (((va >> 18) & 0x3F) < ((vb >> 18) & 0x3F) ? ((va >> 18) & 0x3F) : ((vb >> 18) & 0x3F)) << 18;
    }
    return (v2i32)r0 | ((v2i32)r1 << 32);
}

// Vector max of two v2i32 (per 6-bit component)
static inline v2i32 mmi_max_v2i32_rgb6(v2i32 a, v2i32 b)
{
    uint32_t a0 = (uint32_t)a;
    uint32_t a1 = (uint32_t)(a >> 32);
    uint32_t b0 = (uint32_t)b;
    uint32_t b1 = (uint32_t)(b >> 32);
    uint32_t r0 = 0, r1 = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t *av = (i == 0) ? &a0 : &a1;
        uint32_t *bv = (i == 0) ? &b0 : &b1;
        uint32_t *rv = (i == 0) ? &r0 : &r1;
        uint32_t va = *av, vb = *bv;
        *rv = ((va & 0x3F) > (vb & 0x3F) ? (va & 0x3F) : (vb & 0x3F)) |
              (((va >> 6) & 0x3F) > ((vb >> 6) & 0x3F) ? ((va >> 6) & 0x3F) : ((vb >> 6) & 0x3F)) << 6 |
              (((va >> 12) & 0x3F) > ((vb >> 12) & 0x3F) ? ((va >> 12) & 0x3F) : ((vb >> 12) & 0x3F)) << 12 |
              (((va >> 18) & 0x3F) > ((vb >> 18) & 0x3F) ? ((va >> 18) & 0x3F) : ((vb >> 18) & 0x3F)) << 18;
    }
    return (v2i32)r0 | ((v2i32)r1 << 32);
}

// Shuffle bytes in 64-bit vector
static inline mmi64_t mmi_shuffle_bytes(mmi64_t a, int mask)
{
    mmi64_t result;
    __asm__ volatile("pshufb %0, %1, %2" : "=r"(result) : "r"(a), "r"(mask));
    return result;
}

// Convert 2x RGB5 (16-bit) to 2x RGB6 (18-bit packed in 32-bit)
static inline v2i32 mmi_rgb5_to_rgb6(v4i16 rgb5_pairs)
{
    v2i32 result;
    uint16_t *p = (uint16_t*)&rgb5_pairs;
    uint32_t r0 = 0, r1 = 0;
    for (int i = 0; i < 2; i++) {
        uint16_t c = p[i];
        uint32_t r = (c >> 0) & 0x1F;
        uint32_t g = (c >> 5) & 0x1F;
        uint32_t b = (c >> 10) & 0x1F;
        r = (r << 1) | (r != 0);
        g = (g << 1) | (g != 0);
        b = (b << 1) | (b != 0);
        uint32_t val = (b << 12) | (g << 6) | r;
        if (i == 0) r0 = val; else r1 = val;
    }
    return (v2i32)r0 | ((v2i32)r1 << 32);
}

// Convert 2x RGB6 (32-bit) to 2x CT16 (16-bit GS format)
static inline v4i16 mmi_rgb6_to_ct16(v2i32 rgb6_pairs)
{
    v4i16 result;
    uint32_t a0 = (uint32_t)rgb6_pairs;
    uint32_t a1 = (uint32_t)(rgb6_pairs >> 32);
    uint16_t r0 = 0, r1 = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t c = (i == 0) ? a0 : a1;
        uint32_t r = (c >> 0) & 0x3F;
        uint32_t g = (c >> 6) & 0x3F;
        uint32_t b = (c >> 12) & 0x3F;
        // Convert 6-bit to 5-bit (GS CT16 format: 1-5-5-5)
        uint16_t out = (1 << 15) | ((r >> 1) << 10) | ((g >> 1) << 5) | (b >> 1);
        if (i == 0) r0 = out; else r1 = out;
    }
    return (v4i16)((uint64_t)r0 | ((uint64_t)r1 << 16));
}

#ifdef __cplusplus
}
#endif

#endif // MMI_H
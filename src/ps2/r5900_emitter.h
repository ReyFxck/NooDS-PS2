#ifndef PS2_R5900_EMITTER_H
#define PS2_R5900_EMITTER_H

#include <cstdint>

namespace R5900
{
    enum Register
    {
        R_ZERO = 0, R_AT = 1,
        R_V0 = 2, R_V1 = 3,
        R_A0 = 4, R_A1 = 5, R_A2 = 6, R_A3 = 7,
        R_T0 = 8, R_T1 = 9, R_T2 = 10, R_T3 = 11,
        R_T4 = 12, R_T5 = 13, R_T6 = 14, R_T7 = 15,
        R_S0 = 16, R_S1 = 17, R_S2 = 18, R_S3 = 19,
        R_S4 = 20, R_S5 = 21, R_S6 = 22, R_S7 = 23,
        R_T8 = 24, R_T9 = 25,
        R_K0 = 26, R_K1 = 27,
        R_GP = 28, R_SP = 29, R_FP = 30, R_RA = 31
    };

    constexpr uint32_t rType(int rs, int rt, int rd, int shamt, int funct)
    {
        return (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
               (uint32_t(rd) << 11) | (uint32_t(shamt) << 6) | uint32_t(funct);
    }

    constexpr uint32_t iType(int op, int rs, int rt, uint16_t imm)
    {
        return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
               (uint32_t(rt) << 16) | imm;
    }

    constexpr uint32_t nop()                         { return 0; }
    constexpr uint32_t jr(int rs)                   { return rType(rs, 0, 0, 0, 0x08); }
    constexpr uint32_t jalr(int rd,int rs)          { return rType(rs, 0, rd, 0, 0x09); }
    constexpr uint32_t addu(int rd,int rs,int rt)    { return rType(rs, rt, rd, 0, 0x21); }
    constexpr uint32_t subu(int rd,int rs,int rt)    { return rType(rs, rt, rd, 0, 0x23); }
    constexpr uint32_t and_(int rd,int rs,int rt)    { return rType(rs, rt, rd, 0, 0x24); }
    constexpr uint32_t or_(int rd,int rs,int rt)     { return rType(rs, rt, rd, 0, 0x25); }
    constexpr uint32_t xor_(int rd,int rs,int rt)    { return rType(rs, rt, rd, 0, 0x26); }
    constexpr uint32_t nor_(int rd,int rs,int rt)    { return rType(rs, rt, rd, 0, 0x27); }
    constexpr uint32_t sll(int rd,int rt,int sh)     { return rType(0, rt, rd, sh, 0x00); }
    constexpr uint32_t srl(int rd,int rt,int sh)     { return rType(0, rt, rd, sh, 0x02); }
    constexpr uint32_t sra(int rd,int rt,int sh)     { return rType(0, rt, rd, sh, 0x03); }
    constexpr uint32_t sllv(int rd,int rt,int rs)    { return rType(rs, rt, rd, 0, 0x04); }
    constexpr uint32_t srlv(int rd,int rt,int rs)    { return rType(rs, rt, rd, 0, 0x06); }
    constexpr uint32_t srav(int rd,int rt,int rs)    { return rType(rs, rt, rd, 0, 0x07); }
    constexpr uint32_t movz(int rd,int rs,int rt)    { return rType(rs, rt, rd, 0, 0x0A); }
    constexpr uint32_t movn(int rd,int rs,int rt)    { return rType(rs, rt, rd, 0, 0x0B); }
    constexpr uint32_t sltu(int rd,int rs,int rt)     { return rType(rs, rt, rd, 0, 0x2B); }

    constexpr uint32_t addiu(int rt,int rs,int16_t i){ return iType(0x09, rs, rt, uint16_t(i)); }
    constexpr uint32_t andi(int rt,int rs,uint16_t i){ return iType(0x0C, rs, rt, i); }
    constexpr uint32_t ori(int rt,int rs,uint16_t i) { return iType(0x0D, rs, rt, i); }
    constexpr uint32_t xori(int rt,int rs,uint16_t i){ return iType(0x0E, rs, rt, i); }
    constexpr uint32_t sltiu(int rt,int rs,uint16_t i){ return iType(0x0B, rs, rt, i); }
    constexpr uint32_t lui(int rt,uint16_t i)        { return iType(0x0F, 0, rt, i); }
    constexpr uint32_t lw(int rt,int16_t o,int rs)   { return iType(0x23, rs, rt, uint16_t(o)); }
    constexpr uint32_t lhu(int rt,int16_t o,int rs)  { return iType(0x25, rs, rt, uint16_t(o)); }
    constexpr uint32_t sw(int rt,int16_t o,int rs)   { return iType(0x2B, rs, rt, uint16_t(o)); }
    constexpr uint32_t lbu(int rt,int16_t o,int rs)  { return iType(0x24, rs, rt, uint16_t(o)); }
    constexpr uint32_t sb(int rt,int16_t o,int rs)   { return iType(0x28, rs, rt, uint16_t(o)); }
    constexpr uint32_t sh(int rt,int16_t o,int rs)   { return iType(0x29, rs, rt, uint16_t(o)); }

    constexpr uint32_t beq(int rs,int rt,int16_t o)  { return iType(0x04, rs, rt, uint16_t(o)); }
    constexpr uint32_t bne(int rs,int rt,int16_t o)  { return iType(0x05, rs, rt, uint16_t(o)); }

    // Compile-time checks: bad encodings fail CI instead of crashing a PS2.
    static_assert(lui(R_V0, 0x1234) == 0x3C021234u, "R5900 LUI encoding");
    static_assert(ori(R_V0, R_V0, 0x5678) == 0x34425678u, "R5900 ORI encoding");
    static_assert(jr(R_RA) == 0x03E00008u, "R5900 JR encoding");
    static_assert(jalr(R_RA, R_T9) == 0x0320F809u, "R5900 JALR encoding");
    static_assert(lw(R_T0, 4, R_A0) == 0x8C880004u, "R5900 LW encoding");
    static_assert(lhu(R_T0, 4, R_A0) == 0x94880004u, "R5900 LHU encoding");
    static_assert(sw(R_T0, 4, R_A0) == 0xAC880004u, "R5900 SW encoding");
    static_assert(sh(R_T0, 4, R_A0) == 0xA4880004u, "R5900 SH encoding");
    static_assert(nor_(R_T2, R_T1, R_ZERO) == 0x01205027u, "R5900 NOR encoding");
    static_assert(sra(R_T2, R_T1, 4) == 0x00095103u, "R5900 SRA encoding");
    static_assert(sllv(R_T2, R_T1, R_T3) == 0x01695004u, "R5900 SLLV encoding");
    static_assert(movz(R_T1, R_ZERO, R_T4) == 0x000C480Au, "R5900 MOVZ encoding");
    static_assert(movn(R_T7, R_T6, R_T5) == 0x01CD780Bu, "R5900 MOVN encoding");
    static_assert(sltu(R_T2, R_T0, R_T1) == 0x0109502Bu, "R5900 SLTU encoding");
}

#endif

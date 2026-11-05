#ifndef PS2_THUMB_ALU_H
#define PS2_THUMB_ALU_H

#include <cstdint>

namespace Ps2Thumb
{
    enum Kind : uint8_t
    {
        UNSUPPORTED = 0,
        ARM_ALU,
        ARM_LOAD,
        ADDRESS_ADD,
        LITERAL_LOAD,
        CONDITIONAL_BRANCH,
        BRANCH,
        BRANCH_EXCHANGE
    };

    struct Decoded
    {
        Kind kind = UNSUPPORTED;
        uint32_t armOpcode = 0;
        uint32_t target = 0;
        uint16_t readMask = 0;
        uint16_t writeMask = 0;
        uint8_t rd = 0xFF;
        uint8_t condition = 0xE;
        uint8_t baseReg = 0xFF;
        int16_t addend = 0;
        bool readsFlags = false;
        bool writesFlags = false;
        bool terminal = false;
        uint8_t minCycles = 1;
        uint8_t maxCycles = 1;
    };

    inline uint32_t armDataReg(uint8_t operation, bool setFlags,
        uint8_t rn, uint8_t rd, uint8_t rm)
    {
        return 0xE0000000u |
            (static_cast<uint32_t>(operation) << 21) |
            (setFlags ? 0x00100000u : 0u) |
            (static_cast<uint32_t>(rn) << 16) |
            (static_cast<uint32_t>(rd) << 12) |
            rm;
    }

    inline uint32_t armDataImm(uint8_t operation, bool setFlags,
        uint8_t rn, uint8_t rd, uint8_t imm8)
    {
        return 0xE2000000u |
            (static_cast<uint32_t>(operation) << 21) |
            (setFlags ? 0x00100000u : 0u) |
            (static_cast<uint32_t>(rn) << 16) |
            (static_cast<uint32_t>(rd) << 12) |
            imm8;
    }

    inline bool decode(uint16_t opcode, uint32_t pc, Decoded *out)
    {
        if (!out)
            return false;
        *out = Decoded();

        // Format 1: LSL/LSR/ASR immediate. ARM's immediate shift encoding
        // has the same special zero semantics for LSR/ASR (#32).
        if ((opcode & 0xE000u) == 0x0000u && (opcode & 0x1800u) != 0x1800u)
        {
            const uint8_t shiftType = static_cast<uint8_t>((opcode >> 11) & 3u);
            if (shiftType > 2)
                return false;
            const uint8_t imm5 = static_cast<uint8_t>((opcode >> 6) & 0x1Fu);
            const uint8_t rs = static_cast<uint8_t>((opcode >> 3) & 7u);
            const uint8_t rd = static_cast<uint8_t>(opcode & 7u);
            out->kind = ARM_ALU;
            out->armOpcode = 0xE1B00000u |
                (static_cast<uint32_t>(rd) << 12) |
                (static_cast<uint32_t>(imm5) << 7) |
                (static_cast<uint32_t>(shiftType) << 5) | rs;
            out->readMask = static_cast<uint16_t>(1u << rs);
            out->writeMask = static_cast<uint16_t>(1u << rd);
            out->rd = rd;
            out->writesFlags = true;
            return true;
        }

        // Format 2: ADD/SUB register or 3-bit immediate.
        if ((opcode & 0xF800u) == 0x1800u)
        {
            const bool immediate = (opcode & 0x0400u) != 0;
            const bool subtract = (opcode & 0x0200u) != 0;
            const uint8_t operand = static_cast<uint8_t>((opcode >> 6) & 7u);
            const uint8_t rs = static_cast<uint8_t>((opcode >> 3) & 7u);
            const uint8_t rd = static_cast<uint8_t>(opcode & 7u);
            out->kind = ARM_ALU;
            out->armOpcode = immediate
                ? armDataImm(subtract ? 2u : 4u, true, rs, rd, operand)
                : armDataReg(subtract ? 2u : 4u, true, rs, rd, operand);
            out->readMask = static_cast<uint16_t>(1u << rs);
            if (!immediate)
                out->readMask |= static_cast<uint16_t>(1u << operand);
            out->writeMask = static_cast<uint16_t>(1u << rd);
            out->rd = rd;
            out->writesFlags = true;
            return true;
        }

        // Format 3: MOV/CMP/ADD/SUB immediate.
        if ((opcode & 0xE000u) == 0x2000u)
        {
            const uint8_t op = static_cast<uint8_t>((opcode >> 11) & 3u);
            const uint8_t rd = static_cast<uint8_t>((opcode >> 8) & 7u);
            const uint8_t imm = static_cast<uint8_t>(opcode);
            out->kind = ARM_ALU;
            switch (op)
            {
                case 0: // MOVS
                    out->armOpcode = armDataImm(13, true, 0, rd, imm);
                    out->writeMask = static_cast<uint16_t>(1u << rd);
                    break;
                case 1: // CMP
                    out->armOpcode = armDataImm(10, true, rd, 0, imm);
                    out->readMask = static_cast<uint16_t>(1u << rd);
                    break;
                case 2: // ADDS Rd,Rd,#imm
                    out->armOpcode = armDataImm(4, true, rd, rd, imm);
                    out->readMask = static_cast<uint16_t>(1u << rd);
                    out->writeMask = static_cast<uint16_t>(1u << rd);
                    break;
                default: // SUBS Rd,Rd,#imm
                    out->armOpcode = armDataImm(2, true, rd, rd, imm);
                    out->readMask = static_cast<uint16_t>(1u << rd);
                    out->writeMask = static_cast<uint16_t>(1u << rd);
                    break;
            }
            out->rd = rd;
            out->writesFlags = true;
            return true;
        }

        // Format 4: low-register ALU. Register-shifts/ROR and MUL are kept in
        // the interpreter initially because their Thumb timing differs from
        // the ARM emitter's timing model.
        if ((opcode & 0xFC00u) == 0x4000u)
        {
            const uint8_t op = static_cast<uint8_t>((opcode >> 6) & 0xFu);
            const uint8_t rs = static_cast<uint8_t>((opcode >> 3) & 7u);
            const uint8_t rd = static_cast<uint8_t>(opcode & 7u);
            if (op == 2 || op == 3 || op == 4 || op == 7 || op == 13)
                return false;

            out->kind = ARM_ALU;
            out->writesFlags = true;
            out->rd = rd;
            switch (op)
            {
                case 0: out->armOpcode = armDataReg(0, true, rd, rd, rs); break; // AND
                case 1: out->armOpcode = armDataReg(1, true, rd, rd, rs); break; // EOR
                case 5: out->armOpcode = armDataReg(5, true, rd, rd, rs);
                        out->readsFlags = true; break; // ADC
                case 6: out->armOpcode = armDataReg(6, true, rd, rd, rs);
                        out->readsFlags = true; break; // SBC
                case 8: out->armOpcode = armDataReg(8, true, rd, 0, rs); break; // TST
                case 9: out->armOpcode = armDataImm(3, true, rs, rd, 0); break; // NEG
                case 10: out->armOpcode = armDataReg(10, true, rd, 0, rs); break; // CMP
                case 11: out->armOpcode = armDataReg(11, true, rd, 0, rs); break; // CMN
                case 12: out->armOpcode = armDataReg(12, true, rd, rd, rs); break; // ORR
                case 14: out->armOpcode = armDataReg(14, true, rd, rd, rs); break; // BIC
                case 15: out->armOpcode = armDataReg(15, true, 0, rd, rs); break; // MVN
                default: return false;
            }
            out->readMask = static_cast<uint16_t>(1u << rs);
            if (op != 9 && op != 15)
                out->readMask |= static_cast<uint16_t>(1u << rd);
            if (op != 8 && op != 10 && op != 11)
                out->writeMask = static_cast<uint16_t>(1u << rd);
            return true;
        }

        // Format 5: high-register ADD/CMP/MOV and BX. Keep PC as a
        // general ALU operand/destination outside this conservative subset;
        // BX is the only control-flow form admitted here.
        if ((opcode & 0xFC00u) == 0x4400u)
        {
            const uint8_t op = static_cast<uint8_t>((opcode >> 8) & 3u);
            const uint8_t rs = static_cast<uint8_t>((opcode >> 3) & 0xFu);
            const uint8_t rd = static_cast<uint8_t>(
                ((opcode >> 4) & 8u) | (opcode & 7u));

            if (op == 3u)
            {
                // Bit 7 selects BLX-register on ARMv5. ARM7 (cpu=1) has no
                // BLX, so reject it here; plain BX may switch ARM/Thumb.
                if (opcode & 0x0080u || rs == 15u)
                    return false;
                out->kind = BRANCH_EXCHANGE;
                out->armOpcode = 0xE12FFF10u | rs;
                out->readMask = static_cast<uint16_t>(1u << rs);
                out->readsFlags = false;
                out->writesFlags = true; // CPSR.T follows target bit 0.
                out->terminal = true;
                out->minCycles = out->maxCycles = 3;
                return true;
            }

            if (rd == 15u || rs == 15u)
                return false;

            out->kind = ARM_ALU;
            out->rd = rd;
            out->readMask = static_cast<uint16_t>(1u << rs);
            if (op == 0u) // ADD Rd,Rs (no flags)
            {
                out->armOpcode = armDataReg(4, false, rd, rd, rs);
                out->readMask |= static_cast<uint16_t>(1u << rd);
                out->writeMask = static_cast<uint16_t>(1u << rd);
            }
            else if (op == 1u) // CMP Rd,Rs
            {
                out->armOpcode = armDataReg(10, true, rd, 0, rs);
                out->readMask |= static_cast<uint16_t>(1u << rd);
                out->writesFlags = true;
            }
            else // MOV Rd,Rs (no flags)
            {
                out->armOpcode = armDataReg(13, false, 0, rd, rs);
                out->writeMask = static_cast<uint16_t>(1u << rd);
            }
            return true;
        }

        // Formats 7/8: register-offset loads. Store forms are kept
        // interpreter-only; all five load forms share ARM7's 3-cycle timing.
        if ((opcode & 0xF000u) == 0x5000u)
        {
            const uint8_t op = static_cast<uint8_t>((opcode >> 9) & 7u);
            if (op < 3u)
                return false; // STR/STRH/STRB

            const uint8_t ro = static_cast<uint8_t>((opcode >> 6) & 7u);
            const uint8_t rb = static_cast<uint8_t>((opcode >> 3) & 7u);
            const uint8_t rd = static_cast<uint8_t>(opcode & 7u);

            uint32_t arm = 0;
            switch (op)
            {
                case 3: arm = 0xE19000D0u; break; // LDRSB
                case 4: arm = 0xE7900000u; break; // LDR
                case 5: arm = 0xE19000B0u; break; // LDRH
                case 6: arm = 0xE7D00000u; break; // LDRB
                case 7: arm = 0xE19000F0u; break; // LDRSH
                default: return false;
            }

            out->kind = ARM_LOAD;
            out->rd = rd;
            out->readMask = static_cast<uint16_t>(
                (1u << rb) | (1u << ro));
            out->writeMask = static_cast<uint16_t>(1u << rd);
            out->armOpcode = arm |
                (static_cast<uint32_t>(rb) << 16) |
                (static_cast<uint32_t>(rd) << 12) | ro;
            out->minCycles = out->maxCycles = 3;
            return true;
        }

        // Format 9: LDR/LDRB with immediate offset. Stores stay outside
        // the conservative backend. ARM7 timing matches the ARM load emitter.
        if ((opcode & 0xE000u) == 0x6000u)
        {
            const bool byte = (opcode & 0x1000u) != 0;
            const bool load = (opcode & 0x0800u) != 0;
            if (!load)
                return false;
            const uint8_t imm5 = static_cast<uint8_t>((opcode >> 6) & 0x1Fu);
            const uint8_t rb = static_cast<uint8_t>((opcode >> 3) & 7u);
            const uint8_t rd = static_cast<uint8_t>(opcode & 7u);
            const uint32_t offset = byte ? imm5 :
                (static_cast<uint32_t>(imm5) << 2);

            out->kind = ARM_LOAD;
            out->rd = rd;
            out->readMask = static_cast<uint16_t>(1u << rb);
            out->writeMask = static_cast<uint16_t>(1u << rd);
            out->armOpcode = (byte ? 0xE5D00000u : 0xE5900000u) |
                (static_cast<uint32_t>(rb) << 16) |
                (static_cast<uint32_t>(rd) << 12) | offset;
            out->minCycles = out->maxCycles = 3;
            return true;
        }

        // Format 10: LDRH Rd,[Rb,#imm5*2]. STRH remains interpreter-only.
        if ((opcode & 0xF000u) == 0x8000u)
        {
            const bool load = (opcode & 0x0800u) != 0;
            if (!load)
                return false;
            const uint8_t imm5 = static_cast<uint8_t>((opcode >> 6) & 0x1Fu);
            const uint8_t rb = static_cast<uint8_t>((opcode >> 3) & 7u);
            const uint8_t rd = static_cast<uint8_t>(opcode & 7u);
            const uint32_t offset = static_cast<uint32_t>(imm5) << 1;
            out->kind = ARM_LOAD;
            out->rd = rd;
            out->readMask = static_cast<uint16_t>(1u << rb);
            out->writeMask = static_cast<uint16_t>(1u << rd);
            out->armOpcode = 0xE1D000B0u |
                (static_cast<uint32_t>(rb) << 16) |
                (static_cast<uint32_t>(rd) << 12) |
                ((offset & 0xF0u) << 4) | (offset & 0xFu);
            out->minCycles = out->maxCycles = 3;
            return true;
        }

        // Format 11: LDR Rd,[SP,#imm8*4]. STR remains interpreter-only.
        if ((opcode & 0xF000u) == 0x9000u)
        {
            const bool load = (opcode & 0x0800u) != 0;
            if (!load)
                return false;
            const uint8_t rd = static_cast<uint8_t>((opcode >> 8) & 7u);
            const uint32_t offset =
                static_cast<uint32_t>(opcode & 0xFFu) << 2;
            out->kind = ARM_LOAD;
            out->rd = rd;
            out->readMask = static_cast<uint16_t>(1u << 13);
            out->writeMask = static_cast<uint16_t>(1u << rd);
            out->armOpcode = 0xE59D0000u |
                (static_cast<uint32_t>(rd) << 12) | offset;
            out->minCycles = out->maxCycles = 3;
            return true;
        }

        // Format 12: ADD Rd,PC/SP,#imm8*4. These are pure address
        // calculations and do not touch flags. PC uses Thumb's aligned
        // architectural PC+4 value; SP stays a normal live register input.
        if ((opcode & 0xF000u) == 0xA000u)
        {
            const bool useSp = (opcode & 0x0800u) != 0;
            const uint8_t rd = static_cast<uint8_t>((opcode >> 8) & 7u);
            const uint32_t offset =
                static_cast<uint32_t>(opcode & 0xFFu) << 2;
            out->kind = ADDRESS_ADD;
            out->rd = rd;
            out->writeMask = static_cast<uint16_t>(1u << rd);
            if (useSp)
            {
                out->baseReg = 13;
                out->addend = static_cast<int16_t>(offset);
                out->readMask = static_cast<uint16_t>(1u << 13);
            }
            else
            {
                out->target = ((pc + 4u) & ~3u) + offset;
            }
            return true;
        }

        // Format 13: ADD/SUB SP,#imm7*4. No flags are modified.
        if ((opcode & 0xFF00u) == 0xB000u)
        {
            const bool subtract = (opcode & 0x0080u) != 0;
            const int16_t offset = static_cast<int16_t>(
                static_cast<uint16_t>(opcode & 0x7Fu) << 2);
            out->kind = ADDRESS_ADD;
            out->rd = 13;
            out->baseReg = 13;
            out->addend = subtract
                ? static_cast<int16_t>(-offset) : offset;
            out->readMask = static_cast<uint16_t>(1u << 13);
            out->writeMask = static_cast<uint16_t>(1u << 13);
            return true;
        }

        // Format 14 POP without PC. PUSH and POP {...,pc} stay outside the
        // first read-only live backend. Reuse the already differential-gated
        // ARM7 LDMIA sp! emitter.
        if ((opcode & 0xFE00u) == 0xBC00u)
        {
            const uint16_t list = static_cast<uint16_t>(opcode & 0x00FFu);
            if (!list || (opcode & 0x0100u))
                return false;

            uint8_t count = 0;
            for (uint16_t bits = list; bits; bits >>= 1)
                count += static_cast<uint8_t>(bits & 1u);

            out->kind = ARM_LOAD;
            out->armOpcode = 0xE8BD0000u | list; // LDMIA sp!,{rlist}
            out->readMask = static_cast<uint16_t>(1u << 13);
            out->writeMask = static_cast<uint16_t>(
                list | (1u << 13));
            out->minCycles = out->maxCycles =
                static_cast<uint8_t>(count + 2u);
            return true;
        }

        // Format 15 LDMIA Rb!,{rlist}. Store-multiple and base-in-list are
        // intentionally left to the interpreter; both complicate the first
        // read-only Thumb live path.
        if ((opcode & 0xF000u) == 0xC000u)
        {
            const bool load = (opcode & 0x0800u) != 0;
            const uint8_t rb = static_cast<uint8_t>((opcode >> 8) & 7u);
            const uint16_t list = static_cast<uint16_t>(opcode & 0x00FFu);
            if (!load || !list || (list & (1u << rb)))
                return false;

            uint8_t count = 0;
            for (uint16_t bits = list; bits; bits >>= 1)
                count += static_cast<uint8_t>(bits & 1u);

            out->kind = ARM_LOAD;
            out->armOpcode = 0xE8B00000u |
                (static_cast<uint32_t>(rb) << 16) | list;
            out->readMask = static_cast<uint16_t>(1u << rb);
            out->writeMask = static_cast<uint16_t>(
                list | (1u << rb));
            out->minCycles = out->maxCycles =
                static_cast<uint8_t>(count + 2u);
            return true;
        }

        // LDR Rd,[PC,#imm*4]. Thumb PC is current+4, word aligned.
        if ((opcode & 0xF800u) == 0x4800u)
        {
            const uint8_t rd = static_cast<uint8_t>((opcode >> 8) & 7u);
            const uint32_t base = (pc + 4u) & ~3u;
            out->kind = LITERAL_LOAD;
            out->rd = rd;
            out->target = base +
                (static_cast<uint32_t>(opcode & 0xFFu) << 2);
            out->writeMask = static_cast<uint16_t>(1u << rd);
            out->minCycles = out->maxCycles = 3;
            return true;
        }

        // Conditional B. 0xE is undefined/reserved and 0xF is SWI.
        if ((opcode & 0xF000u) == 0xD000u)
        {
            const uint8_t condition = static_cast<uint8_t>((opcode >> 8) & 0xFu);
            if (condition >= 0xEu)
                return false;
            const int32_t offset =
                static_cast<int32_t>(static_cast<int8_t>(opcode & 0xFFu)) << 1;
            out->kind = CONDITIONAL_BRANCH;
            out->condition = condition;
            out->readsFlags = true;
            out->terminal = true;
            out->target = pc + 4u + static_cast<uint32_t>(offset);
            out->minCycles = 1;
            out->maxCycles = 3;
            return true;
        }

        // Unconditional B.
        if ((opcode & 0xF800u) == 0xE000u)
        {
            int32_t offset = static_cast<int32_t>(opcode & 0x07FFu);
            if (offset & 0x400)
                offset |= ~0x7FF;
            offset <<= 1;
            out->kind = BRANCH;
            out->terminal = true;
            out->target = pc + 4u + static_cast<uint32_t>(offset);
            out->minCycles = out->maxCycles = 3;
            return true;
        }

        return false;
    }
}

#endif

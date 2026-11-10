#include <cassert>
#include <cstdint>
#include <functional>
#include <initializer_list>

// Exercise the actual interpreter method without constructing the whole core.
#define private public
#include "../src/interpreter.h"
#undef private
#include "../src/interpreter_alu.h"
#include "../src/ps2/arm_alu.h"
#include "../src/ps2/r5900_alu_flags.h"

Interpreter::Interpreter(Core *core, bool cpu): core(core), cpu(cpu)
{
    for (int i = 0; i < 16; ++i)
        registers[i] = &registersUsr[i];
}
void Interpreter::setCpsr(uint32_t value, bool) { cpsr = value; }
void Interpreter::flushPipeline() {}

static uint64_t word(uint32_t x)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(x)));
}

// Execute the actual emitted subset with R5900 64-bit GPR and word-shift
// behavior. Expected results below use independent wide signed arithmetic.
static uint32_t emittedOverflow(uint32_t left, uint32_t right,
    uint32_t result, bool subtraction, bool reverse)
{
    uint32_t code[8] = {};
    const size_t count = emitArmCarryOverflow(code, subtraction, reverse);
    uint64_t r[32] = {};
    r[R5900::R_T0] = word(left);
    r[R5900::R_T1] = word(right);
    r[R5900::R_T2] = word(result);
    for (size_t i = 0; i < count; ++i)
    {
        uint32_t insn = code[i];
        assert((insn >> 26) == 0);
        unsigned rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
        unsigned rd = (insn >> 11) & 31, shift = (insn >> 6) & 31;
        switch (insn & 63)
        {
            case 0x26: r[rd] = r[rs] ^ r[rt]; break;
            case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
            case 0x24: r[rd] = r[rs] & r[rt]; break;
            case 0x02: r[rd] = word(static_cast<uint32_t>(r[rt]) >> shift); break;
            default: assert(false);
        }
    }
    return static_cast<uint32_t>(r[R5900::R_V0]);
}

static void check(uint32_t a, uint32_t b, bool carry)
{
    const uint32_t opcodes[] = {0xE0B02001u, 0xE0D02001u, 0xE0F02001u};
    for (int op = 0; op < 3; ++op)
    {
        int64_t left = static_cast<int32_t>(op == 2 ? b : a);
        int64_t right = static_cast<int32_t>(op == 2 ? a : b);
        int64_t value = op == 0 ? left + right + carry : left - right - !carry;
        uint32_t result = static_cast<uint32_t>(value);
        bool overflow = value > INT32_MAX || value < INT32_MIN;
        assert(emittedOverflow(a, b, result, op != 0, op == 2) == overflow);

        if (op == 2)
        {
            for (uint32_t oldV : {0u, 0x10000000u})
            {
                Interpreter cpu(nullptr, false);
                cpu.registersUsr[0] = a;
                cpu.registersUsr[1] = b;
                cpu.cpsr = 0x13u | oldV | (carry ? 0x20000000u : 0u);
                uint32_t regs[16] = {};
                regs[0] = a; regs[1] = b;
                uint32_t expected = cpu.cpsr;
                assert(Ps2ArmAlu::execute(opcodes[op], regs, &expected));
                assert(cpu.rscs(opcodes[op], b) == 1);
                assert(cpu.registersUsr[2] == regs[2]);
                assert(cpu.cpsr == expected);
            }
        }
    }
}

int main()
{
    const uint32_t edges[] = {0, 1, 2, 0x7FFFFFFEu, 0x7FFFFFFFu,
        0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu};
    for (uint32_t a : edges) for (uint32_t b : edges)
        for (int carry = 0; carry < 2; ++carry) check(a, b, carry);
    uint32_t seed = 0xBEEFCACEu;
    for (int i = 0; i < 20000; ++i)
    {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        uint32_t a = seed;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        check(a, seed, (i & 1) != 0);
    }
}

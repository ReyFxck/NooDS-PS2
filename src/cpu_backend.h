#ifndef CPU_BACKEND_H
#define CPU_BACKEND_H

#include <cstdint>

#include "interpreter.h"

// Architecture-neutral access layer for alternative CPU execution engines.
// It intentionally exposes only the active ARM register bank and calls back
// into Interpreter for CPSR/pipeline transitions, so backends do not duplicate
// ARM mode banking or interrupt semantics.
class CpuBackendAccess
{
    public:
        static bool isArm9(const Interpreter &cpu)
        {
            return cpu.cpu == 0;
        }

        static bool isThumb(const Interpreter &cpu)
        {
            return (cpu.cpsr & BIT(5)) != 0;
        }

        static uint32_t readCpsr(const Interpreter &cpu)
        {
            return cpu.cpsr;
        }

        static uint32_t readReg(const Interpreter &cpu, int index)
        {
            return *cpu.registers[index & 15];
        }

        static void writeReg(Interpreter &cpu, int index, uint32_t value)
        {
            *cpu.registers[index & 15] = value;
        }

        // Address of pipeline[0], i.e. the next instruction that runOpcode()
        // would execute at a scheduler boundary.
        static uint32_t nextInstructionAddress(const Interpreter &cpu)
        {
            uint32_t pc = *cpu.registers[15];
            return pc - ((cpu.cpsr & BIT(5)) ? 2u : 4u);
        }

        static uint32_t pipelineOpcode(const Interpreter &cpu, int slot)
        {
            return cpu.pipeline[slot & 1];
        }

        // Restore a straight-line differential-test snapshot exactly. This is
        // intentionally raw: callers must only use it when the tested block
        // cannot change CPU mode, banked registers, memory or exceptions.
        static void restoreStraightLineState(Interpreter &cpu,
            const uint32_t regs[16], uint32_t cpsr,
            uint32_t pipeline0, uint32_t pipeline1)
        {
            cpu.cpsr = cpsr;
            for (int i = 0; i < 16; i++)
                *cpu.registers[i] = regs[i];
            cpu.pipeline[0] = pipeline0;
            cpu.pipeline[1] = pipeline1;
        }

        static void writeCpsr(Interpreter &cpu, uint32_t value, bool save = false)
        {
            cpu.setCpsr(value, save);
        }

        static void branchTo(Interpreter &cpu, uint32_t address, bool thumb)
        {
            uint32_t cpsr = cpu.cpsr;
            if (thumb) cpsr |= BIT(5);
            else       cpsr &= ~BIT(5);

            cpu.setCpsr(cpsr);
            *cpu.registers[15] = address;
            cpu.flushPipeline();
        }

        static void refillPipeline(Interpreter &cpu)
        {
            cpu.flushPipeline();
        }
};

#endif

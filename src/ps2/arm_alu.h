#ifndef PS2_ARM_ALU_H
#define PS2_ARM_ALU_H

#include <cstdint>

// Portable decoder/shadow semantics for the deliberately small ARM subset
// admitted by the PS2 dynarec. Keeping this independent of emitted R5900 code
// lets host CI exercise all edge cases before the runtime differential gate.
namespace Ps2ArmAlu
{
    enum InstructionKind : uint8_t
    {
        KIND_DATA = 0,
        KIND_MULTIPLY,
        KIND_MEMORY,
        KIND_MULTI_MEMORY,
        KIND_BRANCH,
        KIND_BRANCH_EXCHANGE,
        KIND_SYSTEM,
        KIND_UNKNOWN
    };

    enum RejectReason : uint8_t
    {
        REJECT_NONE = 0,
        REJECT_UNSUPPORTED_OPCODE,
        REJECT_CONDITION,
        REJECT_FLAGS,
        REJECT_SHIFT,
        REJECT_MEMORY,
        REJECT_BRANCH,
        REJECT_PC,
        REJECT_SYSTEM,
        REJECT_OTHER,
        REJECT_REASON_COUNT
    };

    struct InstructionInfo
    {
        InstructionKind kind = KIND_UNKNOWN;
        uint8_t condition = 0;
        uint8_t operation = 0;
        uint8_t rn = 0xFF;
        uint8_t rd = 0xFF;
        uint8_t rm = 0xFF;
        uint8_t rs = 0xFF;
        uint8_t shiftType = 0;
        uint8_t shiftAmount = 0;
        uint32_t immediateValue = 0;
        uint32_t branchTarget = 0xFFFFFFFFu;
        uint16_t readMask = 0;
        uint16_t writeMask = 0;
        bool immediate = false;
        bool registerShift = false;
        bool readsFlags = false;
        bool writesFlags = false;
        bool backEdge = false;
        bool terminatesBlock = false;
    };

    struct Decoded
    {
        uint8_t condition = 0xE;
        bool immediate = false;
        bool setFlags = false;
        bool writesResult = true;
        uint8_t operation = 0;
        uint8_t rn = 0;
        uint8_t rd = 0;
        uint8_t rm = 0;
        uint8_t rs = 0;
        uint8_t shiftType = 0;
        uint8_t shiftAmount = 0;
        bool registerShift = false;
        uint8_t immediateRotate = 0;
        uint32_t immediateValue = 0;
    };

    struct DecodedMultiply
    {
        uint8_t condition = 0xE;
        bool accumulate = false;
        bool setFlags = false;
        uint8_t rd = 0;
        uint8_t rn = 0;
        uint8_t rs = 0;
        uint8_t rm = 0;
    };

    struct DecodedBranch
    {
        uint8_t condition = 0xE;
        int32_t offset = 0;
    };

    struct DecodedBranchExchange
    {
        uint8_t condition = 0xE;
        uint8_t rm = 0;
    };


    struct DecodedHalfwordTransfer
    {
        uint8_t condition = 0xE;
        bool load = false;
        bool immediate = false;
        bool preIndex = true;
        bool addOffset = true;
        bool writeBack = false;
        uint8_t rn = 0;
        uint8_t rd = 0;
        uint8_t rm = 0;
        uint8_t immediateOffset = 0;
    };

    struct DecodedSingleTransfer
    {
        uint8_t condition = 0xE;
        bool load = false;
        bool registerOffset = false;
        bool preIndex = true;
        bool addOffset = true;
        bool writeBack = false;
        uint8_t rn = 0;
        uint8_t rd = 0;
        uint8_t rm = 0;
        uint8_t shiftType = 0;
        uint8_t shiftAmount = 0;
        uint16_t immediateOffset = 0;
    };

    struct DecodedByteTransfer
    {
        uint8_t condition = 0xE;
        bool load = false;
        bool registerOffset = false;
        bool preIndex = true;
        bool addOffset = true;
        bool writeBack = false;
        uint8_t rn = 0;
        uint8_t rd = 0;
        uint8_t rm = 0;
        uint8_t shiftType = 0;
        uint8_t shiftAmount = 0;
        uint16_t immediateOffset = 0;
    };

    struct DecodedLiteralLoad
    {
        uint8_t condition = 0xE;
        bool addOffset = true;
        uint8_t rd = 0;
        uint16_t immediateOffset = 0;
    };

    struct DecodedSingleBlockTransfer
    {
        uint8_t condition = 0xE;
        bool load = false;
        bool writeBack = false;
        uint8_t rn = 0;
        uint8_t reg = 0;
    };



    typedef bool (*ReadHalfword)(void *context, uint32_t address,
        uint16_t *value);
    typedef bool (*WriteHalfword)(void *context, uint32_t address,
        uint16_t value);

    typedef bool (*ReadByte)(void *context, uint32_t address,
        uint8_t *value);
    typedef bool (*WriteByte)(void *context, uint32_t address,
        uint8_t value);

    typedef bool (*ReadWord)(void *context, uint32_t address,
        uint32_t *value);
    typedef bool (*WriteWord)(void *context, uint32_t address,
        uint32_t value);

    bool applyImmediateShift(uint32_t value, uint8_t type,
        uint8_t encodedAmount, uint32_t *result);
    bool applyRegisterShift(uint32_t value, uint8_t type,
        uint32_t registerAmount, uint32_t *result);
    bool conditionPassed(uint8_t condition, uint32_t cpsr);
    bool decode(uint32_t opcode, Decoded *decoded);
    bool decodeMultiply(uint32_t opcode, DecodedMultiply *decoded);
    bool decodeBranch(uint32_t opcode, DecodedBranch *decoded);
    bool decodeBranchExchange(uint32_t opcode,
        DecodedBranchExchange *decoded);
    bool decodeHalfwordTransfer(uint32_t opcode,
        DecodedHalfwordTransfer *decoded);
    bool decodeSingleTransfer(uint32_t opcode,
        DecodedSingleTransfer *decoded);
    bool decodeByteTransfer(uint32_t opcode,
        DecodedByteTransfer *decoded);
    bool decodeLiteralLoad(uint32_t opcode, DecodedLiteralLoad *decoded);
    bool decodeSingleBlockTransfer(uint32_t opcode,
        DecodedSingleBlockTransfer *decoded);
    bool executeMultiply(uint32_t opcode, uint32_t registers[16],
        uint32_t *cpsr = nullptr);
    bool executeHalfwordTransfer(uint32_t opcode, uint32_t registers[16],
        uint32_t *cpsr, void *context, ReadHalfword read,
        WriteHalfword write = nullptr);
    bool executeSingleTransfer(uint32_t opcode, uint32_t registers[16],
        uint32_t *cpsr, void *context, ReadWord read,
        WriteWord write = nullptr);
    bool executeByteTransfer(uint32_t opcode, uint32_t registers[16],
        uint32_t *cpsr, void *context, ReadByte read,
        WriteByte write = nullptr);
    bool executeLiteralLoad(uint32_t opcode, uint32_t pc,
        uint32_t registers[16], uint32_t *cpsr, void *context,
        ReadWord read);
    bool executeSingleBlockTransfer(uint32_t opcode, uint32_t registers[16],
        uint32_t *cpsr, void *context, ReadWord read,
        WriteWord write = nullptr);
    bool executeBranch(uint32_t opcode, uint32_t pc, uint32_t cpsr,
        uint32_t *nextPc);
    bool executeBranchExchange(uint32_t opcode, uint32_t registers[16],
        uint32_t *cpsr, uint32_t *nextPc);
    bool execute(uint32_t opcode, uint32_t registers[16],
        uint32_t *cpsr = nullptr);
    void inspect(uint32_t opcode, uint32_t pc, InstructionInfo *info);
    RejectReason rejectionReason(uint32_t opcode);
    const char *rejectionReasonName(RejectReason reason);
    void format(uint32_t opcode, uint32_t pc, char *buffer, uint32_t size);
}

#endif

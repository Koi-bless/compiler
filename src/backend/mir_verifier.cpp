#include "toyc/backend/mir_verifier.hpp"

#include <algorithm>
#include <set>
#include <string>

#include "toyc/backend/riscv32.hpp"
#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

[[noreturn]] void invalid(const MachineFunction& function, MIRStage stage, const std::string& message) {
    const char* name = stage == MIRStage::PreRegisterAllocation ? "pre-RA" :
                       stage == MIRStage::PostRegisterAllocation ? "post-RA" : "after-frame";
    throw CompileError(function.location, "MIR verification", std::string(name) + ": " + message);
}

bool terminator(MOpcode opcode) {
    return opcode == MOpcode::BRCOND || opcode == MOpcode::BEQ ||
           opcode == MOpcode::BNE || opcode == MOpcode::BLT ||
           opcode == MOpcode::BGE || opcode == MOpcode::JUMP || opcode == MOpcode::RET;
}

bool isRegister(const MOperand& operand) {
    return std::holds_alternative<VirtualReg>(operand) ||
           std::holds_alternative<PhysReg>(operand);
}

void require(const MachineFunction& function, MIRStage stage, bool condition,
             const std::string& message) {
    if (!condition) invalid(function, stage, message);
}

void checkInstructionShape(const MachineFunction& function, MIRStage stage,
                           const MInstruction& instruction) {
    const auto oneRegisterDef = [&] {
        return instruction.defs.size() == 1 && isRegister(instruction.defs[0]);
    };
    const auto registerBinary = [&] {
        return oneRegisterDef() && instruction.uses.size() == 2 &&
               isRegister(instruction.uses[0]) && isRegister(instruction.uses[1]);
    };
    switch (instruction.opcode) {
    case MOpcode::LI:
        require(function, stage, oneRegisterDef() && instruction.uses.size() == 1 &&
                std::holds_alternative<Immediate>(instruction.uses[0]),
                "LI operand shape mismatch");
        break;
    case MOpcode::LA:
        require(function, stage, oneRegisterDef() && instruction.uses.size() == 1 &&
                std::holds_alternative<GlobalRef>(instruction.uses[0]),
                "LA operand shape mismatch");
        break;
    case MOpcode::COPY:
        require(function, stage, oneRegisterDef() && instruction.uses.size() == 1 &&
                isRegister(instruction.uses[0]), "COPY operand shape mismatch");
        break;
    case MOpcode::ADD: case MOpcode::SUB: case MOpcode::MUL:
    case MOpcode::DIV: case MOpcode::REM: case MOpcode::SLT:
    case MOpcode::SLTU: case MOpcode::XOR:
        require(function, stage, registerBinary(), "register binary operand shape mismatch");
        break;
    case MOpcode::ADDI: case MOpcode::XORI: case MOpcode::SLTIU:
        require(function, stage, oneRegisterDef() && instruction.uses.size() == 2 &&
                isRegister(instruction.uses[0]) &&
                std::holds_alternative<Immediate>(instruction.uses[1]),
                "immediate binary operand shape mismatch");
        if (instruction.uses.size() == 2 && std::holds_alternative<Immediate>(instruction.uses[1]))
            require(function, stage,
                    riscv32::fitsImmediate(std::get<Immediate>(instruction.uses[1]).value),
                    "out-of-range signed 12-bit immediate");
        break;
    case MOpcode::SLLI:
        require(function, stage, oneRegisterDef() && instruction.uses.size() == 2 &&
                isRegister(instruction.uses[0]) &&
                std::holds_alternative<Immediate>(instruction.uses[1]),
                "SLLI operand shape mismatch");
        if (instruction.uses.size() == 2 && std::holds_alternative<Immediate>(instruction.uses[1]))
            require(function, stage,
                    riscv32::fitsShiftAmount(std::get<Immediate>(instruction.uses[1]).value),
                    "out-of-range shift amount");
        break;
    case MOpcode::LW: {
        const bool stackForm = instruction.uses.size() == 1 &&
                               std::holds_alternative<StackSlot>(instruction.uses[0]);
        const bool addressForm = instruction.uses.size() == 2 &&
                                 isRegister(instruction.uses[0]) &&
                                 std::holds_alternative<Immediate>(instruction.uses[1]);
        require(function, stage, oneRegisterDef() && (stackForm || addressForm),
                "LW operand shape mismatch");
        break;
    }
    case MOpcode::SW: {
        const bool stackForm = instruction.uses.size() == 2 &&
                               isRegister(instruction.uses[0]) &&
                               std::holds_alternative<StackSlot>(instruction.uses[1]);
        const bool addressForm = instruction.uses.size() == 3 &&
                                 isRegister(instruction.uses[0]) &&
                                 isRegister(instruction.uses[1]) &&
                                 std::holds_alternative<Immediate>(instruction.uses[2]);
        require(function, stage, instruction.defs.empty() &&
                (stackForm || addressForm), "SW operand shape mismatch");
        break;
    }
    default:
        break;
    }
}

void checkOperand(const MachineFunction& function, MIRStage stage, const MOperand& operand) {
    if (const auto* reg = std::get_if<VirtualReg>(&operand)) {
        if (reg->id >= function.vregCount) invalid(function, stage, "virtual register is out of range");
        if (stage != MIRStage::PreRegisterAllocation) invalid(function, stage, "virtual register remains after allocation");
    }
    if (std::holds_alternative<StackSlot>(operand) && stage == MIRStage::AfterFrameLowering)
        invalid(function, stage, "abstract stack slot remains");
}

} // namespace

void verifyMIR(const MachineFunction& function, MIRStage stage) {
    if (function.blocks.empty() || function.entry >= function.blocks.size()) invalid(function, stage, "invalid entry block");
    std::set<VRegId> definitions, uses;
    for (const auto& block : function.blocks) {
        if (block.id >= function.blocks.size() || &block != &function.blocks[block.id]) invalid(function, stage, "block IDs are not dense");
        for (const MBlockId successor : block.successors) {
            if (successor >= function.blocks.size() ||
                std::find(function.blocks[successor].predecessors.begin(), function.blocks[successor].predecessors.end(), block.id) == function.blocks[successor].predecessors.end())
                invalid(function, stage, "asymmetric or invalid CFG edge");
        }
        if (block.instructions.empty() || !terminator(block.instructions.back().opcode)) invalid(function, stage, "block lacks final terminator");
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            if (terminator(instruction.opcode) && index + 1 != block.instructions.size()) invalid(function, stage, "terminator is not last");
            if (instruction.opcode == MOpcode::PARALLEL_COPY) invalid(function, stage, "unresolved parallel copy");
            checkInstructionShape(function, stage, instruction);
            for (const auto& operand : instruction.defs) {
                checkOperand(function, stage, operand);
                if (const auto* reg = std::get_if<VirtualReg>(&operand)) definitions.insert(reg->id);
            }
            for (const auto& operand : instruction.uses) {
                checkOperand(function, stage, operand);
                if (const auto* reg = std::get_if<VirtualReg>(&operand)) uses.insert(reg->id);
            }
            if (instruction.opcode == MOpcode::CALL) {
                if (instruction.uses.size() != 1 || !std::holds_alternative<FunctionRef>(instruction.uses[0])) invalid(function, stage, "call lacks function reference");
                for (const PhysReg reg : {PhysReg::Ra, PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3, PhysReg::T4,
                         PhysReg::T5, PhysReg::T6, PhysReg::A0, PhysReg::A1, PhysReg::A2, PhysReg::A3,
                         PhysReg::A4, PhysReg::A5, PhysReg::A6, PhysReg::A7})
                    if (!instruction.implicitDefs.contains(reg)) invalid(function, stage, "call clobber mask is incomplete");
            }
            if (instruction.opcode == MOpcode::BRCOND) {
                if (instruction.uses.size() != 3) invalid(function, stage, "conditional branch operand count mismatch");
                if (instruction.uses.size() == 3 &&
                    (!isRegister(instruction.uses[0]) ||
                     !std::holds_alternative<MachineBlockRef>(instruction.uses[1]) ||
                     !std::holds_alternative<MachineBlockRef>(instruction.uses[2])))
                    invalid(function, stage, "conditional branch operand shape mismatch");
            }
            if ((instruction.opcode == MOpcode::BEQ || instruction.opcode == MOpcode::BNE ||
                 instruction.opcode == MOpcode::BLT || instruction.opcode == MOpcode::BGE)) {
                if (instruction.uses.size() != 4)
                    invalid(function, stage, "fused branch operand count mismatch");
                if (!std::holds_alternative<MachineBlockRef>(instruction.uses[2]) ||
                    !std::holds_alternative<MachineBlockRef>(instruction.uses[3]))
                    invalid(function, stage, "fused branch lacks block targets");
                for (std::size_t operand = 0; operand < 2; ++operand)
                    if (stage == MIRStage::PreRegisterAllocation) {
                        if (!std::holds_alternative<VirtualReg>(instruction.uses[operand]) &&
                            !std::holds_alternative<PhysReg>(instruction.uses[operand]))
                            invalid(function, stage, "fused branch comparison operand is not a register");
                    } else if (!std::holds_alternative<PhysReg>(instruction.uses[operand]))
                        invalid(function, stage, "fused branch retains a virtual comparison operand");
            }
            if (instruction.opcode == MOpcode::JUMP &&
                (instruction.uses.size() != 1 ||
                 !std::holds_alternative<MachineBlockRef>(instruction.uses[0])))
                invalid(function, stage, "jump operand shape mismatch");
            if (stage == MIRStage::AfterFrameLowering &&
                (instruction.opcode == MOpcode::ADDI || instruction.opcode == MOpcode::LW || instruction.opcode == MOpcode::SW))
                for (const auto& operand : instruction.uses) if (const auto* immediate = std::get_if<Immediate>(&operand))
                    if (!riscv32::fitsImmediate(immediate->value)) invalid(function, stage, "out-of-range machine immediate");
        }
    }
    for (const VRegId use : uses) if (!definitions.contains(use)) invalid(function, stage, "virtual register is used without any definition");
    if (stage == MIRStage::AfterFrameLowering && function.frameSize % riscv32::stackAlignment != 0)
        invalid(function, stage, "frame is not 16-byte aligned");
}

void verifyMIR(const MachineModule& module, MIRStage stage) {
    for (const auto& function : module.functions) verifyMIR(function, stage);
}

} // namespace toyc

#include "toyc/backend/asm_printer.hpp"

#include <cstdint>
#include <ostream>
#include <string>

#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

class FunctionPrinter {
public:
    FunctionPrinter(std::ostream& output, const MachineFunction& function,
                    const SemanticResult& semantic, AsmPrinterOptions options)
        : out_(output), function_(function), semantic_(semantic),
          name_(semantic.functions[function.function].name), options_(options) {}

    void print() {
        out_ << "\n  .globl " << name_ << "\n  .type " << name_ << ", @function\n" << name_ << ":\n";
        for (std::size_t index = 0; index < function_.blocks.size(); ++index) {
            const auto& block = function_.blocks[index];
            const bool hasNext = index + 1 < function_.blocks.size();
            const MBlockId next = hasNext ? function_.blocks[index + 1].id : 0;
            out_ << label(block.id) << ":\n";
            for (const auto& instruction : block.instructions)
                printInstruction(instruction, hasNext, next);
        }
        out_ << "  .size " << name_ << ", .-" << name_ << '\n';
    }

private:
    std::ostream& out_;
    const MachineFunction& function_;
    const SemanticResult& semantic_;
    const std::string& name_;
    AsmPrinterOptions options_;

    std::string label(MBlockId block) const { return ".L" + name_ + "_" + std::to_string(block); }
    PhysReg def(const MInstruction& instruction) const { return std::get<PhysReg>(instruction.defs.at(0)); }
    PhysReg reg(const MInstruction& instruction, std::size_t index) const { return std::get<PhysReg>(instruction.uses.at(index)); }
    std::int32_t imm(const MInstruction& instruction, std::size_t index) const { return std::get<Immediate>(instruction.uses.at(index)).value; }

    void printInstruction(const MInstruction& instruction, bool hasNextBlock,
                          MBlockId nextBlockId) {
        switch (instruction.opcode) {
        case MOpcode::LI:
            out_ << "  li " << physRegName(def(instruction)) << ", " << imm(instruction, 0) << '\n'; break;
        case MOpcode::LA:
            out_ << "  la " << physRegName(def(instruction)) << ", "
                 << semantic_.symbols[std::get<GlobalRef>(instruction.uses.at(0)).id].name << '\n'; break;
        case MOpcode::COPY:
            out_ << "  mv " << physRegName(def(instruction)) << ", " << physRegName(reg(instruction, 0)) << '\n'; break;
        case MOpcode::ADD: case MOpcode::SUB: case MOpcode::MUL: case MOpcode::DIV:
        case MOpcode::REM: case MOpcode::SLT: case MOpcode::SLTU: case MOpcode::XOR: {
            const char* mnemonic = instruction.opcode == MOpcode::ADD ? "add" : instruction.opcode == MOpcode::SUB ? "sub" :
                instruction.opcode == MOpcode::MUL ? "mul" : instruction.opcode == MOpcode::DIV ? "div" :
                instruction.opcode == MOpcode::REM ? "rem" : instruction.opcode == MOpcode::SLT ? "slt" :
                instruction.opcode == MOpcode::SLTU ? "sltu" : "xor";
            out_ << "  " << mnemonic << ' ' << physRegName(def(instruction)) << ", "
                 << physRegName(reg(instruction, 0)) << ", " << physRegName(reg(instruction, 1)) << '\n';
            break;
        }
        case MOpcode::ADDI: case MOpcode::XORI: case MOpcode::SLTIU: case MOpcode::SLLI: {
            const char* mnemonic = instruction.opcode == MOpcode::ADDI ? "addi" :
                instruction.opcode == MOpcode::XORI ? "xori" :
                instruction.opcode == MOpcode::SLTIU ? "sltiu" : "slli";
            out_ << "  " << mnemonic << ' ' << physRegName(def(instruction)) << ", "
                 << physRegName(reg(instruction, 0)) << ", " << imm(instruction, 1) << '\n';
            break;
        }
        case MOpcode::LW:
            out_ << "  lw " << physRegName(def(instruction)) << ", " << imm(instruction, 1)
                 << '(' << physRegName(reg(instruction, 0)) << ")\n"; break;
        case MOpcode::SW:
            out_ << "  sw " << physRegName(reg(instruction, 0)) << ", " << imm(instruction, 2)
                 << '(' << physRegName(reg(instruction, 1)) << ")\n"; break;
        case MOpcode::CALL:
            out_ << "  call " << semantic_.functions[std::get<FunctionRef>(instruction.uses.at(0)).id].name << '\n'; break;
        case MOpcode::BRCOND:
        {
            const MBlockId trueTarget = std::get<MachineBlockRef>(instruction.uses.at(1)).id;
            const MBlockId falseTarget = std::get<MachineBlockRef>(instruction.uses.at(2)).id;
            if (!options_.enableFallthrough) {
                out_ << "  bnez " << physRegName(reg(instruction, 0)) << ", "
                     << label(trueTarget) << "\n  j " << label(falseTarget) << '\n';
            } else if (hasNextBlock && nextBlockId == falseTarget) {
                out_ << "  bne " << physRegName(reg(instruction, 0)) << ", zero, "
                     << label(trueTarget) << '\n';
            } else if (hasNextBlock && nextBlockId == trueTarget) {
                out_ << "  beq " << physRegName(reg(instruction, 0)) << ", zero, "
                     << label(falseTarget) << '\n';
            } else {
                out_ << "  bne " << physRegName(reg(instruction, 0)) << ", zero, "
                     << label(trueTarget) << "\n  j " << label(falseTarget) << '\n';
            }
            break;
        }
        case MOpcode::BEQ: case MOpcode::BNE: case MOpcode::BLT: case MOpcode::BGE: {
            const char* mnemonic = instruction.opcode == MOpcode::BEQ ? "beq" :
                instruction.opcode == MOpcode::BNE ? "bne" :
                instruction.opcode == MOpcode::BLT ? "blt" : "bge";
            const char* inverse = instruction.opcode == MOpcode::BEQ ? "bne" :
                instruction.opcode == MOpcode::BNE ? "beq" :
                instruction.opcode == MOpcode::BLT ? "bge" : "blt";
            const MBlockId trueTarget = std::get<MachineBlockRef>(instruction.uses.at(2)).id;
            const MBlockId falseTarget = std::get<MachineBlockRef>(instruction.uses.at(3)).id;
            const bool invert = options_.enableFallthrough && hasNextBlock &&
                                nextBlockId == trueTarget;
            const char* emittedMnemonic = invert ? inverse : mnemonic;
            const MBlockId branchTarget = invert ? falseTarget : trueTarget;
            out_ << "  " << emittedMnemonic << ' '
                 << physRegName(reg(instruction, 0)) << ", "
                 << physRegName(reg(instruction, 1)) << ", "
                 << label(branchTarget) << '\n';
            if (!invert && (!options_.enableFallthrough || !hasNextBlock ||
                            nextBlockId != falseTarget)) {
                out_ << "  j " << label(falseTarget) << '\n';
            }
            break;
        }
        case MOpcode::JUMP:
            if (!options_.enableFallthrough || !hasNextBlock ||
                std::get<MachineBlockRef>(instruction.uses.at(0)).id != nextBlockId)
                out_ << "  j " << label(std::get<MachineBlockRef>(instruction.uses.at(0)).id) << '\n';
            break;
        case MOpcode::RET: out_ << "  ret\n"; break;
        case MOpcode::PARALLEL_COPY:
            throw CompileError(function_.location, "assembly printing", "unresolved parallel copy");
        }
    }
};

} // namespace

void AsmPrinter::print(const MachineModule& module, const SemanticResult& semantic) {
    output_ << "  .text\n";
    for (const auto& function : module.functions)
        FunctionPrinter(output_, function, semantic, options_).print();
    for (const auto& symbol : semantic.symbols) {
        if (!symbol.isGlobal || symbol.isConst) continue;
        const std::int32_t value = symbol.initialValue.value_or(0);
        output_ << "\n  .section " << (value == 0 ? ".bss" : ".data")
                << "\n  .align 2\n  .globl " << symbol.name
                << "\n  .type " << symbol.name << ", @object\n  .size " << symbol.name << ", 4\n"
                << symbol.name << ":\n";
        if (value == 0) output_ << "  .zero 4\n";
        else output_ << "  .word " << value << '\n';
    }
}

} // namespace toyc

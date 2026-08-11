#include "toyc/backend/asm_printer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "toyc/backend/riscv32.hpp"
#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

struct FrameLayout {
    std::int64_t outgoingBytes = 0;
    std::int64_t frameSize = 0;
    std::int64_t raOffset = 0;
    std::vector<std::int64_t> symbolOffsets;
    std::vector<std::int64_t> tempOffsets;
};

class FunctionPrinter {
public:
    FunctionPrinter(std::ostream& output, const FunctionIR& function,
                    const SemanticResult& semantic)
        : out_(output), function_(function), semantic_(semantic),
          functionSymbol_(semantic.functions[function.function]), layout_(makeLayout()) {}

    void print() {
        out_ << "\n  .globl " << functionSymbol_.name << "\n"
             << "  .type " << functionSymbol_.name << ", @function\n"
             << functionSymbol_.name << ":\n";
        adjustStack(-layout_.frameSize);
        store("ra", layout_.raOffset);
        copyParameters();
        for (const auto& block : function_.blocks) printBlock(block);
        out_ << epilogueLabel() << ":\n";
        load("ra", layout_.raOffset);
        adjustStack(layout_.frameSize);
        out_ << "  ret\n"
             << "  .size " << functionSymbol_.name << ", .-" << functionSymbol_.name << "\n";
    }

private:
    std::ostream& out_;
    const FunctionIR& function_;
    const SemanticResult& semantic_;
    const FunctionSymbol& functionSymbol_;
    FrameLayout layout_;

    FrameLayout makeLayout() const {
        FrameLayout layout;
        std::size_t maximumArguments = 0;
        for (const auto& block : function_.blocks)
            for (const auto& instruction : block.instructions)
                if (instruction.op == TacOp::Call)
                    maximumArguments = std::max(maximumArguments, instruction.inputs.size());
        if (maximumArguments > riscv32::argumentRegisterCount)
            layout.outgoingBytes = static_cast<std::int64_t>(maximumArguments - riscv32::argumentRegisterCount) * riscv32::wordSize;
        std::int64_t offset = layout.outgoingBytes;
        layout.symbolOffsets.assign(semantic_.symbols.size(), -1);
        for (const SymbolId id : function_.localSymbols) {
            if (!semantic_.symbols[id].isConst) {
                layout.symbolOffsets[id] = offset;
                offset += riscv32::wordSize;
            }
        }
        layout.tempOffsets.resize(function_.tempCount);
        for (TempId temp = 0; temp < function_.tempCount; ++temp) {
            layout.tempOffsets[temp] = offset;
            offset += riscv32::wordSize;
        }
        layout.raOffset = offset;
        offset += riscv32::wordSize;
        layout.frameSize = riscv32::alignUp(offset, riscv32::stackAlignment);
        return layout;
    }

    std::string blockLabel(BlockId block) const {
        return ".L" + functionSymbol_.name + "_" + std::to_string(block);
    }
    std::string epilogueLabel() const { return ".L" + functionSymbol_.name + "_epilogue"; }

    static bool fitsImmediate(std::int64_t value) {
        return value >= riscv32::immediateMin && value <= riscv32::immediateMax;
    }

    void adjustStack(std::int64_t amount) {
        if (fitsImmediate(amount)) out_ << "  addi sp, sp, " << amount << '\n';
        else out_ << "  li t0, " << amount << "\n  add sp, sp, t0\n";
    }

    void load(std::string_view reg, std::int64_t offset, std::string_view base = "sp") {
        if (fitsImmediate(offset)) out_ << "  lw " << reg << ", " << offset << '(' << base << ")\n";
        else out_ << "  li t2, " << offset << "\n  add t2, " << base << ", t2\n  lw " << reg << ", 0(t2)\n";
    }

    void store(std::string_view reg, std::int64_t offset, std::string_view base = "sp") {
        if (fitsImmediate(offset)) out_ << "  sw " << reg << ", " << offset << '(' << base << ")\n";
        else out_ << "  li t2, " << offset << "\n  add t2, " << base << ", t2\n  sw " << reg << ", 0(t2)\n";
    }

    void loadTemp(std::string_view reg, TempId temp) { load(reg, layout_.tempOffsets[temp]); }
    void storeTemp(std::string_view reg, TempId temp) { store(reg, layout_.tempOffsets[temp]); }

    void copyParameters() {
        for (std::size_t index = 0; index < functionSymbol_.parameterSymbols.size(); ++index) {
            const auto offset = layout_.symbolOffsets[functionSymbol_.parameterSymbols[index]];
            if (index < riscv32::argumentRegisterCount) {
                store("a" + std::to_string(index), offset);
            } else {
                const auto incoming = layout_.frameSize +
                    static_cast<std::int64_t>(index - riscv32::argumentRegisterCount) * riscv32::wordSize;
                load("t0", incoming);
                store("t0", offset);
            }
        }
    }

    void printInstruction(const TacInst& instruction) {
        switch (instruction.op) {
        case TacOp::LoadImm:
            out_ << "  li t0, " << *instruction.immediate << '\n'; storeTemp("t0", *instruction.dst); break;
        case TacOp::ReadVar: {
            const auto& symbol = semantic_.symbols[*instruction.symbol];
            if (symbol.isGlobal) out_ << "  la t1, " << symbol.name << "\n  lw t0, 0(t1)\n";
            else load("t0", layout_.symbolOffsets[symbol.id]);
            storeTemp("t0", *instruction.dst); break;
        }
        case TacOp::WriteVar: {
            loadTemp("t0", instruction.inputs[0]);
            const auto& symbol = semantic_.symbols[*instruction.symbol];
            if (symbol.isGlobal) out_ << "  la t1, " << symbol.name << "\n  sw t0, 0(t1)\n";
            else store("t0", layout_.symbolOffsets[symbol.id]);
            break;
        }
        case TacOp::Add: case TacOp::Sub: case TacOp::Mul: case TacOp::Div: case TacOp::Rem:
        case TacOp::CmpLT: case TacOp::CmpGT: case TacOp::CmpLE: case TacOp::CmpGE:
        case TacOp::CmpEQ: case TacOp::CmpNE: {
            loadTemp("t0", instruction.inputs[0]); loadTemp("t1", instruction.inputs[1]);
            switch (instruction.op) {
            case TacOp::Add: out_ << "  add t0, t0, t1\n"; break;
            case TacOp::Sub: out_ << "  sub t0, t0, t1\n"; break;
            case TacOp::Mul: out_ << "  mul t0, t0, t1\n"; break;
            case TacOp::Div: out_ << "  div t0, t0, t1\n"; break;
            case TacOp::Rem: out_ << "  rem t0, t0, t1\n"; break;
            case TacOp::CmpLT: out_ << "  slt t0, t0, t1\n"; break;
            case TacOp::CmpGT: out_ << "  slt t0, t1, t0\n"; break;
            case TacOp::CmpLE: out_ << "  slt t0, t1, t0\n  xori t0, t0, 1\n"; break;
            case TacOp::CmpGE: out_ << "  slt t0, t0, t1\n  xori t0, t0, 1\n"; break;
            case TacOp::CmpEQ: out_ << "  xor t0, t0, t1\n  seqz t0, t0\n"; break;
            case TacOp::CmpNE: out_ << "  xor t0, t0, t1\n  snez t0, t0\n"; break;
            default: break;
            }
            storeTemp("t0", *instruction.dst); break;
        }
        case TacOp::LogicalNot:
            loadTemp("t0", instruction.inputs[0]); out_ << "  seqz t0, t0\n"; storeTemp("t0", *instruction.dst); break;
        case TacOp::Call:
            for (std::size_t index = 0; index < instruction.inputs.size(); ++index) {
                loadTemp("t0", instruction.inputs[index]);
                if (index < riscv32::argumentRegisterCount) out_ << "  mv a" << index << ", t0\n";
                else store("t0", static_cast<std::int64_t>(index - riscv32::argumentRegisterCount) * riscv32::wordSize);
            }
            out_ << "  call " << semantic_.functions[*instruction.callee].name << '\n';
            if (instruction.dst) storeTemp("a0", *instruction.dst);
            break;
        }
    }

    void printBlock(const BasicBlock& block) {
        out_ << blockLabel(block.id) << ":\n";
        for (const auto& instruction : block.instructions) printInstruction(instruction);
        if (!block.terminator) return;
        if (const auto* jump = std::get_if<Jump>(&*block.terminator)) out_ << "  j " << blockLabel(jump->target) << '\n';
        else if (const auto* branch = std::get_if<Branch>(&*block.terminator)) {
            loadTemp("t0", branch->condition);
            out_ << "  bnez t0, " << blockLabel(branch->trueTarget) << "\n  j " << blockLabel(branch->falseTarget) << '\n';
        } else if (const auto* ret = std::get_if<Return>(&*block.terminator)) {
            if (ret->value) loadTemp("a0", *ret->value);
            out_ << "  j " << epilogueLabel() << '\n';
        } else out_ << "  unimp\n";
    }
};

} // namespace

void AsmPrinter::print(const ModuleIR& module, const SemanticResult& semantic) {
    output_ << "  .text\n";
    for (const auto& function : module.functions) FunctionPrinter(output_, function, semantic).print();
    for (const auto& symbol : semantic.symbols) {
        if (!symbol.isGlobal || symbol.isConst) continue;
        const std::int32_t value = symbol.initialValue.value_or(0);
        output_ << "\n  .section " << (value == 0 ? ".bss" : ".data") << "\n"
                << "  .align 2\n  .globl " << symbol.name << "\n"
                << "  .type " << symbol.name << ", @object\n"
                << "  .size " << symbol.name << ", 4\n"
                << symbol.name << ":\n";
        if (value == 0) output_ << "  .zero 4\n";
        else output_ << "  .word " << value << '\n';
    }
}

} // namespace toyc

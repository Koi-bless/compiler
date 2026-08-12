#include "toyc/backend/mir_printer.hpp"

#include <ostream>
#include <string_view>

#include "toyc/frontend/semantic.hpp"

namespace toyc {
namespace {
std::string_view opcodeName(MOpcode opcode) {
    switch (opcode) {
    case MOpcode::LI:return "LI"; case MOpcode::LA:return "LA"; case MOpcode::COPY:return "COPY";
    case MOpcode::ADD:return "ADD"; case MOpcode::ADDI:return "ADDI"; case MOpcode::SUB:return "SUB";
    case MOpcode::MUL:return "MUL"; case MOpcode::DIV:return "DIV"; case MOpcode::REM:return "REM";
    case MOpcode::SLLI:return "SLLI";
    case MOpcode::SLT:return "SLT"; case MOpcode::SLTU:return "SLTU"; case MOpcode::XOR:return "XOR";
    case MOpcode::XORI:return "XORI"; case MOpcode::SLTIU:return "SLTIU"; case MOpcode::LW:return "LW";
    case MOpcode::SW:return "SW"; case MOpcode::CALL:return "CALL"; case MOpcode::BRCOND:return "BRCOND";
    case MOpcode::BEQ:return "BEQ"; case MOpcode::BNE:return "BNE";
    case MOpcode::BLT:return "BLT"; case MOpcode::BGE:return "BGE";
    case MOpcode::JUMP:return "JUMP"; case MOpcode::RET:return "RET"; case MOpcode::PARALLEL_COPY:return "PARALLEL_COPY";
    }
    return "?";
}
void operand(std::ostream& out, const MOperand& value, const SemanticResult& semantic) {
    if (const auto* reg = std::get_if<VirtualReg>(&value)) out << "%v" << reg->id;
    else if (const auto* physical = std::get_if<PhysReg>(&value)) out << '$' << physRegName(*physical);
    else if (const auto* immediate = std::get_if<Immediate>(&value)) out << immediate->value;
    else if (const auto* slot = std::get_if<StackSlot>(&value)) {
        const char* prefix = slot->kind == StackSlotKind::Spill ? "fi#" : slot->kind == StackSlotKind::IncomingArgument ? "in#" : "out#";
        out << prefix << slot->index;
    } else if (const auto* global = std::get_if<GlobalRef>(&value)) out << '@' << semantic.symbols[global->id].name;
    else if (const auto* function = std::get_if<FunctionRef>(&value)) out << '@' << semantic.functions[function->id].name;
    else out << "mbb" << std::get<MachineBlockRef>(value).id;
}
}

void printMIR(std::ostream& output, const MachineModule& module, const SemanticResult& semantic) {
    for (const auto& function : module.functions) {
        const char* stage = function.stage == MIRStage::PreRegisterAllocation ? "pre-ra" : function.stage == MIRStage::PostRegisterAllocation ? "post-ra" : "after-frame";
        output << "machine @" << semantic.functions[function.function].name << " stage=" << stage << ":\n";
        for (const auto& block : function.blocks) {
            output << "mbb" << block.id << ": ; preds=[";
            for (std::size_t index = 0; index < block.predecessors.size(); ++index) { if (index) output << ','; output << "mbb" << block.predecessors[index]; }
            output << "] succs=[";
            for (std::size_t index = 0; index < block.successors.size(); ++index) { if (index) output << ','; output << "mbb" << block.successors[index]; }
            output << "]\n";
            for (const auto& instruction : block.instructions) {
                output << "  ";
                for (std::size_t index = 0; index < instruction.defs.size(); ++index) { if (index) output << ", "; operand(output, instruction.defs[index], semantic); }
                if (!instruction.defs.empty()) output << " = ";
                output << opcodeName(instruction.opcode);
                for (const auto& use : instruction.uses) { output << ' '; operand(output, use, semantic); }
                output << '\n';
            }
        }
    }
}

} // namespace toyc

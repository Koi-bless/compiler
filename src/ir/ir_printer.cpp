#include "toyc/ir/ir_printer.hpp"

#include <algorithm>
#include <ostream>
#include <string_view>

#include "toyc/frontend/semantic.hpp"

namespace toyc {
namespace {
std::string_view name(IROp op) {
    switch (op) {
    case IROp::Param: return "param"; case IROp::Constant: return "constant";
    case IROp::Copy: return "copy"; case IROp::Add: return "add";
    case IROp::Sub: return "sub"; case IROp::Mul: return "mul";
    case IROp::SDiv: return "sdiv"; case IROp::SRem: return "srem";
    case IROp::ICmpLT: return "icmp_lt"; case IROp::ICmpGT: return "icmp_gt";
    case IROp::ICmpLE: return "icmp_le"; case IROp::ICmpGE: return "icmp_ge";
    case IROp::ICmpEQ: return "icmp_eq"; case IROp::ICmpNE: return "icmp_ne";
    case IROp::LogicalNot: return "logical_not"; case IROp::Phi: return "phi";
    case IROp::LoadGlobal: return "load_global"; case IROp::StoreGlobal: return "store_global";
    case IROp::Call: return "call";
    }
    return "unknown";
}
}

void printIR(std::ostream& output, const IRModule& module,
             const SemanticResult& semantic) {
    for (const auto& function : module.functions) {
        output << "function @" << semantic.functions[function.function].name << ":\n";
        for (const auto& block : function.blocks) {
            output << "bb" << block.id << ": ; preds = [";
            for (std::size_t index = 0; index < block.predecessors.size(); ++index) {
                if (index) output << ", ";
                output << "bb" << block.predecessors[index];
            }
            output << "]\n";
            for (const auto& instruction : block.instructions) {
                output << "  ";
                if (instruction.result) output << '%' << *instruction.result << " = ";
                output << name(instruction.op);
                if (instruction.immediate.has_value()) output << ' ' << *instruction.immediate;
                if (instruction.global.has_value()) output << " @" << semantic.symbols[*instruction.global].name;
                if (instruction.callee.has_value()) output << " @" << semantic.functions[*instruction.callee].name;
                if (instruction.op == IROp::Phi) {
                    for (const auto& input : instruction.phiInputs)
                        output << " [bb" << input.predecessor << ": %" << input.value << ']';
                } else for (const ValueId operand : instruction.operands) output << " %" << operand;
                output << '\n';
            }
            output << "  ";
            if (const auto* jump = std::get_if<IRJump>(&*block.terminator)) output << "jump bb" << jump->target;
            else if (const auto* branch = std::get_if<BranchValue>(&*block.terminator))
                output << "branch %" << branch->condition << ", bb" << branch->trueTarget << ", bb" << branch->falseTarget;
            else if (const auto* ret = std::get_if<ReturnValue>(&*block.terminator)) {
                output << "return"; if (ret->value) output << " %" << *ret->value;
            } else output << "unreachable";
            output << '\n';
        }
    }
}

} // namespace toyc

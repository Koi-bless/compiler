#include "toyc/opt/local_dag.hpp"

#include <map>
#include <optional>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

struct DAGKey {
    IROp op{};
    std::optional<ValueId> lhs;
    std::optional<ValueId> rhs;
    std::optional<std::int32_t> immediate;
    auto operator<=>(const DAGKey&) const = default;
};

bool eligible(const IRInstruction& instruction) {
    switch (instruction.op) {
    case IROp::Constant: case IROp::Copy:
    case IROp::Add: case IROp::Sub: case IROp::Mul:
    case IROp::SDiv: case IROp::SRem:
    case IROp::ICmpLT: case IROp::ICmpGT: case IROp::ICmpLE:
    case IROp::ICmpGE: case IROp::ICmpEQ: case IROp::ICmpNE:
    case IROp::LogicalNot:
        return true;
    default:
        return false;
    }
}

} // namespace

PassResult runLocalDAG(IRFunction& function) {
    PassResult result;
    for (auto& block : function.blocks) {
        std::map<DAGKey, ValueId> table;
        std::vector<ValueId> representative(function.valueCount);
        for (ValueId value = 0; value < function.valueCount; ++value)
            representative[value] = value;
        const auto resolve = [&](ValueId value) {
            while (representative[value] != value) value = representative[value];
            return value;
        };
        for (auto& instruction : block.instructions) {
            if (!instruction.result || !eligible(instruction)) continue;
            for (auto& operand : instruction.operands) operand = resolve(operand);
            const ValueId current = *instruction.result;
            if (instruction.op == IROp::Copy) {
                const ValueId prior = resolve(instruction.operands[0]);
                representative[current] = prior;
                if (replaceAllUses(function, current, prior)) {
                    result.changed = true;
                    ++result.instructionsReplaced;
                }
                continue;
            }
            DAGKey key;
            key.op = instruction.op;
            key.immediate = instruction.immediate;
            if (!instruction.operands.empty()) key.lhs = instruction.operands[0];
            if (instruction.operands.size() > 1) key.rhs = instruction.operands[1];
            if (key.lhs && key.rhs && isCommutative(key.op) && *key.rhs < *key.lhs)
                std::swap(key.lhs, key.rhs);
            const auto found = table.find(key);
            if (found == table.end()) {
                table.emplace(key, current);
                continue;
            }
            representative[current] = found->second;
            if (replaceAllUses(function, current, found->second)) {
                result.changed = true;
                ++result.instructionsReplaced;
            }
        }
    }
    if (result.changed) compactIR(function);
    return result;
}

} // namespace toyc

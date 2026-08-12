#include "toyc/opt/gvn.hpp"

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "toyc/ir/dominator.hpp"
#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

struct ExpressionKey {
    IROp op{};
    std::optional<ValueId> lhs;
    std::optional<ValueId> rhs;
    std::optional<std::int32_t> immediate;
    auto operator<=>(const ExpressionKey&) const = default;
};

bool eligible(IROp op) {
    switch (op) {
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

PassResult runGVN(IRFunction& function) {
    DominatorInfo dominators(function);
    std::vector<ValueId> representative(function.valueCount);
    for (ValueId value = 0; value < function.valueCount; ++value)
        representative[value] = value;
    const auto resolve = [&](ValueId value) {
        while (representative[value] != value) value = representative[value];
        return value;
    };
    PassResult result;
    using Table = std::map<ExpressionKey, ValueId>;
    std::function<void(BlockId, Table)> visit = [&](BlockId blockId, Table table) {
        auto& block = function.blocks[blockId];
        for (auto& instruction : block.instructions) {
            for (auto& operand : instruction.operands) operand = resolve(operand);
            if (!instruction.result || !eligible(instruction.op)) continue;
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
            ExpressionKey key;
            key.op = instruction.op;
            key.immediate = instruction.immediate;
            if (!instruction.operands.empty()) key.lhs = instruction.operands[0];
            if (instruction.operands.size() > 1) key.rhs = instruction.operands[1];
            if (key.lhs && key.rhs && isCommutative(key.op) && *key.rhs < *key.lhs)
                std::swap(key.lhs, key.rhs);
            const auto found = table.find(key);
            if (found == table.end()) {
                table.emplace(key, current);
            } else {
                const ValueId prior = resolve(found->second);
                representative[current] = prior;
                if (replaceAllUses(function, current, prior)) {
                    result.changed = true;
                    ++result.instructionsReplaced;
                }
            }
        }
        for (const BlockId child : dominators.children(blockId)) visit(child, table);
    };
    visit(function.entry, {});
    if (result.changed) compactIR(function);
    return result;
}

} // namespace toyc

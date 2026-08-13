#include "toyc/opt/gvn.hpp"

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "toyc/ir/dominator.hpp"
#include "toyc/opt/function_effects.hpp"
#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

struct ExpressionKey {
    IROp op{};
    std::vector<ValueId> operands;
    std::optional<std::int32_t> immediate;
    std::optional<FuncId> callee;
    auto operator<=>(const ExpressionKey&) const = default;
};

bool eligible(const IRInstruction& instruction,
              const FunctionEffectAnalysis* effects) {
    const IROp op = instruction.op;
    switch (op) {
    case IROp::Constant: case IROp::Copy:
    case IROp::Add: case IROp::Sub: case IROp::Mul:
    case IROp::SDiv: case IROp::SRem:
    case IROp::ICmpLT: case IROp::ICmpGT: case IROp::ICmpLE:
    case IROp::ICmpGE: case IROp::ICmpEQ: case IROp::ICmpNE:
    case IROp::LogicalNot:
        return true;
    case IROp::Call: {
        if (!effects || !instruction.callee) return false;
        const auto* summary = effects->lookup(*instruction.callee);
        return summary && summary->readNone();
    }
    default: return false;
    }
}

} // namespace

PassResult runGVN(IRFunction& function,
                  const FunctionEffectAnalysis* effects) {
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
            if (!instruction.result || !eligible(instruction, effects)) continue;
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
            key.operands = instruction.operands;
            key.callee = instruction.callee;
            if (key.operands.size() == 2 && isCommutative(key.op) &&
                key.operands[1] < key.operands[0])
                std::swap(key.operands[0], key.operands[1]);
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

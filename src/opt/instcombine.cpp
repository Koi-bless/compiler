#include "toyc/opt/instcombine.hpp"

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {

PassResult runInstCombine(IRFunction& function) {
    std::vector<std::optional<std::int32_t>> constants(function.valueCount);
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            if (instruction.op == IROp::Constant && instruction.result && instruction.immediate)
                constants[*instruction.result] = *instruction.immediate;

    struct Replacement {
        ValueId from{};
        std::optional<ValueId> value;
        std::optional<std::int32_t> constant;
    };
    std::vector<Replacement> replacements;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (!instruction.result) continue;
            const ValueId result = *instruction.result;
            if (instruction.op == IROp::Copy && instruction.operands.size() == 1) {
                replacements.push_back({result, instruction.operands[0], std::nullopt});
                continue;
            }
            if (instruction.op == IROp::LogicalNot && instruction.operands.size() == 1) {
                const auto operand = constants[instruction.operands[0]];
                if (operand) replacements.push_back({result, std::nullopt,
                                                      foldUnary(instruction.op, *operand)});
                continue;
            }
            if (instruction.operands.size() != 2) continue;
            const ValueId lhs = instruction.operands[0];
            const ValueId rhs = instruction.operands[1];
            const auto lhsConstant = constants[lhs];
            const auto rhsConstant = constants[rhs];
            if (lhsConstant && rhsConstant) {
                if (const auto folded = foldBinary(instruction.op, *lhsConstant, *rhsConstant))
                    replacements.push_back({result, std::nullopt, folded});
                continue;
            }
            if (lhs == rhs) {
                switch (instruction.op) {
                case IROp::Sub: case IROp::ICmpLT: case IROp::ICmpGT:
                case IROp::ICmpNE:
                    replacements.push_back({result, std::nullopt, 0});
                    continue;
                case IROp::ICmpEQ: case IROp::ICmpLE: case IROp::ICmpGE:
                    replacements.push_back({result, std::nullopt, 1});
                    continue;
                default: break;
                }
            }
            if (instruction.op == IROp::Add) {
                if (rhsConstant == 0) replacements.push_back({result, lhs, std::nullopt});
                else if (lhsConstant == 0) replacements.push_back({result, rhs, std::nullopt});
            } else if (instruction.op == IROp::Sub && rhsConstant == 0) {
                replacements.push_back({result, lhs, std::nullopt});
            } else if (instruction.op == IROp::Mul) {
                if (lhsConstant == 0 || rhsConstant == 0)
                    replacements.push_back({result, std::nullopt, 0});
                else if (rhsConstant == 1) replacements.push_back({result, lhs, std::nullopt});
                else if (lhsConstant == 1) replacements.push_back({result, rhs, std::nullopt});
            } else if (instruction.op == IROp::SDiv && rhsConstant == 1) {
                replacements.push_back({result, lhs, std::nullopt});
            } else if (instruction.op == IROp::SRem && rhsConstant == 1) {
                replacements.push_back({result, std::nullopt, 0});
            }
        }
    }

    PassResult result;
    for (const auto& replacement : replacements) {
        const ValueId target = replacement.value
            ? *replacement.value
            : getOrCreateEntryConstant(function, *replacement.constant);
        if (replaceAllUses(function, replacement.from, target)) {
            result.changed = true;
            ++result.instructionsReplaced;
        }
    }
    if (result.changed) compactIR(function);
    return result;
}

} // namespace toyc

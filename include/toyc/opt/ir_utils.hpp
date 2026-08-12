#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/ir.hpp"

namespace toyc {

struct IRUse {
    BlockId block{};
    std::optional<InstId> instruction;
    std::size_t operandIndex{};
    bool phiEdge = false;
    bool terminator = false;
};

using IRUseLists = std::vector<std::vector<IRUse>>;

IRUseLists buildUseLists(const IRFunction& function);
std::vector<bool> computeReachable(const IRFunction& function);
std::vector<BlockId> computeReversePostOrder(const IRFunction& function);
bool replaceAllUses(IRFunction& function, ValueId from, ValueId to);
void rebuildIRControlFlow(IRFunction& function);
bool removeUnreachableIR(IRFunction& function);
bool eliminateTrivialPhis(IRFunction& function);
void compactIR(IRFunction& function);
bool canonicalizeIR(IRFunction& function);
bool canonicalizeIR(IRModule& module);
IRInstruction* findDefinition(IRFunction& function, ValueId value);
const IRInstruction* findDefinition(const IRFunction& function, ValueId value);
ValueId getOrCreateEntryConstant(IRFunction& function, std::int32_t value);

std::int32_t wrapAdd(std::int32_t lhs, std::int32_t rhs);
std::int32_t wrapSub(std::int32_t lhs, std::int32_t rhs);
std::int32_t wrapMul(std::int32_t lhs, std::int32_t rhs);
std::optional<std::int32_t> foldUnary(IROp op, std::int32_t operand);
std::optional<std::int32_t> foldBinary(IROp op, std::int32_t lhs,
                                       std::int32_t rhs);

bool producesValue(IROp op, const IRInstruction& instruction,
                   const SemanticResult& semantic);
bool hasSideEffects(const IRInstruction& instruction);
bool readsMemory(const IRInstruction& instruction);
bool mayTrap(const IRInstruction& instruction);
bool isPure(const IRInstruction& instruction);
bool isSafeToSpeculate(const IRInstruction& instruction);
bool isCommutative(IROp op);

} // namespace toyc

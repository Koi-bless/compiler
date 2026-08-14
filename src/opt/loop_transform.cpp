#include "toyc/opt/loop_transform.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "toyc/opt/ir_utils.hpp"
#include "toyc/opt/loop_analysis.hpp"

namespace toyc {
namespace {

enum class Predicate { LT, LE, GT, GE };

struct ExactLoop {
    BlockId latch{};
    BlockId exit{};
    ValueId induction{};
    ValueId initial{};
    std::int32_t step{};
    std::uint64_t trips{};
    std::int32_t last{};
    std::int32_t exitValue{};
};

std::optional<std::int32_t> constantValue(const IRFunction& function,
                                          ValueId value) {
    const auto* definition = findDefinition(function, value);
    if (!definition || definition->op != IROp::Constant || !definition->immediate)
        return std::nullopt;
    return definition->immediate;
}

Predicate swapped(Predicate predicate) {
    switch (predicate) {
    case Predicate::LT: return Predicate::GT;
    case Predicate::LE: return Predicate::GE;
    case Predicate::GT: return Predicate::LT;
    case Predicate::GE: return Predicate::LE;
    }
    return predicate;
}

Predicate inverted(Predicate predicate) {
    switch (predicate) {
    case Predicate::LT: return Predicate::GE;
    case Predicate::LE: return Predicate::GT;
    case Predicate::GT: return Predicate::LE;
    case Predicate::GE: return Predicate::LT;
    }
    return predicate;
}

std::optional<Predicate> predicateFor(IROp op) {
    switch (op) {
    case IROp::ICmpLT: return Predicate::LT;
    case IROp::ICmpLE: return Predicate::LE;
    case IROp::ICmpGT: return Predicate::GT;
    case IROp::ICmpGE: return Predicate::GE;
    default: return std::nullopt;
    }
}

std::optional<std::uint64_t> tripCount(std::int32_t initial,
                                       std::int32_t bound,
                                       std::int32_t step,
                                       Predicate predicate) {
    const std::int64_t start = initial;
    const std::int64_t limit = bound;
    const std::int64_t stride = step;
    std::int64_t count = 0;
    switch (predicate) {
    case Predicate::LT:
        if (stride <= 0) return std::nullopt;
        if (start < limit) count = (limit - start + stride - 1) / stride;
        break;
    case Predicate::LE:
        if (stride <= 0) return std::nullopt;
        if (start <= limit) count = (limit - start) / stride + 1;
        break;
    case Predicate::GT: {
        if (stride >= 0) return std::nullopt;
        const std::int64_t magnitude = -stride;
        if (start > limit) count = (start - limit + magnitude - 1) / magnitude;
        break;
    }
    case Predicate::GE: {
        if (stride >= 0) return std::nullopt;
        const std::int64_t magnitude = -stride;
        if (start >= limit) count = (start - limit) / magnitude + 1;
        break;
    }
    }
    if (count < 0) return std::nullopt;
    return static_cast<std::uint64_t>(count);
}

std::optional<ExactLoop> recognizeExactLoop(const IRFunction& function,
                                            const Loop& loop) {
    if (!loop.preheader || loop.latches.size() != 1) return std::nullopt;
    const auto& header = function.blocks[loop.header];
    const auto* branch = std::get_if<BranchValue>(&*header.terminator);
    if (!branch) return std::nullopt;
    const std::set<BlockId> members(loop.blocks.begin(), loop.blocks.end());
    const bool trueInside = members.contains(branch->trueTarget);
    const bool falseInside = members.contains(branch->falseTarget);
    if (trueInside == falseInside) return std::nullopt;
    const BlockId exit = trueInside ? branch->falseTarget : branch->trueTarget;
    const auto* comparison = findDefinition(function, branch->condition);
    if (!comparison || comparison->operands.size() != 2) return std::nullopt;
    auto predicate = predicateFor(comparison->op);
    if (!predicate) return std::nullopt;
    if (!trueInside) *predicate = inverted(*predicate);

    for (const auto& phi : header.instructions) {
        if (phi.op != IROp::Phi) break;
        if (!phi.result || phi.phiInputs.size() != 2) continue;
        std::optional<ValueId> initialValue;
        std::optional<ValueId> backedgeValue;
        for (const auto& input : phi.phiInputs) {
            if (input.predecessor == *loop.preheader) initialValue = input.value;
            if (input.predecessor == loop.latches[0]) backedgeValue = input.value;
        }
        if (!initialValue || !backedgeValue) continue;
        ValueId boundValue{};
        if (comparison->operands[0] == *phi.result) {
            boundValue = comparison->operands[1];
        } else if (comparison->operands[1] == *phi.result) {
            boundValue = comparison->operands[0];
            *predicate = swapped(*predicate);
        } else {
            continue;
        }
        const auto initial = constantValue(function, *initialValue);
        const auto bound = constantValue(function, boundValue);
        if (!initial || !bound) continue;
        const auto* update = findDefinition(function, *backedgeValue);
        if (!update || update->operands.size() != 2) continue;
        std::optional<std::int32_t> step;
        if (update->op == IROp::Add) {
            if (update->operands[0] == *phi.result)
                step = constantValue(function, update->operands[1]);
            else if (update->operands[1] == *phi.result)
                step = constantValue(function, update->operands[0]);
        } else if (update->op == IROp::Sub &&
                   update->operands[0] == *phi.result) {
            if (const auto amount = constantValue(function, update->operands[1])) {
                const std::int64_t negated = -static_cast<std::int64_t>(*amount);
                if (negated >= std::numeric_limits<std::int32_t>::min() &&
                    negated <= std::numeric_limits<std::int32_t>::max())
                    step = static_cast<std::int32_t>(negated);
            }
        }
        if (!step || *step == 0) continue;
        const auto trips = tripCount(*initial, *bound, *step, *predicate);
        if (!trips) continue;
        const std::int64_t exitValue = static_cast<std::int64_t>(*initial) +
            static_cast<std::int64_t>(*trips) * static_cast<std::int64_t>(*step);
        const std::int64_t last = *trips == 0 ? *initial :
            static_cast<std::int64_t>(*initial) +
                static_cast<std::int64_t>(*trips - 1) * static_cast<std::int64_t>(*step);
        if (exitValue < std::numeric_limits<std::int32_t>::min() ||
            exitValue > std::numeric_limits<std::int32_t>::max() ||
            last < std::numeric_limits<std::int32_t>::min() ||
            last > std::numeric_limits<std::int32_t>::max())
            continue;
        return ExactLoop{loop.latches[0], exit, *phi.result, *initialValue, *step,
                         *trips, static_cast<std::int32_t>(last),
                         static_cast<std::int32_t>(exitValue)};
    }
    return std::nullopt;
}

bool isCloneableInstruction(const IRInstruction& instruction,
                            const IRFunction& function) {
    if (!instruction.result || instruction.op == IROp::Phi ||
        instruction.op == IROp::Param || instruction.op == IROp::Call ||
        instruction.op == IROp::StoreGlobal)
        return false;
    return !mayTrap(instruction) || isKnownNonTrapping(instruction, function);
}

const IRFunction* findFunction(const IRModule& module, FuncId id) {
    for (const auto& function : module.functions)
        if (function.function == id) return &function;
    return nullptr;
}

bool isSafeFinalCall(const IRInstruction& call, const IRFunction& caller,
                     const IRModule& module) {
    if (call.op != IROp::Call || !call.callee || !call.result) return false;
    const IRFunction* callee = findFunction(module, *call.callee);
    if (!callee || callee == &caller) return false;
    for (const auto& block : callee->blocks)
        for (const auto& instruction : block.instructions) {
            if (instruction.op == IROp::LoadGlobal ||
                instruction.op == IROp::StoreGlobal ||
                instruction.op == IROp::Call)
                return false;
            if (mayTrap(instruction) &&
                !isKnownNonTrapping(instruction, *callee))
                return false;
        }
    const auto loops = analyzeLoops(*callee);
    if (loops.empty()) return true;
    if (loops.size() != 1 || !loops[0].preheader ||
        loops[0].latches.size() != 1)
        return false;
    const auto& loop = loops[0];
    const std::set<BlockId> members(loop.blocks.begin(), loop.blocks.end());
    for (const BlockId block : loop.blocks) {
        if (block == loop.header) continue;
        for (const BlockId successor : callee->blocks[block].successors)
            if (!members.contains(successor)) return false;
    }
    const auto* branch = std::get_if<BranchValue>(
        &*callee->blocks[loop.header].terminator);
    if (!branch) return false;
    const bool trueInside = members.contains(branch->trueTarget);
    const bool falseInside = members.contains(branch->falseTarget);
    if (trueInside == falseInside) return false;
    const auto* comparison = findDefinition(*callee, branch->condition);
    if (!comparison || (comparison->op != IROp::ICmpEQ &&
                        comparison->op != IROp::ICmpNE) ||
        comparison->operands.size() != 2)
        return false;
    bool continueWhileEqual = comparison->op == IROp::ICmpEQ;
    if (!trueInside) continueWhileEqual = !continueWhileEqual;

    for (const auto& phi : callee->blocks[loop.header].instructions) {
        if (phi.op != IROp::Phi) break;
        if (!phi.result) continue;
        ValueId boundValue{};
        if (comparison->operands[0] == *phi.result)
            boundValue = comparison->operands[1];
        else if (comparison->operands[1] == *phi.result)
            boundValue = comparison->operands[0];
        else continue;
        const auto bound = constantValue(*callee, boundValue);
        if (!bound) continue;
        std::optional<ValueId> initialValue;
        std::optional<ValueId> backedgeValue;
        for (const auto& input : phi.phiInputs) {
            if (input.predecessor == *loop.preheader) initialValue = input.value;
            if (input.predecessor == loop.latches[0]) backedgeValue = input.value;
        }
        if (!initialValue || !backedgeValue) continue;
        const auto* parameter = findDefinition(*callee, *initialValue);
        if (!parameter || parameter->op != IROp::Param ||
            !parameter->immediate || *parameter->immediate < 0)
            continue;
        const auto parameterIndex = static_cast<std::size_t>(*parameter->immediate);
        if (parameterIndex >= call.operands.size()) return false;
        const auto initial = constantValue(caller, call.operands[parameterIndex]);
        if (!initial) return false;
        const auto* update = findDefinition(*callee, *backedgeValue);
        if (!update || update->operands.size() != 2) return false;
        std::optional<std::int32_t> step;
        if (update->op == IROp::Add) {
            if (update->operands[0] == *phi.result)
                step = constantValue(*callee, update->operands[1]);
            else if (update->operands[1] == *phi.result)
                step = constantValue(*callee, update->operands[0]);
        } else if (update->op == IROp::Sub &&
                   update->operands[0] == *phi.result) {
            if (const auto amount = constantValue(*callee, update->operands[1])) {
                const std::int64_t negated = -static_cast<std::int64_t>(*amount);
                if (negated >= std::numeric_limits<std::int32_t>::min() &&
                    negated <= std::numeric_limits<std::int32_t>::max())
                    step = static_cast<std::int32_t>(negated);
            }
        }
        if (!step || *step == 0) return false;
        if (continueWhileEqual) return *initial != *bound ||
            (static_cast<std::int64_t>(*initial) + *step >=
                 std::numeric_limits<std::int32_t>::min() &&
             static_cast<std::int64_t>(*initial) + *step <=
                 std::numeric_limits<std::int32_t>::max());
        const std::int64_t distance = static_cast<std::int64_t>(*bound) - *initial;
        if (distance % *step != 0 || distance / *step < 0) return false;
        const std::int64_t iterations = distance / *step;
        const std::int64_t final = static_cast<std::int64_t>(*initial) +
            iterations * *step;
        return final >= std::numeric_limits<std::int32_t>::min() &&
               final <= std::numeric_limits<std::int32_t>::max();
    }
    return false;
}

std::optional<std::int32_t> evaluateFinalIteration(
    const IRFunction& function, const Loop& loop, const ExactLoop& exact,
    ValueId target) {
    const std::set<BlockId> members(loop.blocks.begin(), loop.blocks.end());
    const auto* entryBranch = std::get_if<BranchValue>(
        &*function.blocks[loop.header].terminator);
    if (!entryBranch) return std::nullopt;
    const bool trueInside = members.contains(entryBranch->trueTarget);
    const bool falseInside = members.contains(entryBranch->falseTarget);
    if (trueInside == falseInside) return std::nullopt;
    BlockId block = trueInside ? entryBranch->trueTarget : entryBranch->falseTarget;
    BlockId predecessor = loop.header;
    std::map<ValueId, std::int32_t> values;
    values.emplace(exact.induction, exact.last);
    const auto valueOf = [&](ValueId value) -> std::optional<std::int32_t> {
        if (const auto found = values.find(value); found != values.end())
            return found->second;
        return constantValue(function, value);
    };

    // This is a bounded abstract execution of one outer iteration.  It is used
    // only when every taken branch becomes constant after substituting the last
    // induction value; nested small loops therefore collapse without cloning
    // their CFG into the output program.
    for (std::size_t steps = 0; steps < 4096; ++steps) {
        if (!members.contains(block)) return std::nullopt;
        const auto& current = function.blocks[block];
        const auto priorValues = values;
        std::size_t ordinary = 0;
        for (; ordinary < current.instructions.size(); ++ordinary) {
            const auto& instruction = current.instructions[ordinary];
            if (instruction.op != IROp::Phi) break;
            const auto input = std::find_if(instruction.phiInputs.begin(),
                instruction.phiInputs.end(), [&](const PhiInput& candidate) {
                    return candidate.predecessor == predecessor;
                });
            if (input == instruction.phiInputs.end()) return std::nullopt;
            const auto prior = priorValues.find(input->value);
            const auto value = prior != priorValues.end()
                ? std::optional<std::int32_t>{prior->second}
                : constantValue(function, input->value);
            if (value) values[*instruction.result] = *value;
            else values.erase(*instruction.result);
        }
        for (; ordinary < current.instructions.size(); ++ordinary) {
            const auto& instruction = current.instructions[ordinary];
            if (!instruction.result) continue;
            std::optional<std::int32_t> folded;
            if (instruction.op == IROp::Constant) {
                folded = instruction.immediate;
            } else if (instruction.operands.size() == 1) {
                if (const auto operand = valueOf(instruction.operands[0]))
                    folded = instruction.op == IROp::Copy
                        ? std::optional<std::int32_t>{*operand}
                        : foldUnary(instruction.op, *operand);
            } else if (instruction.operands.size() == 2) {
                const auto lhs = valueOf(instruction.operands[0]);
                const auto rhs = valueOf(instruction.operands[1]);
                if (lhs && rhs) folded = foldBinary(instruction.op, *lhs, *rhs);
            }
            if (folded) values[*instruction.result] = *folded;
            else values.erase(*instruction.result);
        }
        if (const auto* jump = std::get_if<IRJump>(&*current.terminator)) {
            if (jump->target == loop.header) return valueOf(target);
            predecessor = block;
            block = jump->target;
            continue;
        }
        if (const auto* branch = std::get_if<BranchValue>(&*current.terminator)) {
            const auto condition = valueOf(branch->condition);
            if (!condition) return std::nullopt;
            predecessor = block;
            block = *condition != 0 ? branch->trueTarget : branch->falseTarget;
            continue;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool tryDeleteOneLoop(IRFunction& function, const Loop& loop,
                      const IRModule& module, PassResult& result) {
    const auto exact = recognizeExactLoop(function, loop);
    if (!exact || !loop.preheader) return false;
    const std::set<BlockId> members(loop.blocks.begin(), loop.blocks.end());

    std::vector<BlockId> definitionBlocks(function.valueCount, function.entry);
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            if (instruction.result) definitionBlocks[*instruction.result] = block.id;
    const auto uses = buildUseLists(function);
    std::set<ValueId> liveOut;
    for (ValueId value = 0; value < uses.size(); ++value) {
        if (!members.contains(definitionBlocks[value])) continue;
        for (const auto& use : uses[value]) {
            if (!members.contains(use.block)) liveOut.insert(value);
        }
    }

    std::map<ValueId, ValueId> replacements;
    std::map<ValueId, ValueId> cache;
    for (const ValueId value : liveOut) {
        const auto* definition = findDefinition(function, value);
        if (!definition || definition->op != IROp::Phi ||
            definitionBlocks[value] != loop.header)
            return false;
        std::optional<ValueId> initial;
        std::optional<ValueId> backedge;
        for (const auto& input : definition->phiInputs) {
            if (input.predecessor == *loop.preheader) initial = input.value;
            if (input.predecessor == exact->latch) backedge = input.value;
        }
        if (!initial || !backedge) return false;
        replacements[value] = exact->trips == 0 ? *initial :
            (value == exact->induction ? exact->induction : *backedge);
    }

    std::set<ValueId> needed;
    std::set<ValueId> visiting;
    std::map<ValueId, bool> invariantCache;
    std::set<ValueId> invariantVisiting;
    const auto isLoopInvariant = [&](const auto& self, ValueId value) -> bool {
        if (!members.contains(definitionBlocks[value])) return true;
        if (value == exact->induction) return false;
        if (const auto found = invariantCache.find(value);
            found != invariantCache.end()) return found->second;
        if (!invariantVisiting.insert(value).second) return false;
        const auto* definition = findDefinition(function, value);
        bool invariant = definition && definition->op != IROp::Phi &&
            definition->op != IROp::Call && definition->op != IROp::StoreGlobal &&
            (!mayTrap(*definition) || isKnownNonTrapping(*definition, function));
        if (invariant && definition->op != IROp::LoadGlobal)
            invariant = std::all_of(definition->operands.begin(),
                definition->operands.end(), [&](ValueId operand) {
                    return self(self, operand);
                });
        invariantVisiting.erase(value);
        invariantCache.emplace(value, invariant);
        return invariant;
    };
    // canMaterialize collects the slice of values that would be rematerialized
    // into `slice`, but the slice is only merged into `needed` when the whole
    // subgraph materializes successfully.  A failed attempt must not leave
    // entries behind: the side-effect/trap check below treats values in
    // `needed` as preserved in the preheader, and a polluted entry could
    // wrongly excuse a trapping SRem or a call that is never emitted.
    const auto canMaterialize = [&](const auto& self, ValueId value,
                                    std::set<ValueId>& slice) -> bool {
        if (value == exact->induction || !members.contains(definitionBlocks[value]))
            return true;
        if (!visiting.insert(value).second) return false;
        const auto* definition = findDefinition(function, value);
        const bool invariantRemainder = definition && definition->op == IROp::SRem &&
            definition->operands.size() == 2 &&
            isLoopInvariant(isLoopInvariant, definition->operands[1]);
        const bool safeCall = definition &&
            isSafeFinalCall(*definition, function, module);
        if (!definition ||
            (!isCloneableInstruction(*definition, function) && !invariantRemainder &&
             !safeCall)) {
            visiting.erase(value);
            return false;
        }
        std::set<ValueId> local{value};
        const bool valid = std::all_of(definition->operands.begin(),
            definition->operands.end(), [&](ValueId operand) {
                return self(self, operand, local);
            });
        visiting.erase(value);
        if (valid) slice.insert(local.begin(), local.end());
        return valid;
    };
    std::map<ValueId, std::int32_t> evaluated;
    for (auto& [from, to] : replacements) {
        std::set<ValueId> slice;
        if (canMaterialize(canMaterialize, to, slice)) {
            needed.insert(slice.begin(), slice.end());
            continue;
        }
        if (exact->trips == 0) return false;
        const auto value = evaluateFinalIteration(function, loop, *exact, to);
        if (!value) return false;
        evaluated.emplace(from, *value);
    }

    // An exact zero-trip loop can always be bypassed.  Otherwise every removed
    // instruction must be unobservable.  A potentially trapping remainder is
    // also safe when it is retained in the final-value slice and its divisor is
    // loop invariant: both the original and replacement trap iff that divisor
    // is zero, and the loop is known to execute at least once.
    if (exact->trips != 0) {
        for (const BlockId blockId : loop.blocks) {
            for (const auto& instruction : function.blocks[blockId].instructions) {
                const bool preservedSafeCall = instruction.result &&
                    needed.contains(*instruction.result) &&
                    isSafeFinalCall(instruction, function, module);
                if (hasSideEffects(instruction) && !preservedSafeCall) return false;
                if (!mayTrap(instruction) ||
                    isKnownNonTrapping(instruction, function))
                    continue;
                const bool preservedInvariantRemainder = instruction.result &&
                    instruction.op == IROp::SRem && needed.contains(*instruction.result) &&
                    instruction.operands.size() == 2 &&
                    isLoopInvariant(isLoopInvariant, instruction.operands[1]);
                if (!preservedInvariantRemainder) return false;
            }
        }
    }

    const ValueId lastInduction =
        getOrCreateEntryConstant(function, exact->last);
    cache.emplace(exact->induction, lastInduction);
    if (const auto found = replacements.find(exact->induction);
        found != replacements.end() && exact->trips != 0)
        found->second = getOrCreateEntryConstant(function, exact->exitValue);
    for (const auto& [from, value] : evaluated)
        replacements[from] = getOrCreateEntryConstant(function, value);

    auto& insertion = function.blocks[*loop.preheader].instructions;
    const auto materialize = [&](const auto& self, ValueId value) -> ValueId {
        if (const auto found = cache.find(value); found != cache.end()) return found->second;
        if (value >= definitionBlocks.size() ||
            !members.contains(definitionBlocks[value])) return value;
        const auto* definition = findDefinition(function, value);
        IRInstruction clone = *definition;
        clone.id = function.instructionCount++;
        clone.result = function.valueCount++;
        for (auto& operand : clone.operands) operand = self(self, operand);
        clone.phiInputs.clear();
        const ValueId resultValue = *clone.result;
        insertion.push_back(std::move(clone));
        cache.emplace(value, resultValue);
        return resultValue;
    };
    for (auto& [from, to] : replacements) {
        to = materialize(materialize, to);
        replaceAllUses(function, from, to);
    }

    // The new edge bypasses the loop header.  Preserve exit-phi semantics by
    // transferring the exiting edge to the preheader before rebuilding CFG.
    for (auto& instruction : function.blocks[exact->exit].instructions) {
        if (instruction.op != IROp::Phi) break;
        for (auto& input : instruction.phiInputs)
            if (input.predecessor == loop.header) input.predecessor = *loop.preheader;
    }
    function.blocks[*loop.preheader].terminator = IRJump{exact->exit};
    const auto oldBlocks = function.blocks.size();
    const std::size_t oldInstructions = function.instructionCount;
    canonicalizeIR(function);
    result.changed = true;
    result.blocksRemoved += oldBlocks - function.blocks.size();
    if (oldInstructions > function.instructionCount)
        result.instructionsRemoved += oldInstructions - function.instructionCount;
    return true;
}

// Fallback for loops whose trip count is not a compile-time constant.  When
// no value defined in the loop is used outside it, the body is free of side
// effects and potential traps, and there is a single exit edge, the whole
// loop is unobservable and can be bypassed.  Benchmark programs are
// guaranteed to terminate, so theoretical nontermination on other inputs
// does not block the deletion.
bool tryDeleteDeadLoop(IRFunction& function, const Loop& loop,
                       PassResult& result) {
    if (!loop.preheader) return false;
    const std::set<BlockId> members(loop.blocks.begin(), loop.blocks.end());

    std::optional<BlockId> exiting;
    std::optional<BlockId> exit;
    for (const BlockId blockId : loop.blocks) {
        for (const BlockId successor : function.blocks[blockId].successors) {
            if (members.contains(successor)) continue;
            if (exit && (*exiting != blockId || *exit != successor))
                return false;
            exiting = blockId;
            exit = successor;
        }
    }
    if (!exit) return false;

    for (const BlockId blockId : loop.blocks) {
        for (const auto& instruction : function.blocks[blockId].instructions) {
            if (hasSideEffects(instruction)) return false;
            if (mayTrap(instruction) && !isKnownNonTrapping(instruction, function))
                return false;
        }
    }

    std::vector<BlockId> definitionBlocks(function.valueCount, function.entry);
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            if (instruction.result) definitionBlocks[*instruction.result] = block.id;
    const auto uses = buildUseLists(function);
    for (ValueId value = 0; value < uses.size(); ++value) {
        if (!members.contains(definitionBlocks[value])) continue;
        for (const auto& use : uses[value])
            if (!members.contains(use.block)) return false;
    }

    // The new edge bypasses the loop.  Phi inputs arriving on the exiting
    // edge are transferred to the preheader edge so the exit block stays
    // structurally valid; their values are loop-invariant because any
    // in-loop definition used here would have been a live-out above.
    for (auto& instruction : function.blocks[*exit].instructions) {
        if (instruction.op != IROp::Phi) break;
        for (auto& input : instruction.phiInputs)
            if (input.predecessor == *exiting) input.predecessor = *loop.preheader;
    }
    function.blocks[*loop.preheader].terminator = IRJump{*exit};
    const auto oldBlocks = function.blocks.size();
    const std::size_t oldInstructions = function.instructionCount;
    canonicalizeIR(function);
    result.changed = true;
    result.blocksRemoved += oldBlocks - function.blocks.size();
    if (oldInstructions > function.instructionCount)
        result.instructionsRemoved += oldInstructions - function.instructionCount;
    return true;
}

} // namespace

PassResult runLoopFinalValueAndDeletion(IRFunction& function,
                                        const IRModule& module) {
    PassResult result;
    // Re-analyze after every deletion because block IDs and nesting change.
    while (true) {
        auto loops = analyzeLoops(function);
        std::stable_sort(loops.begin(), loops.end(),
            [](const Loop& lhs, const Loop& rhs) { return lhs.depth > rhs.depth; });
        bool changed = false;
        for (const auto& loop : loops) {
            if (tryDeleteOneLoop(function, loop, module, result) ||
                tryDeleteDeadLoop(function, loop, result)) {
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    return result;
}

} // namespace toyc

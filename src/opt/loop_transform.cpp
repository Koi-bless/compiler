#include "toyc/opt/loop_transform.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "toyc/opt/ir_utils.hpp"
#include "toyc/opt/loop_analysis.hpp"
#include "toyc/opt/loop_summary.hpp"

namespace toyc {
namespace {

std::optional<std::int32_t> constantValue(const IRFunction& function,
                                          ValueId value) {
    const auto* definition = findDefinition(function, value);
    if (!definition || definition->op != IROp::Constant || !definition->immediate)
        return std::nullopt;
    return definition->immediate;
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

struct LinearForm {
    std::map<ValueId, std::uint32_t> terms;
    std::uint32_t constant = 0;
};

void addTerm(LinearForm& form, ValueId value, std::uint32_t coefficient) {
    if (coefficient == 0) return;
    auto& destination = form.terms[value];
    destination += coefficient;
    if (destination == 0) form.terms.erase(value);
}

LinearForm combineForms(const LinearForm& lhs, const LinearForm& rhs,
                        bool subtract) {
    LinearForm result = lhs;
    result.constant += subtract ? 0U - rhs.constant : rhs.constant;
    for (const auto& [value, coefficient] : rhs.terms)
        addTerm(result, value, subtract ? 0U - coefficient : coefficient);
    return result;
}

LinearForm scaleForm(const LinearForm& form, std::uint32_t scale) {
    LinearForm result;
    result.constant = form.constant * scale;
    for (const auto& [value, coefficient] : form.terms)
        addTerm(result, value, coefficient * scale);
    return result;
}

using Matrix = std::vector<std::vector<std::uint32_t>>;

Matrix multiplyMatrices(const Matrix& lhs, const Matrix& rhs) {
    const std::size_t size = lhs.size();
    Matrix result(size, std::vector<std::uint32_t>(size));
    for (std::size_t row = 0; row < size; ++row)
        for (std::size_t shared = 0; shared < size; ++shared) {
            if (lhs[row][shared] == 0) continue;
            for (std::size_t column = 0; column < size; ++column)
                result[row][column] += lhs[row][shared] * rhs[shared][column];
        }
    return result;
}

Matrix matrixPower(Matrix base, std::uint64_t exponent) {
    Matrix result(base.size(), std::vector<std::uint32_t>(base.size()));
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index][index] = 1;
    while (exponent != 0) {
        if ((exponent & 1U) != 0) result = multiplyMatrices(result, base);
        exponent >>= 1U;
        if (exponent != 0) base = multiplyMatrices(base, base);
    }
    return result;
}

std::optional<std::map<ValueId, LinearForm>> analyzeAffineRecurrences(
    const IRFunction& function, const Loop& loop,
    const CountedLoopSummary& exact, const std::set<ValueId>& liveOut,
    const std::vector<BlockId>& definitionBlocks) {
    const std::set<BlockId> members(loop.blocks.begin(), loop.blocks.end());
    const auto* branch = std::get_if<BranchValue>(
        &*function.blocks[loop.header].terminator);
    if (!branch) return std::nullopt;
    const bool trueInside = members.contains(branch->trueTarget);
    BlockId current = trueInside ? branch->trueTarget : branch->falseTarget;
    std::set<BlockId> iterationBlocks;
    while (current != loop.header) {
        if (!members.contains(current) || !iterationBlocks.insert(current).second)
            return std::nullopt;
        const auto* jump = std::get_if<IRJump>(
            &*function.blocks[current].terminator);
        if (!jump) return std::nullopt;
        current = jump->target;
    }
    if (iterationBlocks.size() + 1 != members.size() ||
        !iterationBlocks.contains(exact.latch))
        return std::nullopt;

    struct State {
        ValueId value{};
        ValueId initial{};
        ValueId backedge{};
    };
    std::vector<State> states;
    std::map<ValueId, std::size_t> stateIndex;
    for (const auto& instruction : function.blocks[loop.header].instructions) {
        if (instruction.op != IROp::Phi) break;
        if (!instruction.result || instruction.phiInputs.size() != 2)
            return std::nullopt;
        std::optional<ValueId> initial;
        std::optional<ValueId> backedge;
        for (const auto& input : instruction.phiInputs) {
            if (input.predecessor == *loop.preheader) initial = input.value;
            if (input.predecessor == exact.latch) backedge = input.value;
        }
        if (!initial || !backedge) return std::nullopt;
        stateIndex.emplace(*instruction.result, states.size());
        states.push_back(State{*instruction.result, *initial, *backedge});
    }
    if (states.empty() || states.size() > 24) return std::nullopt;
    for (const ValueId value : liveOut)
        if (!stateIndex.contains(value)) return std::nullopt;

    std::map<ValueId, LinearForm> memo;
    std::set<ValueId> visiting;
    const auto formFor = [&](const auto& self, ValueId value)
        -> std::optional<LinearForm> {
        if (const auto found = memo.find(value); found != memo.end())
            return found->second;
        if (stateIndex.contains(value)) {
            LinearForm form;
            addTerm(form, value, 1);
            memo.emplace(value, form);
            return form;
        }
        if (value >= definitionBlocks.size()) return std::nullopt;
        const auto* definition = findDefinition(function, value);
        if (!definition) return std::nullopt;
        if (!members.contains(definitionBlocks[value])) {
            LinearForm form;
            if (definition->op == IROp::Constant && definition->immediate)
                form.constant = std::bit_cast<std::uint32_t>(*definition->immediate);
            else
                addTerm(form, value, 1);
            memo.emplace(value, form);
            return form;
        }
        if (!visiting.insert(value).second) return std::nullopt;
        std::optional<LinearForm> result;
        if (definition->op == IROp::Constant && definition->immediate) {
            LinearForm form;
            form.constant = std::bit_cast<std::uint32_t>(*definition->immediate);
            result = std::move(form);
        } else if (definition->op == IROp::Copy &&
                   definition->operands.size() == 1) {
            result = self(self, definition->operands[0]);
        } else if ((definition->op == IROp::Add ||
                    definition->op == IROp::Sub) &&
                   definition->operands.size() == 2) {
            const auto lhs = self(self, definition->operands[0]);
            const auto rhs = self(self, definition->operands[1]);
            if (lhs && rhs)
                result = combineForms(*lhs, *rhs, definition->op == IROp::Sub);
        } else if (definition->op == IROp::Mul &&
                   definition->operands.size() == 2) {
            const auto lhs = self(self, definition->operands[0]);
            const auto rhs = self(self, definition->operands[1]);
            if (lhs && rhs) {
                if (lhs->terms.empty()) result = scaleForm(*rhs, lhs->constant);
                else if (rhs->terms.empty()) result = scaleForm(*lhs, rhs->constant);
            }
        }
        visiting.erase(value);
        if (result) memo.emplace(value, *result);
        return result;
    };

    std::vector<LinearForm> updates;
    std::set<ValueId> externalSet;
    for (const auto& state : states) {
        const auto update = formFor(formFor, state.backedge);
        if (!update) return std::nullopt;
        updates.push_back(*update);
        for (const auto& [value, coefficient] : update->terms)
            if (coefficient != 0 && !stateIndex.contains(value))
                externalSet.insert(value);
    }
    if (states.size() + externalSet.size() + 1 > 32) return std::nullopt;
    const std::vector<ValueId> externals(externalSet.begin(), externalSet.end());
    std::map<ValueId, std::size_t> externalIndex;
    for (std::size_t index = 0; index < externals.size(); ++index)
        externalIndex.emplace(externals[index], states.size() + index);
    const std::size_t constantIndex = states.size() + externals.size();
    Matrix transform(constantIndex + 1,
                     std::vector<std::uint32_t>(constantIndex + 1));
    for (std::size_t row = 0; row < states.size(); ++row) {
        for (const auto& [value, coefficient] : updates[row].terms) {
            if (const auto state = stateIndex.find(value); state != stateIndex.end())
                transform[row][state->second] += coefficient;
            else if (const auto external = externalIndex.find(value);
                     external != externalIndex.end())
                transform[row][external->second] += coefficient;
            else
                return std::nullopt;
        }
        transform[row][constantIndex] = updates[row].constant;
    }
    for (std::size_t index = states.size(); index <= constantIndex; ++index)
        transform[index][index] = 1;
    const Matrix final = matrixPower(std::move(transform), exact.trips);

    std::map<ValueId, LinearForm> plans;
    for (const ValueId value : liveOut) {
        const std::size_t row = stateIndex.at(value);
        LinearForm plan;
        for (std::size_t column = 0; column < states.size(); ++column)
            addTerm(plan, states[column].initial, final[row][column]);
        for (std::size_t column = 0; column < externals.size(); ++column)
            addTerm(plan, externals[column], final[row][states.size() + column]);
        plan.constant = final[row][constantIndex];
        plans.emplace(value, std::move(plan));
    }
    return plans;
}

ValueId materializeLinearForm(IRFunction& function, BlockId block,
                              const LinearForm& form) {
    const auto appendBinary = [&](IROp op, ValueId lhs, ValueId rhs) {
        IRInstruction instruction;
        instruction.id = function.instructionCount++;
        instruction.op = op;
        instruction.result = function.valueCount++;
        instruction.operands = {lhs, rhs};
        const ValueId result = *instruction.result;
        function.blocks[block].instructions.push_back(std::move(instruction));
        return result;
    };
    std::optional<ValueId> result;
    for (const auto& [value, coefficient] : form.terms) {
        ValueId term = value;
        if (coefficient != 1) {
            const ValueId scale = getOrCreateEntryConstant(
                function, std::bit_cast<std::int32_t>(coefficient));
            term = appendBinary(IROp::Mul, term, scale);
        }
        result = result ? appendBinary(IROp::Add, *result, term) : term;
    }
    if (form.constant != 0 || !result) {
        const ValueId constant = getOrCreateEntryConstant(
            function, std::bit_cast<std::int32_t>(form.constant));
        result = result ? appendBinary(IROp::Add, *result, constant) : constant;
    }
    return *result;
}

std::optional<std::int32_t> evaluateFinalIteration(
    const IRFunction& function, const Loop& loop,
    const CountedLoopSummary& exact,
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
    const auto exact = summarizeCountedLoop(function, loop);
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
    const auto canMaterialize = [&](const auto& self, ValueId value) -> bool {
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
        needed.insert(value);
        const bool valid = std::all_of(definition->operands.begin(),
            definition->operands.end(), [&](ValueId operand) {
                return self(self, operand);
            });
        visiting.erase(value);
        return valid;
    };
    const auto affinePlans = exact->trips == 0
        ? std::optional<std::map<ValueId, LinearForm>>{}
        : analyzeAffineRecurrences(function, loop, *exact, liveOut,
                                   definitionBlocks);
    std::map<ValueId, std::int32_t> evaluated;
    for (auto& [from, to] : replacements) {
        if (affinePlans && affinePlans->contains(from)) continue;
        if (canMaterialize(canMaterialize, to)) continue;
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

    if (affinePlans) {
        for (const auto& [from, plan] : *affinePlans) {
            if (from == exact->induction) continue;
            replacements[from] = materializeLinearForm(
                function, *loop.preheader, plan);
        }
    }

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
            if (tryDeleteOneLoop(function, loop, module, result)) {
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    return result;
}

} // namespace toyc

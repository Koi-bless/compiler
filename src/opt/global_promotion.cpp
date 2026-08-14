#include "toyc/opt/global_promotion.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "toyc/opt/function_effects.hpp"
#include "toyc/opt/ir_utils.hpp"
#include "toyc/opt/loop_analysis.hpp"

namespace toyc {
namespace {

struct ExitEdge {
    BlockId source{};
    BlockId target{};
    ValueId value{};
};

ValueId appendEntryLoad(IRFunction& function, BlockId preheader,
                        SymbolId global) {
    IRInstruction load;
    load.id = function.instructionCount++;
    load.op = IROp::LoadGlobal;
    load.result = function.valueCount++;
    load.global = global;
    const ValueId value = *load.result;
    // The preheader unconditionally enters the header, so appending the
    // loop-entry load at its end is always correct.
    function.blocks[preheader].instructions.push_back(load);
    return value;
}

IRInstruction makeStore(IRFunction& function, SymbolId global, ValueId value) {
    IRInstruction store;
    store.id = function.instructionCount++;
    store.op = IROp::StoreGlobal;
    store.operands.push_back(value);
    store.global = global;
    return store;
}

// Remove every in-loop load of `global`, substituting the preheader value:
// the loop never stores the global, so that is the only value it can have.
bool promoteReadOnlyGlobal(IRFunction& function, const Loop& loop,
                           SymbolId global, PassResult& result) {
    const ValueId entryValue =
        appendEntryLoad(function, *loop.preheader, global);
    bool changed = false;
    for (const BlockId blockId : loop.blocks) {
        auto& instructions = function.blocks[blockId].instructions;
        for (std::size_t index = 0; index < instructions.size();) {
            const auto& instruction = instructions[index];
            if (instruction.op == IROp::LoadGlobal &&
                instruction.global == global) {
                replaceAllUses(function, *instruction.result, entryValue);
                instructions.erase(instructions.begin() +
                                   static_cast<std::ptrdiff_t>(index));
                ++result.instructionsRemoved;
                changed = true;
                continue;
            }
            ++index;
        }
    }
    return changed;
}

// Promote one mutable global accessed inside one loop to SSA values: the loop
// body then works on a register-allocatable temporary, loaded once in the
// preheader and stored back on every exit edge.
bool promoteMutableGlobal(IRFunction& function, const Loop& loop,
                          const std::set<BlockId>& loopBlocks, SymbolId global,
                          PassResult& result) {
    // Natural loops are single-entry: only the header may have predecessors
    // outside the loop. Bail out defensively if that ever breaks.
    for (const BlockId blockId : loop.blocks) {
        if (blockId == loop.header) continue;
        for (const BlockId predecessor : function.blocks[blockId].predecessors)
            if (!loopBlocks.contains(predecessor)) return false;
    }

    const ValueId entryValue = appendEntryLoad(function, *loop.preheader, global);

    // The value of the global merges at the header (preheader vs latches) and
    // at every in-loop join, so each such block gets a phi. Inputs from
    // in-loop predecessors are appended during the traversal below.
    std::set<BlockId> phiBlocks{loop.header};
    for (const BlockId blockId : loop.blocks)
        if (function.blocks[blockId].predecessors.size() >= 2)
            phiBlocks.insert(blockId);
    std::map<BlockId, ValueId> phiValues;
    for (const BlockId blockId : phiBlocks) {
        IRInstruction phi;
        phi.id = function.instructionCount++;
        phi.op = IROp::Phi;
        phi.result = function.valueCount++;
        phiValues[blockId] = *phi.result;
        auto& instructions = function.blocks[blockId].instructions;
        instructions.insert(instructions.begin(), phi);
    }
    function.blocks[loop.header].instructions.front().phiInputs.push_back(
        {*loop.preheader, entryValue});

    // Track the current value of the global while walking the loop body.
    // Every cycle inside the loop crosses the header or an inner join, and
    // those all carry phis, so each scan processes at least one more block.
    std::map<BlockId, ValueId> blockEnd;
    std::vector<ExitEdge> exits;
    std::set<BlockId> pending(loop.blocks.begin(), loop.blocks.end());
    while (!pending.empty()) {
        bool progressed = false;
        for (auto iterator = pending.begin(); iterator != pending.end();) {
            const BlockId blockId = *iterator;
            std::optional<ValueId> current;
            if (const auto phi = phiValues.find(blockId);
                phi != phiValues.end()) {
                current = phi->second;
            } else {
                // Join-free block: the value flows from the single in-loop
                // predecessor once it has been processed.
                const auto predecessor =
                    blockEnd.find(function.blocks[blockId].predecessors.front());
                if (predecessor != blockEnd.end()) current = predecessor->second;
            }
            if (!current) {
                ++iterator;
                continue;
            }
            iterator = pending.erase(iterator);
            progressed = true;
            auto& instructions = function.blocks[blockId].instructions;
            for (std::size_t index = 0; index < instructions.size();) {
                const auto& instruction = instructions[index];
                if (instruction.op == IROp::LoadGlobal &&
                    instruction.global == global) {
                    replaceAllUses(function, *instruction.result, *current);
                    instructions.erase(instructions.begin() +
                                       static_cast<std::ptrdiff_t>(index));
                    ++result.instructionsRemoved;
                    continue;
                }
                if (instruction.op == IROp::StoreGlobal &&
                    instruction.global == global) {
                    current = instruction.operands[0];
                    instructions.erase(instructions.begin() +
                                       static_cast<std::ptrdiff_t>(index));
                    ++result.instructionsRemoved;
                    continue;
                }
                ++index;
            }
            blockEnd[blockId] = *current;
            for (const BlockId successor : function.blocks[blockId].successors) {
                if (!loopBlocks.contains(successor)) {
                    exits.push_back({blockId, successor, *current});
                    continue;
                }
                const auto phi = phiValues.find(successor);
                if (phi == phiValues.end()) continue;
                for (auto& target : function.blocks[successor].instructions) {
                    if (target.op != IROp::Phi) break;
                    if (target.result != phi->second) continue;
                    target.phiInputs.push_back({blockId, *current});
                    break;
                }
            }
        }
        if (!progressed) break;
    }

    for (const auto& [blockId, value] : phiValues) {
        for (auto& instruction : function.blocks[blockId].instructions) {
            if (instruction.op != IROp::Phi) break;
            if (instruction.result != value) continue;
            std::sort(instruction.phiInputs.begin(), instruction.phiInputs.end(),
                      [](const PhiInput& lhs, const PhiInput& rhs) {
                          return lhs.predecessor < rhs.predecessor;
                      });
            break;
        }
    }

    // Write the final value back on every exit edge. A block that ends in an
    // unconditional exit can carry the store itself; a conditional exit edge
    // gets a fresh block so the store only runs when the edge is taken (which
    // also handles exit targets shared with other predecessors).
    bool splitEdge = false;
    std::map<BlockId, std::vector<ExitEdge>> exitsBySource;
    for (const ExitEdge& exit : exits) exitsBySource[exit.source].push_back(exit);
    for (const auto& [source, edges] : exitsBySource) {
        if (function.blocks[source].successors.size() == 1) {
            function.blocks[source].instructions.push_back(
                makeStore(function, global, edges.front().value));
            continue;
        }
        for (const ExitEdge& exit : edges) {
            IRBlock block;
            block.id = static_cast<BlockId>(function.blocks.size());
            block.instructions.push_back(makeStore(function, global, exit.value));
            block.terminator = IRJump{exit.target};
            auto& terminator = *function.blocks[source].terminator;
            if (auto* branch = std::get_if<BranchValue>(&terminator)) {
                if (branch->trueTarget == exit.target)
                    branch->trueTarget = block.id;
                if (branch->falseTarget == exit.target)
                    branch->falseTarget = block.id;
            } else if (auto* jump = std::get_if<IRJump>(&terminator)) {
                jump->target = block.id;
            }
            for (auto& instruction : function.blocks[exit.target].instructions) {
                if (instruction.op != IROp::Phi) break;
                for (auto& input : instruction.phiInputs)
                    if (input.predecessor == source) input.predecessor = block.id;
            }
            function.blocks.push_back(std::move(block));
            splitEdge = true;
        }
    }
    if (splitEdge) rebuildIRControlFlow(function);
    result.changed = true;
    return true;
}

bool promoteGlobal(IRFunction& function, const Loop& loop,
                   const std::set<BlockId>& loopBlocks, SymbolId global,
                   PassResult& result) {
    bool hasLoad = false;
    bool hasStore = false;
    for (const BlockId blockId : loop.blocks)
        for (const auto& instruction : function.blocks[blockId].instructions) {
            if (instruction.op == IROp::LoadGlobal &&
                instruction.global == global)
                hasLoad = true;
            else if (instruction.op == IROp::StoreGlobal &&
                     instruction.global == global)
                hasStore = true;
        }
    if (hasStore)
        return promoteMutableGlobal(function, loop, loopBlocks, global, result);
    if (hasLoad && promoteReadOnlyGlobal(function, loop, global, result)) {
        result.changed = true;
        return true;
    }
    return false;
}

bool promoteLoop(IRFunction& function, const Loop& loop,
                 const FunctionEffectAnalysis& effects, PassResult& result) {
    const std::set<BlockId> loopBlocks(loop.blocks.begin(), loop.blocks.end());
    std::set<SymbolId> accessed;
    std::set<SymbolId> callTouched;
    bool opaqueCall = false;
    for (const BlockId blockId : loop.blocks) {
        for (const auto& instruction : function.blocks[blockId].instructions) {
            if (instruction.op == IROp::LoadGlobal ||
                instruction.op == IROp::StoreGlobal) {
                accessed.insert(*instruction.global);
            } else if (instruction.op == IROp::Call) {
                // Without pointers the only aliasing risk is a call; promote
                // a global only when no callee in the loop reads or writes it.
                const FunctionEffects* summary =
                    effects.lookup(*instruction.callee);
                if (!summary) {
                    opaqueCall = true;
                    continue;
                }
                callTouched.insert(summary->reads.begin(), summary->reads.end());
                callTouched.insert(summary->writes.begin(), summary->writes.end());
            }
        }
    }
    bool promoted = false;
    for (const SymbolId global : accessed) {
        if (opaqueCall || callTouched.contains(global)) continue;
        promoted =
            promoteGlobal(function, loop, loopBlocks, global, result) || promoted;
    }
    return promoted;
}

} // namespace

PassResult runGlobalPromotion(IRFunction& function,
                              const FunctionEffectAnalysis& effects) {
    PassResult result;
    // Promotion adds the entry load and the exit stores just outside the
    // loop; those can land inside an enclosing loop, which then becomes
    // eligible itself. Recompute the loop nest after each transformed loop so
    // enclosing loops see the fresh blocks, innermost first. A transformed
    // loop keeps no accesses to the promoted globals and new accesses never
    // land inside an already-processed loop, so this terminates.
    bool changed = true;
    while (changed) {
        changed = false;
        auto loops = analyzeLoops(function);
        std::stable_sort(loops.begin(), loops.end(),
                         [](const Loop& lhs, const Loop& rhs) {
                             return lhs.depth > rhs.depth;
                         });
        for (const auto& loop : loops) {
            if (!loop.preheader) continue;
            if (promoteLoop(function, loop, effects, result)) {
                changed = true;
                break;
            }
        }
    }
    if (result.changed) compactIR(function);
    return result;
}

} // namespace toyc

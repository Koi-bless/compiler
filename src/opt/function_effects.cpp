#include "toyc/opt/function_effects.hpp"

#include <algorithm>
#include <functional>
#include <vector>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

bool hasControlFlowCycle(const IRFunction& function) {
    enum class Color { White, Gray, Black };
    std::vector<Color> colors(function.blocks.size(), Color::White);
    std::function<bool(BlockId)> visit = [&](BlockId block) {
        colors[block] = Color::Gray;
        for (const BlockId successor : function.blocks[block].successors) {
            if (successor >= colors.size()) return true;
            if (colors[successor] == Color::Gray) return true;
            if (colors[successor] == Color::White && visit(successor)) return true;
        }
        colors[block] = Color::Black;
        return false;
    };
    return function.entry >= function.blocks.size() || visit(function.entry);
}

bool reaches(const std::vector<std::vector<FuncId>>& calls, FuncId from,
             FuncId target, std::vector<bool>& visited) {
    if (from >= calls.size() || visited[from]) return false;
    visited[from] = true;
    for (const FuncId callee : calls[from]) {
        if (callee == target) return true;
        if (reaches(calls, callee, target, visited)) return true;
    }
    return false;
}

template <class T>
bool mergeSet(std::set<T>& destination, const std::set<T>& source) {
    const auto oldSize = destination.size();
    destination.insert(source.begin(), source.end());
    return destination.size() != oldSize;
}

} // namespace

FunctionEffectAnalysis analyzeFunctionEffects(const IRModule& module) {
    std::size_t count = 0;
    for (const auto& function : module.functions)
        count = std::max(count, static_cast<std::size_t>(function.function) + 1);

    FunctionEffectAnalysis analysis;
    analysis.functions.resize(count);
    std::vector<std::vector<FuncId>> calls(count);

    for (const auto& function : module.functions) {
        auto& effects = analysis.functions[function.function];
        effects.mayNotReturn = hasControlFlowCycle(function);
        for (const auto& block : function.blocks) {
            if (std::holds_alternative<IRUnreachable>(*block.terminator))
                effects.mayNotReturn = true;
            for (const auto& instruction : block.instructions) {
                if (instruction.op == IROp::LoadGlobal && instruction.global)
                    effects.reads.insert(*instruction.global);
                else if (instruction.op == IROp::StoreGlobal && instruction.global)
                    effects.writes.insert(*instruction.global);
                else if (instruction.op == IROp::Call && instruction.callee)
                    calls[function.function].push_back(*instruction.callee);
                else if (mayTrap(instruction) &&
                         !isKnownNonTrapping(instruction, function))
                    effects.mayTrap = true;
            }
        }
        auto& callees = calls[function.function];
        std::sort(callees.begin(), callees.end());
        callees.erase(std::unique(callees.begin(), callees.end()), callees.end());
    }

    for (FuncId function = 0; function < calls.size(); ++function) {
        std::vector<bool> visited(calls.size(), false);
        analysis.functions[function].recursive =
            reaches(calls, function, function, visited);
        if (analysis.functions[function].recursive)
            analysis.functions[function].mayNotReturn = true;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (FuncId function = 0; function < calls.size(); ++function) {
            auto& effects = analysis.functions[function];
            for (const FuncId callee : calls[function]) {
                if (callee >= analysis.functions.size()) {
                    effects.mayTrap = true;
                    effects.mayNotReturn = true;
                    continue;
                }
                const auto& nested = analysis.functions[callee];
                changed = mergeSet(effects.reads, nested.reads) || changed;
                changed = mergeSet(effects.writes, nested.writes) || changed;
                if (nested.mayTrap && !effects.mayTrap) {
                    effects.mayTrap = true;
                    changed = true;
                }
                if (nested.mayNotReturn && !effects.mayNotReturn) {
                    effects.mayNotReturn = true;
                    changed = true;
                }
            }
        }
    }
    return analysis;
}

} // namespace toyc

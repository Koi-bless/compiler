#include "toyc/ir/verifier.hpp"

#include <algorithm>
#include <cstddef>
#include <queue>
#include <string>
#include <vector>

#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

[[noreturn]] void invalid(const FunctionIR& function, const std::string& message) {
    throw CompileError(function.location, "IR verification", message);
}

bool contains(const std::vector<BlockId>& values, BlockId value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void verifyFunction(const FunctionIR& function) {
    if (function.blocks.empty() || function.entry >= function.blocks.size())
        invalid(function, "function has no valid entry block");
    std::vector<bool> reachable(function.blocks.size(), false);
    std::queue<BlockId> work;
    reachable[function.entry] = true; work.push(function.entry);
    while (!work.empty()) {
        const BlockId id = work.front(); work.pop();
        const auto& block = function.blocks[id];
        if (!block.terminator) invalid(function, "reachable block bb" + std::to_string(id) + " has no terminator");
        for (const BlockId target : block.successors) {
            if (target >= function.blocks.size()) invalid(function, "branch target is outside the function");
            if (!reachable[target]) { reachable[target] = true; work.push(target); }
        }
    }

    for (const auto& block : function.blocks) {
        if (block.id >= function.blocks.size() || &block != &function.blocks[block.id])
            invalid(function, "block IDs are not dense and stable");
        std::vector<BlockId> expected;
        if (block.terminator) {
            if (const auto* jump = std::get_if<Jump>(&*block.terminator)) expected.push_back(jump->target);
            else if (const auto* branch = std::get_if<Branch>(&*block.terminator)) {
                expected.push_back(branch->trueTarget);
                if (branch->falseTarget != branch->trueTarget) expected.push_back(branch->falseTarget);
            } else if (const auto* ret = std::get_if<Return>(&*block.terminator)) {
                if (function.returnType == ValueType::Int && !ret->value) invalid(function, "int function has valueless return");
                if (function.returnType == ValueType::Void && ret->value) invalid(function, "void function has valued return");
            }
        }
        if (expected != block.successors) invalid(function, "successor list does not match terminator in bb" + std::to_string(block.id));
        for (std::size_t i = 0; i < block.successors.size(); ++i)
            for (std::size_t j = i + 1; j < block.successors.size(); ++j)
                if (block.successors[i] == block.successors[j]) invalid(function, "duplicate successor");
        for (std::size_t i = 0; i < block.predecessors.size(); ++i)
            for (std::size_t j = i + 1; j < block.predecessors.size(); ++j)
                if (block.predecessors[i] == block.predecessors[j]) invalid(function, "duplicate predecessor");
        for (const BlockId successor : block.successors) {
            if (successor >= function.blocks.size() || !contains(function.blocks[successor].predecessors, block.id))
                invalid(function, "successor/predecessor mismatch");
        }
        for (const BlockId predecessor : block.predecessors) {
            if (predecessor >= function.blocks.size() || !contains(function.blocks[predecessor].successors, block.id))
                invalid(function, "predecessor/successor mismatch");
        }
    }

    const std::size_t tempCount = function.tempCount;
    std::vector<std::vector<bool>> in(function.blocks.size(), std::vector<bool>(tempCount, true));
    std::vector<std::vector<bool>> out = in;
    std::fill(in[function.entry].begin(), in[function.entry].end(), false);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& block : function.blocks) {
            if (!reachable[block.id]) continue;
            std::vector<bool> nextIn(tempCount, false);
            if (block.id != function.entry) {
                std::fill(nextIn.begin(), nextIn.end(), true);
                bool hasReachablePred = false;
                for (const BlockId predecessor : block.predecessors) if (reachable[predecessor]) {
                    hasReachablePred = true;
                    for (std::size_t temp = 0; temp < tempCount; ++temp) nextIn[temp] = nextIn[temp] && out[predecessor][temp];
                }
                if (!hasReachablePred) std::fill(nextIn.begin(), nextIn.end(), false);
            }
            std::vector<bool> nextOut = nextIn;
            for (const auto& instruction : block.instructions) if (instruction.dst) {
                if (*instruction.dst >= tempCount) invalid(function, "temporary definition is out of range");
                nextOut[*instruction.dst] = true;
            }
            if (nextIn != in[block.id] || nextOut != out[block.id]) {
                in[block.id] = std::move(nextIn); out[block.id] = std::move(nextOut); changed = true;
            }
        }
    }

    for (const auto& block : function.blocks) {
        if (!reachable[block.id]) continue;
        auto defined = in[block.id];
        const auto requireDefined = [&](TempId temp) {
            if (temp >= tempCount || !defined[temp]) invalid(function, "temporary %" + std::to_string(temp) + " is read before definition in bb" + std::to_string(block.id));
        };
        for (const auto& instruction : block.instructions) {
            for (const TempId input : instruction.inputs) requireDefined(input);
            if (instruction.dst) defined[*instruction.dst] = true;
        }
        if (const auto* branch = std::get_if<Branch>(&*block.terminator)) requireDefined(branch->condition);
        else if (const auto* ret = std::get_if<Return>(&*block.terminator); ret && ret->value) requireDefined(*ret->value);
    }
}

} // namespace

void verify(const ModuleIR& module, const SemanticResult& semantic) {
    if (module.functions.size() != semantic.functions.size())
        throw CompileError({}, "IR verification", "function table and IR function count differ");
    for (const auto& function : module.functions) {
        if (function.function >= semantic.functions.size())
            throw CompileError(function.location, "IR verification", "function ID is out of range");
        verifyFunction(function);
    }
}

} // namespace toyc

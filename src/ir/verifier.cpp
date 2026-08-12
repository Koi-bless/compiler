#include "toyc/ir/verifier.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

[[noreturn]] void invalidCFG(const CFGFunction& function, const std::string& message) {
    throw CompileError(function.location, "CFG verification", message);
}
[[noreturn]] void invalidIR(const IRFunction& function, const std::string& message) {
    throw CompileError(function.location, "SSA verification", message);
}
bool contains(const std::vector<BlockId>& values, BlockId value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<BlockId> cfgTargets(const CFGBlock& block) {
    std::vector<BlockId> result;
    if (!block.terminator) return result;
    if (const auto* jump = std::get_if<Jump>(&*block.terminator)) result.push_back(jump->target);
    else if (const auto* branch = std::get_if<Branch>(&*block.terminator)) {
        result.push_back(branch->trueTarget);
        if (branch->falseTarget != branch->trueTarget) result.push_back(branch->falseTarget);
    }
    std::sort(result.begin(), result.end());
    return result;
}

void verifyCFGInstruction(const CFGFunction& function, const TacInst& instruction,
                          const SemanticResult& semantic, bool entry,
                          std::vector<bool>& parameters) {
    const auto operands = instruction.inputs.size();
    const bool binary = instruction.op >= TacOp::Add && instruction.op <= TacOp::CmpNE;
    const bool produces = instruction.op != TacOp::StoreGlobal &&
        !(instruction.op == TacOp::Call && instruction.callee &&
          semantic.functions[*instruction.callee].returnType == ValueType::Void);
    if (produces != instruction.dst.has_value()) invalidCFG(function, "opcode has invalid destination");
    if (binary && operands != 2) invalidCFG(function, "binary opcode has invalid operand count");
    if ((instruction.op == TacOp::Copy || instruction.op == TacOp::LogicalNot ||
         instruction.op == TacOp::StoreGlobal) && operands != 1)
        invalidCFG(function, "unary opcode has invalid operand count");
    if ((instruction.op == TacOp::Param || instruction.op == TacOp::LoadImm ||
         instruction.op == TacOp::LoadGlobal) && operands != 0)
        invalidCFG(function, "source opcode has operands");
    if (instruction.op == TacOp::Param) {
        if (!entry || !instruction.immediate || *instruction.immediate < 0)
            invalidCFG(function, "parameter definition is outside entry or lacks an index");
        const auto index = static_cast<std::size_t>(*instruction.immediate);
        if (index >= parameters.size() || parameters[index]) invalidCFG(function, "parameter indices are not unique and continuous");
        parameters[index] = true;
    }
    if (instruction.op == TacOp::LoadImm && !instruction.immediate) invalidCFG(function, "constant lacks immediate");
    if (instruction.op == TacOp::LoadGlobal || instruction.op == TacOp::StoreGlobal) {
        if (!instruction.symbol || *instruction.symbol >= semantic.symbols.size()) invalidCFG(function, "global operation lacks symbol");
        const auto& symbol = semantic.symbols[*instruction.symbol];
        if (!symbol.isGlobal || symbol.isConst) invalidCFG(function, "global operation references a non-variable symbol");
    }
    if (instruction.op == TacOp::Call) {
        if (!instruction.callee || *instruction.callee >= semantic.functions.size()) invalidCFG(function, "call lacks callee");
        if (operands != semantic.functions[*instruction.callee].parameterSymbols.size()) invalidCFG(function, "call argument count mismatch");
    }
}

void verifyCFGFunction(const CFGFunction& function, const SemanticResult& semantic) {
    if (function.blocks.empty() || function.entry >= function.blocks.size()) invalidCFG(function, "function has no valid entry block");
    std::vector<bool> reachable(function.blocks.size());
    std::queue<BlockId> work; reachable[function.entry] = true; work.push(function.entry);
    while (!work.empty()) {
        const BlockId id = work.front(); work.pop();
        const auto& block = function.blocks[id];
        if (!block.terminator) invalidCFG(function, "reachable block has no terminator");
        for (const BlockId target : block.successors) {
            if (target >= function.blocks.size()) invalidCFG(function, "branch target is outside the function");
            if (!reachable[target]) { reachable[target] = true; work.push(target); }
        }
    }
    std::vector<bool> parameters(semantic.functions[function.function].parameterSymbols.size());
    for (const auto& block : function.blocks) {
        if (block.id >= function.blocks.size() || &block != &function.blocks[block.id]) invalidCFG(function, "block IDs are not dense");
        auto expected = cfgTargets(block);
        auto actual = block.successors; std::sort(actual.begin(), actual.end());
        if (expected != actual) invalidCFG(function, "successor list does not match terminator");
        if (std::adjacent_find(actual.begin(), actual.end()) != actual.end()) invalidCFG(function, "duplicate successor");
        auto predecessors = block.predecessors; std::sort(predecessors.begin(), predecessors.end());
        if (std::adjacent_find(predecessors.begin(), predecessors.end()) != predecessors.end()) invalidCFG(function, "duplicate predecessor");
        for (const BlockId successor : block.successors)
            if (!contains(function.blocks[successor].predecessors, block.id)) invalidCFG(function, "successor/predecessor mismatch");
        for (const BlockId predecessor : block.predecessors)
            if (predecessor >= function.blocks.size() || !contains(function.blocks[predecessor].successors, block.id)) invalidCFG(function, "predecessor/successor mismatch");
        if (block.terminator) if (const auto* ret = std::get_if<Return>(&*block.terminator)) {
            if ((function.returnType == ValueType::Int) != ret->value.has_value()) invalidCFG(function, "return type mismatch");
        }
        for (const auto& instruction : block.instructions)
            verifyCFGInstruction(function, instruction, semantic, block.id == function.entry, parameters);
    }
    if (!std::all_of(parameters.begin(), parameters.end(), [](bool value) { return value; })) invalidCFG(function, "parameter indices are not continuous");

    const std::size_t count = function.tempCount;
    std::vector<std::vector<bool>> in(function.blocks.size(), std::vector<bool>(count, true)), out = in;
    std::fill(in[function.entry].begin(), in[function.entry].end(), false);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& block : function.blocks) if (reachable[block.id]) {
            std::vector<bool> nextIn(count, block.id != function.entry);
            if (block.id != function.entry) for (const BlockId predecessor : block.predecessors)
                for (std::size_t temp = 0; temp < count; ++temp) nextIn[temp] = nextIn[temp] && out[predecessor][temp];
            std::vector<bool> nextOut = nextIn;
            for (const auto& instruction : block.instructions) if (instruction.dst) {
                if (*instruction.dst >= count) invalidCFG(function, "temporary definition is out of range");
                nextOut[*instruction.dst] = true;
            }
            if (nextIn != in[block.id] || nextOut != out[block.id]) { in[block.id] = std::move(nextIn); out[block.id] = std::move(nextOut); changed = true; }
        }
    }
    for (const auto& block : function.blocks) if (reachable[block.id]) {
        auto defined = in[block.id];
        const auto require = [&](TempId temp) {
            if (temp >= count || !defined[temp]) invalidCFG(function, "temporary is read before definition");
        };
        for (const auto& instruction : block.instructions) {
            for (const TempId input : instruction.inputs) require(input);
            if (instruction.dst) defined[*instruction.dst] = true;
        }
        if (const auto* branch = std::get_if<Branch>(&*block.terminator)) require(branch->condition);
        else if (const auto* ret = std::get_if<Return>(&*block.terminator); ret && ret->value) require(*ret->value);
    }
}

std::vector<BlockId> irTargets(const IRBlock& block) {
    std::vector<BlockId> result;
    if (!block.terminator) return result;
    if (const auto* jump = std::get_if<IRJump>(&*block.terminator)) result.push_back(jump->target);
    else if (const auto* branch = std::get_if<BranchValue>(&*block.terminator)) {
        result.push_back(branch->trueTarget); if (branch->falseTarget != branch->trueTarget) result.push_back(branch->falseTarget);
    }
    std::sort(result.begin(), result.end()); return result;
}

struct DefInfo { BlockId block{}; std::size_t index{}; bool phi{}; };

void verifyIRFunction(const IRFunction& function, const SemanticResult& semantic) {
    if (function.blocks.empty() || function.entry >= function.blocks.size()) invalidIR(function, "function has no valid entry block");
    if (!function.blocks[function.entry].predecessors.empty())
        invalidIR(function, "entry block has predecessors");
    std::vector<bool> reachable(function.blocks.size(), false);
    std::queue<BlockId> reachableWork;
    reachable[function.entry] = true;
    reachableWork.push(function.entry);
    while (!reachableWork.empty()) {
        const BlockId block = reachableWork.front();
        reachableWork.pop();
        if (!function.blocks[block].terminator)
            invalidIR(function, "reachable block has no terminator");
        for (const BlockId target : irTargets(function.blocks[block])) {
            if (target >= function.blocks.size())
                invalidIR(function, "branch target is outside the function");
            if (!reachable[target]) { reachable[target] = true; reachableWork.push(target); }
        }
    }
    if (!std::all_of(reachable.begin(), reachable.end(), [](bool value) { return value; }))
        invalidIR(function, "unreachable block remains in IR");
    for (const auto& block : function.blocks) {
        for (const BlockId predecessor : block.predecessors)
            if (predecessor >= function.blocks.size())
                invalidIR(function, "predecessor is outside the function");
    }
    std::vector<std::set<BlockId>> dom(function.blocks.size());
    std::set<BlockId> all; for (const auto& block : function.blocks) all.insert(block.id);
    for (const auto& block : function.blocks) dom[block.id] = block.id == function.entry ? std::set<BlockId>{block.id} : all;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& block : function.blocks) if (block.id != function.entry) {
            std::set<BlockId> next = all;
            for (const BlockId predecessor : block.predecessors) {
                std::set<BlockId> intersection;
                std::set_intersection(next.begin(), next.end(), dom[predecessor].begin(), dom[predecessor].end(),
                                      std::inserter(intersection, intersection.end()));
                next = std::move(intersection);
            }
            next.insert(block.id);
            if (next != dom[block.id]) { dom[block.id] = std::move(next); changed = true; }
        }
    }
    std::vector<std::optional<DefInfo>> definitions(function.valueCount);
    std::vector<bool> parameters(semantic.functions[function.function].parameterSymbols.size(), false);
    InstId expectedId = 0;
    for (const auto& block : function.blocks) {
        if (block.id >= function.blocks.size() || &block != &function.blocks[block.id]) invalidIR(function, "block IDs are not dense");
        if (!block.terminator) invalidIR(function, "reachable block has no terminator");
        auto expected = irTargets(block), actual = block.successors; std::sort(actual.begin(), actual.end());
        if (expected != actual) invalidIR(function, "successor list does not match terminator");
        if (std::adjacent_find(actual.begin(), actual.end()) != actual.end()) invalidIR(function, "duplicate edge");
        auto predecessors = block.predecessors;
        std::sort(predecessors.begin(), predecessors.end());
        if (std::adjacent_find(predecessors.begin(), predecessors.end()) != predecessors.end())
            invalidIR(function, "duplicate predecessor");
        for (const BlockId successor : actual) {
            if (successor >= function.blocks.size() || !contains(function.blocks[successor].predecessors, block.id)) invalidIR(function, "asymmetric CFG edge");
        }
        for (const BlockId predecessor : block.predecessors)
            if (!contains(function.blocks[predecessor].successors, block.id))
                invalidIR(function, "asymmetric predecessor edge");
        bool ordinarySeen = false;
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            if (instruction.id != expectedId++) invalidIR(function, "instruction IDs are not dense");
            if (instruction.op == IROp::Phi) { if (ordinarySeen) invalidIR(function, "phi appears after ordinary instruction"); }
            else ordinarySeen = true;
            if (instruction.result) {
                if (*instruction.result >= definitions.size()) invalidIR(function, "definition value is out of range");
                if (definitions[*instruction.result]) invalidIR(function, "value has multiple definitions");
                definitions[*instruction.result] = DefInfo{block.id, index, instruction.op == IROp::Phi};
            }
            const bool binary = instruction.op >= IROp::Add && instruction.op <= IROp::ICmpNE;
            if (instruction.op == IROp::Call &&
                (!instruction.callee || *instruction.callee >= semantic.functions.size()))
                invalidIR(function, "invalid callee");
            const bool resultRequired = instruction.op != IROp::StoreGlobal &&
                !(instruction.op == IROp::Call &&
                  semantic.functions[*instruction.callee].returnType == ValueType::Void);
            if (resultRequired != instruction.result.has_value()) invalidIR(function, "opcode has invalid result");
            if (binary && instruction.operands.size() != 2) invalidIR(function, "binary opcode operand count mismatch");
            if ((instruction.op == IROp::Copy || instruction.op == IROp::LogicalNot || instruction.op == IROp::StoreGlobal) && instruction.operands.size() != 1) invalidIR(function, "unary opcode operand count mismatch");
            if ((instruction.op == IROp::Param || instruction.op == IROp::Constant ||
                 instruction.op == IROp::LoadGlobal) && !instruction.operands.empty())
                invalidIR(function, "source opcode has operands");
            if ((instruction.op == IROp::Param || instruction.op == IROp::Constant) !=
                instruction.immediate.has_value())
                invalidIR(function, "opcode has invalid immediate field");
            if ((instruction.op == IROp::LoadGlobal || instruction.op == IROp::StoreGlobal) !=
                instruction.global.has_value())
                invalidIR(function, "opcode has invalid global field");
            if ((instruction.op == IROp::Call) != instruction.callee.has_value())
                invalidIR(function, "opcode has invalid callee field");
            if (instruction.op != IROp::Phi && !instruction.phiInputs.empty())
                invalidIR(function, "non-phi opcode has phi inputs");
            if (instruction.op == IROp::Param) {
                if (block.id != function.entry || *instruction.immediate < 0)
                    invalidIR(function, "parameter definition is outside entry or has an invalid index");
                const auto parameter = static_cast<std::size_t>(*instruction.immediate);
                if (parameter >= parameters.size() || parameters[parameter])
                    invalidIR(function, "parameter index is out of range or duplicated");
                parameters[parameter] = true;
            }
            if (instruction.op == IROp::Phi) {
                if (block.id == function.entry) invalidIR(function, "entry block contains phi");
                if (!instruction.operands.empty() || instruction.phiInputs.size() != block.predecessors.size()) invalidIR(function, "phi input count mismatch");
                for (std::size_t input = 0; input < instruction.phiInputs.size(); ++input) {
                    if (instruction.phiInputs[input].predecessor != block.predecessors[input]) invalidIR(function, "phi predecessor mismatch or unstable order");
                }
            }
            if ((instruction.op == IROp::LoadGlobal || instruction.op == IROp::StoreGlobal)) {
                if (!instruction.global || *instruction.global >= semantic.symbols.size() ||
                    !semantic.symbols[*instruction.global].isGlobal || semantic.symbols[*instruction.global].isConst)
                    invalidIR(function, "invalid global operation");
            }
            if (instruction.op == IROp::Call) {
                if (instruction.operands.size() != semantic.functions[*instruction.callee].parameterSymbols.size()) invalidIR(function, "call argument count mismatch");
            }
        }
        if (const auto* ret = std::get_if<ReturnValue>(&*block.terminator))
            if ((function.returnType == ValueType::Int) != ret->value.has_value()) invalidIR(function, "return type mismatch");
    }
    if (expectedId != function.instructionCount) invalidIR(function, "instruction count mismatch");
    for (const auto& definition : definitions) if (!definition) invalidIR(function, "value lacks a definition");
    const auto checkUse = [&](ValueId value, BlockId useBlock, std::size_t useIndex, bool edge) {
        if (value >= definitions.size() || !definitions[value]) invalidIR(function, "use references undefined value");
        const auto& definition = *definitions[value];
        if (!dom[useBlock].contains(definition.block)) invalidIR(function, "definition does not dominate use");
        if (!edge && definition.block == useBlock && !definition.phi && definition.index >= useIndex) invalidIR(function, "same-block use precedes definition");
    };
    for (const auto& block : function.blocks) {
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            for (const ValueId operand : instruction.operands) checkUse(operand, block.id, index, false);
            for (const auto& input : instruction.phiInputs) checkUse(input.value, input.predecessor, function.blocks[input.predecessor].instructions.size(), true);
        }
        if (const auto* branch = std::get_if<BranchValue>(&*block.terminator)) checkUse(branch->condition, block.id, block.instructions.size(), false);
        else if (const auto* ret = std::get_if<ReturnValue>(&*block.terminator); ret && ret->value) checkUse(*ret->value, block.id, block.instructions.size(), false);
    }
}

} // namespace

void verifyCFG(const CFGModule& module, const SemanticResult& semantic) {
    if (module.functions.size() != semantic.functions.size()) throw CompileError({}, "CFG verification", "function table count mismatch");
    for (const auto& function : module.functions) {
        if (function.function >= semantic.functions.size()) invalidCFG(function, "function ID is out of range");
        verifyCFGFunction(function, semantic);
    }
}

void verifyIR(const IRModule& module, const SemanticResult& semantic) {
    if (module.functions.size() != semantic.functions.size()) throw CompileError({}, "SSA verification", "function table count mismatch");
    for (const auto& function : module.functions) {
        if (function.function >= semantic.functions.size()) invalidIR(function, "function ID is out of range");
        verifyIRFunction(function, semantic);
    }
}

} // namespace toyc

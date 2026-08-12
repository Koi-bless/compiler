#include "toyc/ir/ssa_builder.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <set>
#include <utility>

#include "toyc/ir/cfg_utils.hpp"
#include "toyc/ir/dominator.hpp"
#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

IROp convertOp(TacOp op) {
    switch (op) {
    case TacOp::Param: return IROp::Param;
    case TacOp::LoadImm: return IROp::Constant;
    case TacOp::Copy: return IROp::Copy;
    case TacOp::Add: return IROp::Add;
    case TacOp::Sub: return IROp::Sub;
    case TacOp::Mul: return IROp::Mul;
    case TacOp::Div: return IROp::SDiv;
    case TacOp::Rem: return IROp::SRem;
    case TacOp::CmpLT: return IROp::ICmpLT;
    case TacOp::CmpGT: return IROp::ICmpGT;
    case TacOp::CmpLE: return IROp::ICmpLE;
    case TacOp::CmpGE: return IROp::ICmpGE;
    case TacOp::CmpEQ: return IROp::ICmpEQ;
    case TacOp::CmpNE: return IROp::ICmpNE;
    case TacOp::LogicalNot: return IROp::LogicalNot;
    case TacOp::LoadGlobal: return IROp::LoadGlobal;
    case TacOp::StoreGlobal: return IROp::StoreGlobal;
    case TacOp::Call: return IROp::Call;
    }
    throw CompileError({}, "SSA construction", "unknown CFG opcode");
}

void eliminateTrivialPhis(IRFunction& function) {
    std::vector<ValueId> replacement(function.valueCount);
    for (ValueId value = 0; value < function.valueCount; ++value) replacement[value] = value;
    const auto resolve = [&](ValueId value) {
        std::size_t guard = 0;
        while (replacement[value] != value && guard++ <= replacement.size()) value = replacement[value];
        return value;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& block : function.blocks) {
            auto iterator = block.instructions.begin();
            while (iterator != block.instructions.end() && iterator->op == IROp::Phi) {
                const ValueId result = *iterator->result;
                std::optional<ValueId> candidate;
                bool nonTrivial = false;
                for (const auto& input : iterator->phiInputs) {
                    const ValueId value = resolve(input.value);
                    if (value == result) continue;
                    if (!candidate) candidate = value;
                    else if (*candidate != value) { nonTrivial = true; break; }
                }
                if (candidate && !nonTrivial) {
                    replacement[result] = *candidate;
                    iterator = block.instructions.erase(iterator);
                    changed = true;
                } else ++iterator;
            }
        }
    }
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            for (auto& operand : instruction.operands) operand = resolve(operand);
            for (auto& input : instruction.phiInputs) input.value = resolve(input.value);
        }
        if (auto* branch = std::get_if<BranchValue>(&*block.terminator)) branch->condition = resolve(branch->condition);
        else if (auto* ret = std::get_if<ReturnValue>(&*block.terminator); ret && ret->value) ret->value = resolve(*ret->value);
    }
    std::vector<ValueId> compact(function.valueCount, function.valueCount);
    ValueId next = 0;
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
        if (instruction.result) compact[*instruction.result] = next++;
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            if (instruction.result) instruction.result = compact[*instruction.result];
            for (auto& operand : instruction.operands) operand = compact[operand];
            for (auto& input : instruction.phiInputs) input.value = compact[input.value];
        }
        if (auto* branch = std::get_if<BranchValue>(&*block.terminator)) branch->condition = compact[branch->condition];
        else if (auto* ret = std::get_if<ReturnValue>(&*block.terminator); ret && ret->value) ret->value = compact[*ret->value];
    }
    function.valueCount = next;
}

IRFunction buildFunction(const CFGFunction& cfg) {
    DominatorInfo dominators(cfg);
    const std::size_t blockCount = cfg.blocks.size();
    const std::size_t tempCount = cfg.tempCount;

    std::vector<std::vector<bool>> use(blockCount, std::vector<bool>(tempCount));
    std::vector<std::vector<bool>> def(blockCount, std::vector<bool>(tempCount));
    std::vector<std::vector<BlockId>> definitionBlocks(tempCount);
    for (const auto& block : cfg.blocks) {
        for (const auto& instruction : block.instructions) {
            for (const TempId input : instruction.inputs)
                if (!def[block.id][input]) use[block.id][input] = true;
            if (instruction.dst) {
                if (!def[block.id][*instruction.dst]) definitionBlocks[*instruction.dst].push_back(block.id);
                def[block.id][*instruction.dst] = true;
            }
        }
        if (const auto* branch = std::get_if<Branch>(&*block.terminator))
            if (!def[block.id][branch->condition]) use[block.id][branch->condition] = true;
        if (const auto* ret = std::get_if<Return>(&*block.terminator); ret && ret->value)
            if (!def[block.id][*ret->value]) use[block.id][*ret->value] = true;
    }
    std::vector<std::vector<bool>> liveIn(blockCount, std::vector<bool>(tempCount));
    std::vector<std::vector<bool>> liveOut = liveIn;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto iterator = dominators.reversePostOrder().rbegin();
             iterator != dominators.reversePostOrder().rend(); ++iterator) {
            const BlockId block = *iterator;
            std::vector<bool> nextOut(tempCount);
            for (const BlockId successor : cfg.blocks[block].successors)
                for (std::size_t temp = 0; temp < tempCount; ++temp)
                    nextOut[temp] = nextOut[temp] || liveIn[successor][temp];
            std::vector<bool> nextIn = use[block];
            for (std::size_t temp = 0; temp < tempCount; ++temp)
                nextIn[temp] = nextIn[temp] || (nextOut[temp] && !def[block][temp]);
            if (nextIn != liveIn[block] || nextOut != liveOut[block]) {
                liveIn[block] = std::move(nextIn); liveOut[block] = std::move(nextOut); changed = true;
            }
        }
    }

    std::vector<std::vector<TempId>> phiTemps(blockCount);
    for (TempId temp = 0; temp < tempCount; ++temp) {
        std::queue<BlockId> work;
        std::vector<bool> queued(blockCount), placed(blockCount);
        for (const BlockId block : definitionBlocks[temp]) { work.push(block); queued[block] = true; }
        while (!work.empty()) {
            const BlockId block = work.front(); work.pop();
            for (const BlockId frontier : dominators.frontier(block)) {
                if (!liveIn[frontier][temp] || placed[frontier]) continue;
                placed[frontier] = true;
                phiTemps[frontier].push_back(temp);
                if (!queued[frontier]) { queued[frontier] = true; work.push(frontier); }
            }
        }
    }
    for (auto& temps : phiTemps) std::sort(temps.begin(), temps.end());

    IRFunction result;
    result.function = cfg.function;
    result.returnType = cfg.returnType;
    result.entry = cfg.entry;
    result.location = cfg.location;
    result.blocks.resize(blockCount);
    for (const auto& block : cfg.blocks) {
        auto& output = result.blocks[block.id];
        output.id = block.id;
        output.predecessors = block.predecessors;
        std::sort(output.predecessors.begin(), output.predecessors.end());
        output.successors = block.successors;
        for (const TempId temp : phiTemps[block.id]) {
            (void)temp;
            IRInstruction phi;
            phi.op = IROp::Phi;
            phi.location = cfg.location;
            output.instructions.push_back(std::move(phi));
        }
    }

    std::vector<std::vector<ValueId>> stacks(tempCount);
    std::function<void(BlockId)> rename = [&](BlockId blockId) {
        const auto& inputBlock = cfg.blocks[blockId];
        auto& outputBlock = result.blocks[blockId];
        std::vector<TempId> pushed;
        for (std::size_t index = 0; index < phiTemps[blockId].size(); ++index) {
            const TempId temp = phiTemps[blockId][index];
            const ValueId value = result.valueCount++;
            outputBlock.instructions[index].result = value;
            stacks[temp].push_back(value); pushed.push_back(temp);
        }
        for (const auto& instruction : inputBlock.instructions) {
            IRInstruction converted;
            converted.op = convertOp(instruction.op);
            converted.immediate = instruction.immediate;
            converted.global = instruction.symbol;
            converted.callee = instruction.callee;
            converted.location = instruction.location;
            for (const TempId input : instruction.inputs) {
                if (input >= stacks.size() || stacks[input].empty())
                    throw CompileError(instruction.location, "SSA construction", "temporary is used without a reaching definition");
                converted.operands.push_back(stacks[input].back());
            }
            if (instruction.dst) {
                const ValueId value = result.valueCount++;
                converted.result = value;
                stacks[*instruction.dst].push_back(value); pushed.push_back(*instruction.dst);
            }
            outputBlock.instructions.push_back(std::move(converted));
        }
        if (const auto* jump = std::get_if<Jump>(&*inputBlock.terminator))
            outputBlock.terminator = IRJump{jump->target};
        else if (const auto* branch = std::get_if<Branch>(&*inputBlock.terminator)) {
            if (stacks[branch->condition].empty())
                throw CompileError(cfg.location, "SSA construction", "branch condition has no reaching definition");
            outputBlock.terminator = BranchValue{stacks[branch->condition].back(), branch->trueTarget, branch->falseTarget};
        } else if (const auto* ret = std::get_if<Return>(&*inputBlock.terminator)) {
            std::optional<ValueId> value;
            if (ret->value) {
                if (stacks[*ret->value].empty())
                    throw CompileError(cfg.location, "SSA construction", "return value has no reaching definition");
                value = stacks[*ret->value].back();
            }
            outputBlock.terminator = ReturnValue{value};
        } else outputBlock.terminator = IRUnreachable{};

        for (const BlockId successor : inputBlock.successors) {
            auto& successorBlock = result.blocks[successor];
            for (std::size_t index = 0; index < phiTemps[successor].size(); ++index) {
                const TempId temp = phiTemps[successor][index];
                if (stacks[temp].empty())
                    throw CompileError(cfg.location, "SSA construction", "phi input has no reaching definition");
                successorBlock.instructions[index].phiInputs.push_back({blockId, stacks[temp].back()});
            }
        }
        for (const BlockId child : dominators.children(blockId)) rename(child);
        for (auto iterator = pushed.rbegin(); iterator != pushed.rend(); ++iterator) stacks[*iterator].pop_back();
    };
    rename(cfg.entry);

    eliminateTrivialPhis(result);
    InstId instructionId = 0;
    for (auto& block : result.blocks) for (auto& instruction : block.instructions) {
        instruction.id = instructionId++;
        if (instruction.op == IROp::Phi)
            std::sort(instruction.phiInputs.begin(), instruction.phiInputs.end(),
                      [](const PhiInput& a, const PhiInput& b) { return a.predecessor < b.predecessor; });
    }
    result.instructionCount = instructionId;
    return result;
}

} // namespace

IRModule SSABuilder::build(const CFGModule& input) const {
    (void)semantic_;
    CFGModule cfg = input;
    removeUnreachable(cfg);
    IRModule module;
    module.functions.reserve(cfg.functions.size());
    for (const auto& function : cfg.functions) module.functions.push_back(buildFunction(function));
    return module;
}

} // namespace toyc

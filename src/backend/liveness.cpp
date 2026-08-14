#include "toyc/backend/liveness.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <map>

namespace toyc {
namespace {

void addVRegs(const std::vector<MOperand>& operands, std::set<VRegId>& values) {
    for (const auto& operand : operands)
        if (const auto* reg = std::get_if<VirtualReg>(&operand)) values.insert(reg->id);
}

std::set<VRegId> setDifference(const std::set<VRegId>& left, const std::set<VRegId>& right) {
    std::set<VRegId> result;
    std::set_difference(left.begin(), left.end(), right.begin(), right.end(),
                        std::inserter(result, result.end()));
    return result;
}

std::set<VRegId> setUnion(const std::set<VRegId>& left, const std::set<VRegId>& right) {
    std::set<VRegId> result;
    std::set_union(left.begin(), left.end(), right.begin(), right.end(),
                   std::inserter(result, result.end()));
    return result;
}

std::vector<MBlockId> reversePostOrder(const MachineFunction& function) {
    std::vector<MBlockId> postorder;
    std::vector<bool> visited(function.blocks.size());
    std::function<void(MBlockId)> visit = [&](MBlockId block) {
        visited[block] = true;
        for (const MBlockId successor : function.blocks[block].successors)
            if (!visited[successor]) visit(successor);
        postorder.push_back(block);
    };
    visit(function.entry);
    std::reverse(postorder.begin(), postorder.end());
    return postorder;
}

std::vector<unsigned> loopDepths(const MachineFunction& function) {
    const std::size_t count = function.blocks.size();
    std::vector<std::set<MBlockId>> dominators(count);
    std::set<MBlockId> all;
    for (const auto& block : function.blocks) all.insert(block.id);
    for (const auto& block : function.blocks)
        dominators[block.id] = block.id == function.entry
            ? std::set<MBlockId>{block.id} : all;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& block : function.blocks) {
            if (block.id == function.entry || block.predecessors.empty()) continue;
            std::set<MBlockId> next = all;
            for (const MBlockId predecessor : block.predecessors) {
                std::set<MBlockId> intersection;
                std::set_intersection(next.begin(), next.end(),
                    dominators[predecessor].begin(), dominators[predecessor].end(),
                    std::inserter(intersection, intersection.end()));
                next = std::move(intersection);
            }
            next.insert(block.id);
            if (next != dominators[block.id]) {
                dominators[block.id] = std::move(next);
                changed = true;
            }
        }
    }
    std::vector<unsigned> depths(count);
    for (const auto& tail : function.blocks) {
        for (const MBlockId header : tail.successors) {
            if (!dominators[tail.id].contains(header)) continue;
            std::set<MBlockId> members{header, tail.id};
            std::vector<MBlockId> work{tail.id};
            while (!work.empty()) {
                const MBlockId block = work.back();
                work.pop_back();
                if (block == header) continue;
                for (const MBlockId predecessor : function.blocks[block].predecessors)
                    if (members.insert(predecessor).second) work.push_back(predecessor);
            }
            for (const MBlockId member : members) ++depths[member];
        }
    }
    return depths;
}

} // namespace

LivenessResult computeLiveness(const MachineFunction& function) {
    LivenessResult result;
    result.blocks.resize(function.blocks.size());
    for (const auto& block : function.blocks) {
        auto& info = result.blocks[block.id];
        for (const auto& instruction : block.instructions) {
            std::set<VRegId> uses, defs;
            addVRegs(instruction.uses, uses); addVRegs(instruction.defs, defs);
            for (const VRegId value : uses) if (!info.def.contains(value)) info.use.insert(value);
            info.def.insert(defs.begin(), defs.end());
        }
    }
    const auto rpo = reversePostOrder(function);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto iterator = rpo.rbegin(); iterator != rpo.rend(); ++iterator) {
            const MBlockId block = *iterator;
            std::set<VRegId> out;
            for (const MBlockId successor : function.blocks[block].successors)
                out = setUnion(out, result.blocks[successor].liveIn);
            const auto in = setUnion(result.blocks[block].use, setDifference(out, result.blocks[block].def));
            if (in != result.blocks[block].liveIn || out != result.blocks[block].liveOut) {
                result.blocks[block].liveIn = in; result.blocks[block].liveOut = out; changed = true;
            }
        }
    }

    const std::uint32_t unset = std::numeric_limits<std::uint32_t>::max();
    result.intervals.resize(function.vregCount);
    for (VRegId id = 0; id < function.vregCount; ++id) {
        result.intervals[id].vreg = id; result.intervals[id].start = unset;
    }
    std::vector<std::uint32_t> blockStart(function.blocks.size()), blockEnd(function.blocks.size());
    const auto depths = loopDepths(function);
    std::vector<double> weightedUses(function.vregCount);
    std::uint32_t position = 0;
    for (const MBlockId id : rpo) {
        blockStart[id] = position;
        for (const auto& instruction : function.blocks[id].instructions) {
            for (const auto& operand : instruction.uses) if (const auto* reg = std::get_if<VirtualReg>(&operand)) {
                auto& interval = result.intervals[reg->id];
                interval.start = std::min(interval.start, position);
                interval.end = std::max(interval.end, position);
                interval.uses.push_back(position);
                double weight = 1.0;
                for (unsigned depth = 0; depth < depths[id]; ++depth)
                    weight = std::min(weight * 10.0, 1000000.0);
                weightedUses[reg->id] += weight;
            }
            for (const auto& operand : instruction.defs) if (const auto* reg = std::get_if<VirtualReg>(&operand)) {
                auto& interval = result.intervals[reg->id];
                interval.start = std::min(interval.start, position);
                interval.end = std::max(interval.end, position);
                interval.defs.push_back(position);
            }
            position += 2;
        }
        blockEnd[id] = position == blockStart[id] ? position : position - 2;
    }
    for (const auto& block : function.blocks) {
        for (const VRegId id : result.blocks[block.id].liveIn)
            result.intervals[id].start = std::min(result.intervals[id].start, blockStart[block.id]);
        for (const VRegId id : result.blocks[block.id].liveOut)
            result.intervals[id].end = std::max(result.intervals[id].end, blockEnd[block.id]);
        std::set<VRegId> live = result.blocks[block.id].liveOut;
        for (auto iterator = block.instructions.rbegin(); iterator != block.instructions.rend(); ++iterator) {
            if (iterator->opcode == MOpcode::CALL)
                for (const VRegId id : live) result.intervals[id].crossesCall = true;
            std::set<VRegId> defs, uses;
            addVRegs(iterator->defs, defs); addVRegs(iterator->uses, uses);
            for (const VRegId id : defs) live.erase(id);
            live.insert(uses.begin(), uses.end());
        }
    }
    // An interval that is live out of the block containing its end position is
    // still live after that position even when no later use is visible there.
    for (const auto& block : function.blocks)
        for (const VRegId id : result.blocks[block.id].liveOut)
            if (result.intervals[id].end == blockEnd[block.id])
                result.intervals[id].liveOutAtEnd = true;
    result.intervals.erase(std::remove_if(result.intervals.begin(), result.intervals.end(),
        [&](const LiveInterval& interval) { return interval.start == unset; }), result.intervals.end());
    for (auto& interval : result.intervals)
        interval.spillWeight = std::max(1.0, weightedUses[interval.vreg]);
    std::sort(result.intervals.begin(), result.intervals.end(), [](const LiveInterval& a, const LiveInterval& b) {
        return a.start != b.start ? a.start < b.start : a.vreg < b.vreg;
    });
    return result;
}

} // namespace toyc

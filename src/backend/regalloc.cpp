#include "toyc/backend/regalloc.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

const std::vector<PhysReg> callerPool{PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3, PhysReg::T4,
    PhysReg::S1, PhysReg::S2, PhysReg::S3, PhysReg::S4, PhysReg::S5, PhysReg::S6,
    PhysReg::S7, PhysReg::S8, PhysReg::S9, PhysReg::S10, PhysReg::S11};
const std::vector<PhysReg> calleePool{PhysReg::S1, PhysReg::S2, PhysReg::S3, PhysReg::S4,
    PhysReg::S5, PhysReg::S6, PhysReg::S7, PhysReg::S8, PhysReg::S9, PhysReg::S10, PhysReg::S11};

} // namespace

AllocationResult LinearScanRegisterAllocator::run(MachineFunction& function) const {
    const auto liveness = computeLiveness(function);
    AllocationResult result;
    result.registers.resize(function.vregCount);
    result.spillSlots.resize(function.vregCount);
    std::map<VRegId, LiveInterval> intervals;
    for (const auto& interval : liveness.intervals) intervals[interval.vreg] = interval;
    std::vector<VRegId> active;
    std::uint32_t nextSpill = 0;
    auto spill = [&](VRegId id) {
        if (!result.spillSlots[id]) result.spillSlots[id] = StackSlot{StackSlotKind::Spill, nextSpill++};
        result.registers[id].reset();
    };
    for (const auto& current : liveness.intervals) {
        active.erase(std::remove_if(active.begin(), active.end(), [&](VRegId id) {
            return intervals[id].end < current.start;
        }), active.end());
        std::set<PhysReg> used;
        for (const VRegId id : active) if (result.registers[id]) used.insert(*result.registers[id]);
        const auto& pool = current.crossesCall ? calleePool : callerPool;
        const auto free = std::find_if(pool.begin(), pool.end(), [&](PhysReg reg) { return !used.contains(reg); });
        if (free != pool.end()) result.registers[current.vreg] = *free;
        else {
            auto victim = active.end();
            for (auto iterator = active.begin(); iterator != active.end(); ++iterator) {
                if (!result.registers[*iterator] || std::find(pool.begin(), pool.end(), *result.registers[*iterator]) == pool.end()) continue;
                if (victim == active.end() || intervals[*iterator].end > intervals[*victim].end) victim = iterator;
            }
            if (victim != active.end() && intervals[*victim].end > current.end) {
                const PhysReg reg = *result.registers[*victim];
                spill(*victim); active.erase(victim); result.registers[current.vreg] = reg;
            } else spill(current.vreg);
        }
        if (result.registers[current.vreg]) active.push_back(current.vreg);
        std::sort(active.begin(), active.end(), [&](VRegId a, VRegId b) {
            return intervals[a].end != intervals[b].end ? intervals[a].end < intervals[b].end : a < b;
        });
    }

    for (auto& block : function.blocks) {
        std::vector<MInstruction> rewritten;
        for (auto instruction : block.instructions) {
            std::map<VRegId, PhysReg> scratchFor;
            std::vector<MInstruction> before, after;
            const auto scratch = [&](VRegId id) {
                if (const auto found = scratchFor.find(id); found != scratchFor.end()) return found->second;
                const PhysReg reg = scratchFor.size() % 2 == 0 ? PhysReg::T5 : PhysReg::T6;
                scratchFor[id] = reg; return reg;
            };
            for (auto& operand : instruction.uses) if (const auto* virtualReg = std::get_if<VirtualReg>(&operand)) {
                const VRegId id = virtualReg->id;
                if (result.registers[id]) operand = *result.registers[id];
                else {
                    const PhysReg reg = scratch(id);
                    before.push_back({MOpcode::LW, {reg}, {*result.spillSlots[id]}, {}, {}, instruction.location});
                    operand = reg;
                }
            }
            for (auto& operand : instruction.defs) if (const auto* virtualReg = std::get_if<VirtualReg>(&operand)) {
                const VRegId id = virtualReg->id;
                if (result.registers[id]) operand = *result.registers[id];
                else {
                    const PhysReg reg = scratch(id);
                    operand = reg;
                    after.push_back({MOpcode::SW, {}, {reg, *result.spillSlots[id]}, {}, {}, instruction.location});
                }
            }
            rewritten.insert(rewritten.end(), before.begin(), before.end());
            if (!(instruction.opcode == MOpcode::COPY && instruction.defs.size() == 1 &&
                  instruction.uses.size() == 1 && instruction.defs[0] == instruction.uses[0]))
                rewritten.push_back(std::move(instruction));
            rewritten.insert(rewritten.end(), after.begin(), after.end());
        }
        block.instructions = std::move(rewritten);
    }
    for (const auto& reg : result.registers) if (reg && isCalleeSaved(*reg)) result.usedCalleeSaved.push_back(*reg);
    std::sort(result.usedCalleeSaved.begin(), result.usedCalleeSaved.end());
    result.usedCalleeSaved.erase(std::unique(result.usedCalleeSaved.begin(), result.usedCalleeSaved.end()), result.usedCalleeSaved.end());
    function.usedCalleeSaved = result.usedCalleeSaved;
    function.spillSlotCount = nextSpill;
    function.stage = MIRStage::PostRegisterAllocation;
    return result;
}

} // namespace toyc

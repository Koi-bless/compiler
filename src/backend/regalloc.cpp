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
const std::vector<PhysReg> leafCallerPool{
    PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3, PhysReg::T4,
    PhysReg::A0, PhysReg::A1, PhysReg::A2, PhysReg::A3,
    PhysReg::A4, PhysReg::A5, PhysReg::A6, PhysReg::A7,
    PhysReg::S1, PhysReg::S2, PhysReg::S3, PhysReg::S4, PhysReg::S5, PhysReg::S6,
    PhysReg::S7, PhysReg::S8, PhysReg::S9, PhysReg::S10, PhysReg::S11};
const std::vector<PhysReg> calleePool{PhysReg::S1, PhysReg::S2, PhysReg::S3, PhysReg::S4,
    PhysReg::S5, PhysReg::S6, PhysReg::S7, PhysReg::S8, PhysReg::S9, PhysReg::S10, PhysReg::S11};

} // namespace

AllocationResult LinearScanRegisterAllocator::run(MachineFunction& function) const {
    const auto liveness = computeLiveness(function);
    bool readsArgumentRegister = false;
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            for (const auto& operand : instruction.uses)
                if (const auto* reg = std::get_if<PhysReg>(&operand);
                    reg && *reg >= PhysReg::A0 && *reg <= PhysReg::A7)
                    readsArgumentRegister = true;
    // In a leaf with no incoming argument-register reads, a0-a7 are ordinary
    // caller-saved temporaries. The final copy to a0 is a definition and is
    // safe; parameterized leaves stay on the conservative pool until fixed
    // physical-register interference is modeled explicitly.
    const auto& generalPool = !function.hasCalls && !readsArgumentRegister
        ? leafCallerPool : callerPool;
    AllocationResult result;
    result.registers.resize(function.vregCount);
    result.spillSlots.resize(function.vregCount);
    result.rematerializations.resize(function.vregCount);
    std::vector<std::vector<VRegId>> hints(function.vregCount);
    std::vector<unsigned> definitionCounts(function.vregCount, 0);
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        if (options_.enableCopyHints && instruction.opcode == MOpcode::COPY &&
            instruction.defs.size() == 1 && instruction.uses.size() == 1) {
            const auto* destination = std::get_if<VirtualReg>(&instruction.defs[0]);
            const auto* source = std::get_if<VirtualReg>(&instruction.uses[0]);
            if (destination && source) {
                hints[destination->id].push_back(source->id);
                hints[source->id].push_back(destination->id);
            }
        }
        for (const auto& operand : instruction.defs) if (const auto* reg = std::get_if<VirtualReg>(&operand)) {
            ++definitionCounts[reg->id];
            if (options_.enableRematerialization && definitionCounts[reg->id] == 1 &&
                instruction.opcode == MOpcode::LI && instruction.uses.size() == 1) {
                if (const auto* immediate = std::get_if<Immediate>(&instruction.uses[0]))
                    result.rematerializations[reg->id] = Rematerialization{MOpcode::LI, *immediate};
            } else {
                result.rematerializations[reg->id].reset();
            }
        }
    }
    std::map<VRegId, LiveInterval> intervals;
    for (const auto& interval : liveness.intervals) intervals[interval.vreg] = interval;
    std::vector<VRegId> active;
    auto spill = [&](VRegId id) {
        result.registers[id].reset();
    };
    for (const auto& current : liveness.intervals) {
        active.erase(std::remove_if(active.begin(), active.end(), [&](VRegId id) {
            return intervals[id].end < current.start;
        }), active.end());
        std::set<PhysReg> used;
        for (const VRegId id : active) if (result.registers[id]) used.insert(*result.registers[id]);
        const auto& pool = current.crossesCall ? calleePool : generalPool;
        std::optional<PhysReg> hinted;
        if (options_.enableCopyHints) for (const VRegId other : hints[current.vreg]) {
            if (!result.registers[other]) continue;
            const PhysReg candidate = *result.registers[other];
            if (std::find(pool.begin(), pool.end(), candidate) == pool.end()) continue;
            bool conflict = used.contains(candidate);
            if (conflict && intervals.contains(other) &&
                intervals[other].end <= current.start)
                conflict = false;
            if (!conflict) { hinted = candidate; break; }
        }
        const auto free = std::find_if(pool.begin(), pool.end(), [&](PhysReg reg) {
            return !used.contains(reg);
        });
        if (hinted) result.registers[current.vreg] = *hinted;
        else if (free != pool.end()) result.registers[current.vreg] = *free;
        else {
            auto victim = active.end();
            const auto priority = [&](VRegId id) {
                const auto& interval = intervals[id];
                const auto remaining = std::max<std::uint32_t>(
                    1, interval.end >= current.start
                        ? interval.end - current.start + 1 : 1);
                return interval.spillWeight / static_cast<double>(remaining);
            };
            for (auto iterator = active.begin(); iterator != active.end(); ++iterator) {
                if (!result.registers[*iterator] || std::find(pool.begin(), pool.end(), *result.registers[*iterator]) == pool.end()) continue;
                if (victim == active.end() || priority(*iterator) < priority(*victim) ||
                    (priority(*iterator) == priority(*victim) &&
                     intervals[*iterator].end > intervals[*victim].end))
                    victim = iterator;
            }
            if (victim != active.end() &&
                (priority(*victim) < priority(current.vreg) ||
                 (priority(*victim) == priority(current.vreg) &&
                  intervals[*victim].end > current.end))) {
                const PhysReg reg = *result.registers[*victim];
                spill(*victim); active.erase(victim); result.registers[current.vreg] = reg;
            } else spill(current.vreg);
        }
        if (result.registers[current.vreg]) active.push_back(current.vreg);
        std::sort(active.begin(), active.end(), [&](VRegId a, VRegId b) {
            return intervals[a].end != intervals[b].end ? intervals[a].end < intervals[b].end : a < b;
        });
    }

    // Color abstract spill intervals onto reusable stack slots. A slot can be
    // shared as soon as the previous value's inclusive interval ends before
    // the next one starts.
    std::vector<std::uint32_t> spillSlotEnds;
    for (const auto& interval : liveness.intervals) {
        const VRegId id = interval.vreg;
        if (result.registers[id] || result.rematerializations[id]) continue;
        std::size_t slot = 0;
        while (slot < spillSlotEnds.size() &&
               spillSlotEnds[slot] >= interval.start)
            ++slot;
        if (slot == spillSlotEnds.size()) spillSlotEnds.push_back(interval.end);
        else spillSlotEnds[slot] = interval.end;
        result.spillSlots[id] = StackSlot{
            StackSlotKind::Spill, static_cast<std::uint32_t>(slot)};
    }
    const std::uint32_t nextSpill =
        static_cast<std::uint32_t>(spillSlotEnds.size());
    for (auto& block : function.blocks) {
        std::vector<MInstruction> rewritten;
        for (auto instruction : block.instructions) {
            bool discardRematerializedDefinition = false;
            if (options_.enableRematerialization && instruction.opcode == MOpcode::LI &&
                instruction.defs.size() == 1) {
                if (const auto* reg = std::get_if<VirtualReg>(&instruction.defs[0]);
                    reg && !result.registers[reg->id] && result.rematerializations[reg->id])
                    discardRematerializedDefinition = true;
            }
            if (discardRematerializedDefinition) continue;
            std::map<VRegId, PhysReg> scratchFor;
            std::vector<MInstruction> before, after;
            const auto scratch = [&](VRegId id, bool mayReuseInput = false) {
                if (const auto found = scratchFor.find(id); found != scratchFor.end()) return found->second;
                if (scratchFor.size() >= 2) {
                    if (mayReuseInput && instruction.defs.size() == 1) {
                        const PhysReg reg = scratchFor.begin()->second;
                        scratchFor[id] = reg;
                        return reg;
                    }
                    throw CompileError(function.location, "register allocation",
                                       "instruction needs more than two spill scratch registers");
                }
                const PhysReg reg = scratchFor.size() % 2 == 0 ? PhysReg::T5 : PhysReg::T6;
                scratchFor[id] = reg; return reg;
            };
            for (auto& operand : instruction.uses) if (const auto* virtualReg = std::get_if<VirtualReg>(&operand)) {
                const VRegId id = virtualReg->id;
                if (result.registers[id]) operand = *result.registers[id];
                else {
                    const PhysReg reg = scratch(id);
                    if (result.rematerializations[id])
                        before.push_back({MOpcode::LI, {reg},
                            {result.rematerializations[id]->immediate}, {}, {}, instruction.location});
                    else
                        before.push_back({MOpcode::LW, {reg}, {*result.spillSlots[id]}, {}, {}, instruction.location});
                    operand = reg;
                }
            }
            for (auto& operand : instruction.defs) if (const auto* virtualReg = std::get_if<VirtualReg>(&operand)) {
                const VRegId id = virtualReg->id;
                if (result.registers[id]) operand = *result.registers[id];
                else {
                    // A single destination may reuse an input scratch: RV32 reads
                    // both source operands before writing the destination.
                    const PhysReg reg = scratch(id, true);
                    operand = reg;
                    if (result.spillSlots[id])
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

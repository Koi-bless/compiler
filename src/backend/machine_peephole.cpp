#include "toyc/backend/machine_peephole.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

bool purePhysicalDefinition(MOpcode opcode) {
    switch (opcode) {
    case MOpcode::LI: case MOpcode::LA: case MOpcode::COPY:
    case MOpcode::ADD: case MOpcode::ADDI: case MOpcode::SUB: case MOpcode::MUL:
    case MOpcode::SLT: case MOpcode::SLTU: case MOpcode::XOR:
    case MOpcode::XORI: case MOpcode::SLTIU: case MOpcode::SLLI:
        return true;
    default:
        return false;
    }
}

void invalidateRegister(std::map<StackSlot, PhysReg>& known, PhysReg reg) {
    for (auto iterator = known.begin(); iterator != known.end();) {
        if (iterator->second == reg) iterator = known.erase(iterator);
        else ++iterator;
    }
}

bool forwardStackSlots(MachineFunction& function,
                       const MachinePeepholeOptions& options,
                       MachinePassResult& result) {
    bool changed = false;
    for (auto& block : function.blocks) {
        std::map<StackSlot, PhysReg> known;
        std::map<StackSlot, std::size_t> pendingStores;
        std::vector<MInstruction> output;
        std::vector<bool> removed;
        output.reserve(block.instructions.size());
        removed.reserve(block.instructions.size());

        for (auto instruction : block.instructions) {
            const bool stackLoad = instruction.opcode == MOpcode::LW &&
                instruction.defs.size() == 1 && instruction.uses.size() == 1 &&
                std::holds_alternative<PhysReg>(instruction.defs[0]) &&
                std::holds_alternative<StackSlot>(instruction.uses[0]);
            const bool stackStore = instruction.opcode == MOpcode::SW &&
                instruction.uses.size() == 2 &&
                std::holds_alternative<PhysReg>(instruction.uses[0]) &&
                std::holds_alternative<StackSlot>(instruction.uses[1]);

            if (stackLoad) {
                const PhysReg destination = std::get<PhysReg>(instruction.defs[0]);
                const StackSlot slot = std::get<StackSlot>(instruction.uses[0]);
                const auto cached = known.find(slot);
                if (cached != known.end()) {
                    const PhysReg source = cached->second;
                    instruction.opcode = MOpcode::COPY;
                    instruction.uses = {source};
                    ++result.loadsForwarded;
                    ++result.instructionsRewritten;
                    result.changed = true;
                    changed = true;
                    if (destination != source) invalidateRegister(known, destination);
                    known[slot] = destination;
                } else {
                    invalidateRegister(known, destination);
                    known[slot] = destination;
                }
                pendingStores.erase(slot);
            } else if (stackStore) {
                const PhysReg source = std::get<PhysReg>(instruction.uses[0]);
                const StackSlot slot = std::get<StackSlot>(instruction.uses[1]);
                const bool spill = slot.kind == StackSlotKind::Spill;
                if (options.eliminateLocalDeadSpillStores && spill) {
                    const auto cached = known.find(slot);
                    if (cached != known.end() && cached->second == source) {
                        ++result.instructionsRemoved;
                        ++result.storesRemoved;
                        result.changed = true;
                        changed = true;
                        continue;
                    }
                    if (const auto previous = pendingStores.find(slot);
                        previous != pendingStores.end()) {
                        removed[previous->second] = true;
                        ++result.instructionsRemoved;
                        ++result.storesRemoved;
                        result.changed = true;
                        changed = true;
                    }
                }
                known[slot] = source;
                if (spill) pendingStores[slot] = output.size();
            } else {
                for (const auto& operand : instruction.defs)
                    if (const auto* reg = std::get_if<PhysReg>(&operand))
                        invalidateRegister(known, *reg);
                for (const PhysReg reg : instruction.implicitDefs)
                    invalidateRegister(known, reg);
                if (instruction.opcode == MOpcode::CALL) {
                    known.clear();
                    pendingStores.clear();
                } else if (instruction.opcode == MOpcode::SW) {
                    // A non-frame store may alias memory visible to later code.  Treat it
                    // as a conservative barrier even though private spill slots cannot alias.
                    known.clear();
                    pendingStores.clear();
                }
            }
            output.push_back(std::move(instruction));
            removed.push_back(false);
        }

        if (std::any_of(removed.begin(), removed.end(), [](bool value) { return value; })) {
            std::vector<MInstruction> kept;
            kept.reserve(output.size());
            for (std::size_t index = 0; index < output.size(); ++index)
                if (!removed[index]) kept.push_back(std::move(output[index]));
            block.instructions = std::move(kept);
        } else {
            block.instructions = std::move(output);
        }
    }
    return changed;
}

bool removeSelfCopies(MachineFunction& function, MachinePassResult& result) {
    bool changed = false;
    for (auto& block : function.blocks) {
        std::vector<MInstruction> kept;
        kept.reserve(block.instructions.size());
        for (auto& instruction : block.instructions) {
            const bool selfCopy = instruction.opcode == MOpcode::COPY &&
                instruction.defs.size() == 1 && instruction.uses.size() == 1 &&
                std::holds_alternative<PhysReg>(instruction.defs[0]) &&
                instruction.defs[0] == instruction.uses[0];
            if (selfCopy) {
                changed = true;
                result.changed = true;
                ++result.instructionsRemoved;
            } else {
                kept.push_back(std::move(instruction));
            }
        }
        block.instructions = std::move(kept);
    }
    return changed;
}

bool removeOverwrittenDefinitions(MachineFunction& function,
                                  MachinePassResult& result) {
    bool changed = false;
    for (auto& block : function.blocks) {
        std::map<PhysReg, std::size_t> candidates;
        std::vector<bool> removed(block.instructions.size());
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            for (const auto& operand : instruction.uses)
                if (const auto* reg = std::get_if<PhysReg>(&operand))
                    candidates.erase(*reg);
            for (const PhysReg reg : instruction.implicitUses)
                candidates.erase(reg);

            const auto overwrite = [&](PhysReg reg) {
                if (const auto old = candidates.find(reg); old != candidates.end()) {
                    removed[old->second] = true;
                    candidates.erase(old);
                }
            };
            for (const auto& operand : instruction.defs)
                if (const auto* reg = std::get_if<PhysReg>(&operand)) overwrite(*reg);
            for (const PhysReg reg : instruction.implicitDefs) overwrite(reg);

            if (purePhysicalDefinition(instruction.opcode) &&
                instruction.defs.size() == 1 && instruction.implicitDefs.empty() &&
                instruction.implicitUses.empty()) {
                if (const auto* reg = std::get_if<PhysReg>(&instruction.defs[0]))
                    candidates[*reg] = index;
            }
        }
        if (std::any_of(removed.begin(), removed.end(), [](bool value) { return value; })) {
            std::vector<MInstruction> kept;
            kept.reserve(block.instructions.size());
            for (std::size_t index = 0; index < block.instructions.size(); ++index) {
                if (removed[index]) {
                    changed = true;
                    result.changed = true;
                    ++result.instructionsRemoved;
                } else {
                    kept.push_back(std::move(block.instructions[index]));
                }
            }
            block.instructions = std::move(kept);
        }
    }
    return changed;
}

} // namespace

MachinePassResult runPostRAPeephole(
    MachineFunction& function, const MachinePeepholeOptions& options) {
    if (function.stage != MIRStage::PostRegisterAllocation)
        throw CompileError(function.location, "machine peephole",
                           "post-RA peephole requires post-RA MIR");
    MachinePassResult result;
    bool changed = false;
    do {
        changed = false;
        if (options.forwardStackSlots && forwardStackSlots(function, options, result)) changed = true;
        if (options.removeSelfCopies && removeSelfCopies(function, result)) changed = true;
        if (options.removeOverwrittenDefinitions &&
            removeOverwrittenDefinitions(function, result)) changed = true;
    } while (changed);
    return result;
}

} // namespace toyc

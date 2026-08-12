#include "toyc/backend/machine_combine.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <vector>

#include "toyc/backend/riscv32.hpp"
#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

struct DefinitionInfo {
    std::vector<unsigned> counts;
    std::vector<std::optional<std::int32_t>> constants;
};

DefinitionInfo buildDefinitions(const MachineFunction& function) {
    DefinitionInfo info;
    info.counts.resize(function.vregCount);
    info.constants.resize(function.vregCount);
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            for (const auto& operand : instruction.defs)
                if (const auto* reg = std::get_if<VirtualReg>(&operand))
                    ++info.counts[reg->id];
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != MOpcode::LI || instruction.defs.size() != 1 ||
                instruction.uses.size() != 1)
                continue;
            const auto* reg = std::get_if<VirtualReg>(&instruction.defs[0]);
            const auto* immediate = std::get_if<Immediate>(&instruction.uses[0]);
            if (reg && immediate && info.counts[reg->id] == 1)
                info.constants[reg->id] = immediate->value;
        }
    return info;
}

std::optional<std::int32_t> constantFor(
    const MOperand& operand, const DefinitionInfo& definitions) {
    const auto* reg = std::get_if<VirtualReg>(&operand);
    if (!reg || reg->id >= definitions.constants.size()) return std::nullopt;
    return definitions.constants[reg->id];
}

bool pureDefinition(MOpcode opcode) {
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

bool zeroRegisterOpcode(MOpcode opcode) {
    switch (opcode) {
    case MOpcode::COPY: case MOpcode::ADD: case MOpcode::SUB: case MOpcode::XOR:
    case MOpcode::SLT: case MOpcode::SLTU: case MOpcode::BEQ: case MOpcode::BNE:
    case MOpcode::BLT: case MOpcode::BGE:
        return true;
    default:
        return false;
    }
}

bool targetCombine(MachineFunction& function, const MachineCombineOptions& options,
                   MachinePassResult& result) {
    const DefinitionInfo definitions = buildDefinitions(function);
    bool changed = false;
    for (auto& block : function.blocks) {
        for (std::size_t instructionIndex = 0;
             instructionIndex < block.instructions.size(); ++instructionIndex) {
            auto& instruction = block.instructions[instructionIndex];
            if (options.combineImmediates && instruction.defs.size() == 1 &&
                instruction.uses.size() == 2) {
                if (instruction.opcode == MOpcode::ADD) {
                    std::optional<std::int32_t> constant = constantFor(instruction.uses[1], definitions);
                    std::size_t valueIndex = 0;
                    if (!constant || !riscv32::fitsImmediate(*constant)) {
                        constant = constantFor(instruction.uses[0], definitions);
                        valueIndex = 1;
                    }
                    if (constant && riscv32::fitsImmediate(*constant)) {
                        const MOperand value = instruction.uses[valueIndex];
                        instruction.opcode = MOpcode::ADDI;
                        instruction.uses = {value, Immediate{*constant}};
                        changed = true;
                        ++result.instructionsRewritten;
                    }
                } else if (instruction.opcode == MOpcode::SUB) {
                    if (const auto constant = constantFor(instruction.uses[1], definitions)) {
                        const std::int64_t negated = -static_cast<std::int64_t>(*constant);
                        if (riscv32::fitsImmediate(negated)) {
                            instruction.opcode = MOpcode::ADDI;
                            instruction.uses[1] = Immediate{static_cast<std::int32_t>(negated)};
                            changed = true;
                            ++result.instructionsRewritten;
                        }
                    }
                }
            }
            if (options.reducePowerOfTwoMultiply && instruction.opcode == MOpcode::MUL &&
                instruction.defs.size() == 1 && instruction.uses.size() == 2) {
                bool expanded = false;
                for (const std::size_t constantIndex : {std::size_t{1}, std::size_t{0}}) {
                    const auto constant = constantFor(instruction.uses[constantIndex], definitions);
                    if (!constant) continue;
                    const std::uint32_t bits = std::bit_cast<std::uint32_t>(*constant);
                    const MOperand value = instruction.uses[1 - constantIndex];
                    if (bits != 0 && (bits & (bits - 1)) == 0) {
                        const unsigned shift = std::countr_zero(bits);
                        instruction.opcode = shift == 0 ? MOpcode::COPY : MOpcode::SLLI;
                        instruction.uses = shift == 0
                            ? std::vector<MOperand>{value}
                            : std::vector<MOperand>{value, Immediate{static_cast<std::int32_t>(shift)}};
                        changed = true;
                        ++result.instructionsRewritten;
                        break;
                    }
                    // RV32IM multiplication can have multi-cycle latency.  A
                    // power-of-two plus/minus one has a target cost of two
                    // single-cycle ALU operations and needs no live constant.
                    if (*constant <= 0) continue;
                    for (unsigned shift = 1; shift <= 30; ++shift) {
                        const std::uint32_t power = std::uint32_t{1} << shift;
                        const bool plus = bits == power + 1;
                        const bool minus = bits == power - 1;
                        if (!plus && !minus) continue;
                        const MOperand destination = instruction.defs[0];
                        const MOperand temporary = vreg(function.vregCount++);
                        instruction.opcode = MOpcode::SLLI;
                        instruction.defs = {temporary};
                        instruction.uses = {value,
                            Immediate{static_cast<std::int32_t>(shift)}};
                        MInstruction arithmetic{
                            plus ? MOpcode::ADD : MOpcode::SUB,
                            {destination}, {temporary, value}, {}, {},
                            instruction.location};
                        block.instructions.insert(
                            block.instructions.begin() +
                                static_cast<std::ptrdiff_t>(instructionIndex + 1),
                            std::move(arithmetic));
                        changed = true;
                        expanded = true;
                        result.instructionsRewritten += 2;
                        break;
                    }
                    if (expanded) break;
                }
                if (expanded) continue;
            }
            if (options.useZeroRegister && zeroRegisterOpcode(instruction.opcode)) {
                const std::size_t registerUses =
                    instruction.opcode == MOpcode::BEQ || instruction.opcode == MOpcode::BNE ||
                    instruction.opcode == MOpcode::BLT || instruction.opcode == MOpcode::BGE
                        ? 2 : instruction.uses.size();
                for (std::size_t index = 0; index < registerUses; ++index) {
                    const auto constant = constantFor(instruction.uses[index], definitions);
                    if (constant && *constant == 0) {
                        instruction.uses[index] = PhysReg::Zero;
                        changed = true;
                        ++result.instructionsRewritten;
                    }
                }
            }
        }
    }
    return changed;
}

bool propagateOneCopy(MachineFunction& function, MachinePassResult& result) {
    const DefinitionInfo definitions = buildDefinitions(function);
    for (auto& block : function.blocks) {
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            if (instruction.opcode != MOpcode::COPY || instruction.defs.size() != 1 ||
                instruction.uses.size() != 1)
                continue;
            const auto* destination = std::get_if<VirtualReg>(&instruction.defs[0]);
            const auto* source = std::get_if<VirtualReg>(&instruction.uses[0]);
            if (!destination || !source || destination->id == source->id ||
                definitions.counts[destination->id] != 1 || definitions.counts[source->id] != 1)
                continue;
            bool createsSelfDependence = false;
            for (const auto& candidateBlock : function.blocks)
                for (const auto& candidate : candidateBlock.instructions) {
                    const bool definesSource = std::any_of(
                        candidate.defs.begin(), candidate.defs.end(), [&](const MOperand& operand) {
                            const auto* reg = std::get_if<VirtualReg>(&operand);
                            return reg && reg->id == source->id;
                        });
                    const bool usesDestination = std::any_of(
                        candidate.uses.begin(), candidate.uses.end(), [&](const MOperand& operand) {
                            const auto* reg = std::get_if<VirtualReg>(&operand);
                            return reg && reg->id == destination->id;
                        });
                    createsSelfDependence = createsSelfDependence ||
                                            (definesSource && usesDestination);
                }
            if (createsSelfDependence) continue;
            for (auto& useBlock : function.blocks)
                for (auto& useInstruction : useBlock.instructions)
                    for (auto& operand : useInstruction.uses)
                        if (const auto* reg = std::get_if<VirtualReg>(&operand);
                            reg && reg->id == destination->id)
                            operand = *source;
            block.instructions.erase(block.instructions.begin() + static_cast<std::ptrdiff_t>(index));
            result.changed = true;
            ++result.instructionsRemoved;
            return true;
        }
    }
    return false;
}

bool eliminateDeadDefinitions(MachineFunction& function, MachinePassResult& result) {
    std::vector<std::size_t> uses(function.vregCount);
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            for (const auto& operand : instruction.uses)
                if (const auto* reg = std::get_if<VirtualReg>(&operand))
                    ++uses[reg->id];
    bool changed = false;
    for (auto& block : function.blocks) {
        std::vector<MInstruction> kept;
        kept.reserve(block.instructions.size());
        for (auto& instruction : block.instructions) {
            bool removable = pureDefinition(instruction.opcode) && !instruction.defs.empty() &&
                             instruction.implicitDefs.empty() && instruction.implicitUses.empty();
            for (const auto& operand : instruction.defs) {
                const auto* reg = std::get_if<VirtualReg>(&operand);
                if (!reg || uses[reg->id] != 0) removable = false;
            }
            if (removable) {
                changed = true;
                result.changed = true;
                ++result.instructionsRemoved;
                for (const auto& operand : instruction.uses)
                    if (const auto* reg = std::get_if<VirtualReg>(&operand))
                        --uses[reg->id];
            } else {
                kept.push_back(std::move(instruction));
            }
        }
        block.instructions = std::move(kept);
    }
    return changed;
}

} // namespace

MachinePassResult runPreRAMachineCombine(
    MachineFunction& function, const MachineCombineOptions& options) {
    if (function.stage != MIRStage::PreRegisterAllocation)
        throw CompileError(function.location, "machine combine",
                           "pre-RA machine combine requires pre-RA MIR");
    MachinePassResult result;
    bool localChanged = false;
    do {
        localChanged = targetCombine(function, options, result);
        result.changed = result.changed || localChanged;
        if (options.propagateVirtualCopies)
            while (propagateOneCopy(function, result)) localChanged = true;
        if (options.eliminateDeadDefinitions && eliminateDeadDefinitions(function, result))
            localChanged = true;
    } while (localChanged);
    return result;
}

} // namespace toyc

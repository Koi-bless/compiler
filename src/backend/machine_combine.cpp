#include "toyc/backend/machine_combine.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
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

struct SignedPowerTerm {
    unsigned shift = 0;
    bool negative = false;
};

// Canonical signed-digit form minimizes the number of non-zero power-of-two
// terms (for example 31 = 32 - 1). It gives a small, target-independent search
// space for RV32I shift/add/sub multiplication candidates.
std::vector<SignedPowerTerm> signedPowerTerms(std::uint32_t value) {
    std::vector<SignedPowerTerm> terms;
    unsigned shift = 0;
    while (value != 0) {
        if ((value & 1U) != 0) {
            const bool negative = (value & 3U) == 3U;
            terms.push_back({shift, negative});
            value = negative ? value + 1U : value - 1U;
        }
        value >>= 1U;
        ++shift;
    }
    std::stable_sort(terms.begin(), terms.end(),
        [](const SignedPowerTerm& left, const SignedPowerTerm& right) {
            return left.negative < right.negative;
        });
    return terms;
}

unsigned signedPowerCost(const std::vector<SignedPowerTerm>& terms) {
    if (terms.empty()) return 0;
    unsigned cost = terms.front().negative ? 1U : 0U;
    for (const auto& term : terms)
        if (term.shift != 0) ++cost;
    cost += static_cast<unsigned>(terms.size() - 1);
    return cost;
}

std::vector<MInstruction> buildConstantMultiply(
    MachineFunction& function, MOperand destination, MOperand value,
    std::uint32_t constant, SourceLocation location) {
    const auto terms = signedPowerTerms(constant);
    std::vector<MInstruction> sequence;
    std::optional<MOperand> accumulator;
    for (std::size_t index = 0; index < terms.size(); ++index) {
        const auto& term = terms[index];
        MOperand termValue = value;
        if (term.shift != 0) {
            termValue = vreg(function.vregCount++);
            sequence.push_back({MOpcode::SLLI, {termValue},
                {value, Immediate{static_cast<std::int32_t>(term.shift)}},
                {}, {}, location});
        }
        const bool last = index + 1 == terms.size();
        if (!accumulator) {
            if (!term.negative) {
                accumulator = termValue;
            } else {
                const MOperand next = last ? destination : MOperand{vreg(function.vregCount++)};
                sequence.push_back({MOpcode::SUB, {next},
                    {PhysReg::Zero, termValue}, {}, {}, location});
                accumulator = next;
            }
            continue;
        }
        const MOperand next = last ? destination : MOperand{vreg(function.vregCount++)};
        sequence.push_back({term.negative ? MOpcode::SUB : MOpcode::ADD,
            {next}, {*accumulator, termValue}, {}, {}, location});
        accumulator = next;
    }
    return sequence;
}

struct SignedDivisionMagic {
    std::int32_t multiplier = 0;
    unsigned shift = 0;
};

SignedDivisionMagic signedDivisionMagic(std::int32_t divisor) {
    const std::uint64_t absolute = divisor < 0
        ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(divisor))
        : static_cast<std::uint64_t>(divisor);
    const std::uint64_t two31 = std::uint64_t{1} << 31U;
    const std::uint64_t t = two31 +
        (static_cast<std::uint32_t>(divisor) >> 31U);
    const std::uint64_t anc = t - 1U - t % absolute;
    unsigned exponent = 31;
    std::uint64_t quotient1 = two31 / anc;
    std::uint64_t remainder1 = two31 - quotient1 * anc;
    std::uint64_t quotient2 = two31 / absolute;
    std::uint64_t remainder2 = two31 - quotient2 * absolute;
    std::uint64_t delta = 0;
    do {
        ++exponent;
        quotient1 <<= 1U;
        remainder1 <<= 1U;
        if (remainder1 >= anc) {
            ++quotient1;
            remainder1 -= anc;
        }
        quotient2 <<= 1U;
        remainder2 <<= 1U;
        if (remainder2 >= absolute) {
            ++quotient2;
            remainder2 -= absolute;
        }
        delta = absolute - remainder2;
    } while (quotient1 < delta ||
             (quotient1 == delta && remainder1 == 0));

    std::uint32_t bits = static_cast<std::uint32_t>(quotient2 + 1U);
    if (divisor < 0) bits = 0U - bits;
    return {std::bit_cast<std::int32_t>(bits), exponent - 32U};
}

void appendSignedConstantQuotient(
    MachineFunction& function, std::vector<MInstruction>& sequence,
    MOperand destination, MOperand dividend, std::int32_t divisor,
    SourceLocation location) {
    if (divisor == 1) {
        sequence.push_back({MOpcode::COPY, {destination}, {dividend}, {}, {}, location});
        return;
    }
    if (divisor == -1) {
        sequence.push_back({MOpcode::SUB, {destination},
            {PhysReg::Zero, dividend}, {}, {}, location});
        return;
    }

    const std::uint32_t absolute = divisor < 0
        ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(divisor))
        : static_cast<std::uint32_t>(divisor);
    const auto fresh = [&]() -> MOperand { return vreg(function.vregCount++); };
    if ((absolute & (absolute - 1U)) == 0) {
        const unsigned shift = std::countr_zero(absolute);
        const MOperand sign = fresh();
        const MOperand bias = fresh();
        const MOperand adjusted = fresh();
        const MOperand positiveQuotient = divisor < 0 ? fresh() : destination;
        sequence.push_back({MOpcode::SRAI, {sign},
            {dividend, Immediate{31}}, {}, {}, location});
        sequence.push_back({MOpcode::SRLI, {bias},
            {sign, Immediate{static_cast<std::int32_t>(32U - shift)}},
            {}, {}, location});
        sequence.push_back({MOpcode::ADD, {adjusted},
            {dividend, bias}, {}, {}, location});
        sequence.push_back({MOpcode::SRAI, {positiveQuotient},
            {adjusted, Immediate{static_cast<std::int32_t>(shift)}},
            {}, {}, location});
        if (divisor < 0)
            sequence.push_back({MOpcode::SUB, {destination},
                {PhysReg::Zero, positiveQuotient}, {}, {}, location});
        return;
    }

    const auto magic = signedDivisionMagic(divisor);
    const MOperand multiplier = fresh();
    MOperand quotient = fresh();
    sequence.push_back({MOpcode::LI, {multiplier},
        {Immediate{magic.multiplier}}, {}, {}, location});
    sequence.push_back({MOpcode::MULH, {quotient},
        {dividend, multiplier}, {}, {}, location});
    if (divisor > 0 && magic.multiplier < 0) {
        const MOperand adjusted = fresh();
        sequence.push_back({MOpcode::ADD, {adjusted},
            {quotient, dividend}, {}, {}, location});
        quotient = adjusted;
    } else if (divisor < 0 && magic.multiplier > 0) {
        const MOperand adjusted = fresh();
        sequence.push_back({MOpcode::SUB, {adjusted},
            {quotient, dividend}, {}, {}, location});
        quotient = adjusted;
    }
    if (magic.shift != 0) {
        const MOperand shifted = fresh();
        sequence.push_back({MOpcode::SRAI, {shifted},
            {quotient, Immediate{static_cast<std::int32_t>(magic.shift)}},
            {}, {}, location});
        quotient = shifted;
    }
    const MOperand correction = fresh();
    sequence.push_back({MOpcode::SRLI, {correction},
        {quotient, Immediate{31}}, {}, {}, location});
    sequence.push_back({MOpcode::ADD, {destination},
        {quotient, correction}, {}, {}, location});
}

std::vector<MInstruction> buildConstantDivision(
    MachineFunction& function, MOperand destination, MOperand dividend,
    std::int32_t divisor, bool remainder, SourceLocation location) {
    std::vector<MInstruction> sequence;
    if (!remainder) {
        appendSignedConstantQuotient(
            function, sequence, destination, dividend, divisor, location);
        return sequence;
    }
    if (divisor == 1 || divisor == -1) {
        sequence.push_back({MOpcode::LI, {destination},
            {Immediate{0}}, {}, {}, location});
        return sequence;
    }
    const std::int32_t absolute = static_cast<std::int32_t>(
        divisor < 0 ? -static_cast<std::int64_t>(divisor) : divisor);
    const MOperand quotient = vreg(function.vregCount++);
    appendSignedConstantQuotient(
        function, sequence, quotient, dividend, absolute, location);
    const MOperand factor = vreg(function.vregCount++);
    const MOperand product = vreg(function.vregCount++);
    sequence.push_back({MOpcode::LI, {factor},
        {Immediate{absolute}}, {}, {}, location});
    sequence.push_back({MOpcode::MUL, {product},
        {quotient, factor}, {}, {}, location});
    sequence.push_back({MOpcode::SUB, {destination},
        {dividend, product}, {}, {}, location});
    return sequence;
}

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
    case MOpcode::MULH:
    case MOpcode::SLT: case MOpcode::SLTU: case MOpcode::XOR:
    case MOpcode::XORI: case MOpcode::SLTIU: case MOpcode::SLLI:
    case MOpcode::SRAI: case MOpcode::SRLI:
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

                // A materialized constant plus an RV32M multiply has a target
                // cost of roughly five simple ALU cycles on the baseline core.
                // Keep the expansion bounded so register pressure cannot grow
                // without limit for large, dense constants.
                for (const std::size_t constantIndex : {std::size_t{1}, std::size_t{0}}) {
                    const auto constant = constantFor(instruction.uses[constantIndex], definitions);
                    if (!constant || *constant <= 0) continue;
                    const auto terms = signedPowerTerms(
                        static_cast<std::uint32_t>(*constant));
                    if (terms.size() < 2 || signedPowerCost(terms) > 4) continue;
                    const MOperand destination = instruction.defs[0];
                    const MOperand value = instruction.uses[1 - constantIndex];
                    auto sequence = buildConstantMultiply(
                        function, destination, value,
                        static_cast<std::uint32_t>(*constant), instruction.location);
                    if (sequence.empty()) continue;
                    block.instructions.erase(
                        block.instructions.begin() +
                        static_cast<std::ptrdiff_t>(instructionIndex));
                    block.instructions.insert(
                        block.instructions.begin() +
                            static_cast<std::ptrdiff_t>(instructionIndex),
                        sequence.begin(), sequence.end());
                    instructionIndex += sequence.size() - 1;
                    changed = true;
                    expanded = true;
                    result.instructionsRewritten += sequence.size();
                    break;
                }
                if (expanded) continue;
            }
            if (options.reduceConstantDivision &&
                (instruction.opcode == MOpcode::DIV ||
                 instruction.opcode == MOpcode::REM) &&
                instruction.defs.size() == 1 && instruction.uses.size() == 2) {
                const auto divisor = constantFor(instruction.uses[1], definitions);
                if (divisor && *divisor != 0 &&
                    *divisor != std::numeric_limits<std::int32_t>::min()) {
                    const bool remainder = instruction.opcode == MOpcode::REM;
                    auto sequence = buildConstantDivision(
                        function, instruction.defs[0], instruction.uses[0],
                        *divisor, remainder, instruction.location);
                    block.instructions.erase(
                        block.instructions.begin() +
                        static_cast<std::ptrdiff_t>(instructionIndex));
                    block.instructions.insert(
                        block.instructions.begin() +
                            static_cast<std::ptrdiff_t>(instructionIndex),
                        sequence.begin(), sequence.end());
                    instructionIndex += sequence.size() - 1;
                    changed = true;
                    result.instructionsRewritten += sequence.size();
                    continue;
                }
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

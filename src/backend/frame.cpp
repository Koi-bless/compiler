#include "toyc/backend/frame.hpp"

#include <algorithm>

#include "toyc/backend/riscv32.hpp"
#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

std::vector<MInstruction> adjustStack(std::int32_t amount, SourceLocation location = {}) {
    if (amount == 0) return {};
    if (riscv32::fitsImmediate(amount))
        return {{MOpcode::ADDI, {PhysReg::Sp}, {PhysReg::Sp, Immediate{amount}}, {}, {}, location}};
    return {{MOpcode::LI, {PhysReg::T6}, {Immediate{amount}}, {}, {}, location},
            {MOpcode::ADD, {PhysReg::Sp}, {PhysReg::Sp, PhysReg::T6}, {}, {}, location}};
}

std::vector<MInstruction> loadAt(PhysReg destination, std::int32_t offset, SourceLocation location = {}) {
    if (riscv32::fitsImmediate(offset))
        return {{MOpcode::LW, {destination}, {PhysReg::Sp, Immediate{offset}}, {}, {}, location}};
    const PhysReg address = destination == PhysReg::T6 ? PhysReg::T5 : PhysReg::T6;
    return {{MOpcode::LI, {address}, {Immediate{offset}}, {}, {}, location},
            {MOpcode::ADD, {address}, {PhysReg::Sp, address}, {}, {}, location},
            {MOpcode::LW, {destination}, {address, Immediate{0}}, {}, {}, location}};
}

std::vector<MInstruction> storeAt(PhysReg value, std::int32_t offset, SourceLocation location = {}) {
    if (riscv32::fitsImmediate(offset))
        return {{MOpcode::SW, {}, {value, PhysReg::Sp, Immediate{offset}}, {}, {}, location}};
    const PhysReg address = value == PhysReg::T6 ? PhysReg::T5 : PhysReg::T6;
    return {{MOpcode::LI, {address}, {Immediate{offset}}, {}, {}, location},
            {MOpcode::ADD, {address}, {PhysReg::Sp, address}, {}, {}, location},
            {MOpcode::SW, {}, {value, address, Immediate{0}}, {}, {}, location}};
}

void append(std::vector<MInstruction>& target, std::vector<MInstruction> source) {
    target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

} // namespace

FrameLayout FrameLowering::run(MachineFunction& function) const {
    FrameLayout layout;
    std::uint32_t outgoingCount = 0;
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
        for (const auto& operand : instruction.uses)
            if (const auto* slot = std::get_if<StackSlot>(&operand); slot && slot->kind == StackSlotKind::OutgoingArgument)
                outgoingCount = std::max(outgoingCount, slot->index + 1);
    layout.outgoingBytes = static_cast<std::int32_t>(outgoingCount * riscv32::wordSize);
    std::int32_t offset = layout.outgoingBytes;
    layout.spillOffsets.resize(function.spillSlotCount);
    for (auto& spillOffset : layout.spillOffsets) { spillOffset = offset; offset += static_cast<std::int32_t>(riscv32::wordSize); }
    for (const PhysReg reg : function.usedCalleeSaved) {
        layout.calleeSavedOffsets.emplace_back(reg, offset);
        offset += static_cast<std::int32_t>(riscv32::wordSize);
    }
    if (function.hasCalls) { layout.raOffset = offset; offset += static_cast<std::int32_t>(riscv32::wordSize); }
    layout.frameSize = static_cast<std::int32_t>(riscv32::alignUp(offset, riscv32::stackAlignment));
    function.frameSize = layout.frameSize;

    const auto slotOffset = [&](const StackSlot& slot) -> std::int32_t {
        switch (slot.kind) {
        case StackSlotKind::Spill:
            if (slot.index >= layout.spillOffsets.size()) throw CompileError(function.location, "frame lowering", "invalid spill slot");
            return layout.spillOffsets[slot.index];
        case StackSlotKind::IncomingArgument:
            return layout.frameSize + static_cast<std::int32_t>(slot.index * riscv32::wordSize);
        case StackSlotKind::OutgoingArgument:
            return static_cast<std::int32_t>(slot.index * riscv32::wordSize);
        case StackSlotKind::CalleeSaved: case StackSlotKind::ReturnAddress: break;
        }
        throw CompileError(function.location, "frame lowering", "unexpected abstract frame object");
    };

    for (auto& block : function.blocks) {
        std::vector<MInstruction> output;
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode == MOpcode::LW && instruction.defs.size() == 1 && instruction.uses.size() == 1 &&
                std::holds_alternative<StackSlot>(instruction.uses[0])) {
                const auto* destination = std::get_if<PhysReg>(&instruction.defs[0]);
                if (!destination) throw CompileError(function.location, "frame lowering", "stack load destination is not physical");
                append(output, loadAt(*destination, slotOffset(std::get<StackSlot>(instruction.uses[0])), instruction.location));
            } else if (instruction.opcode == MOpcode::SW && instruction.uses.size() == 2 &&
                       std::holds_alternative<StackSlot>(instruction.uses[1])) {
                const auto* value = std::get_if<PhysReg>(&instruction.uses[0]);
                if (!value) throw CompileError(function.location, "frame lowering", "stack store source is not physical");
                append(output, storeAt(*value, slotOffset(std::get<StackSlot>(instruction.uses[1])), instruction.location));
            } else output.push_back(instruction);
        }
        block.instructions = std::move(output);
    }

    auto& entry = function.blocks[function.entry].instructions;
    std::vector<MInstruction> prologue;
    append(prologue, adjustStack(-layout.frameSize, function.location));
    for (const auto& [reg, saveOffset] : layout.calleeSavedOffsets) append(prologue, storeAt(reg, saveOffset, function.location));
    if (layout.raOffset) append(prologue, storeAt(PhysReg::Ra, *layout.raOffset, function.location));
    prologue.insert(prologue.end(), std::make_move_iterator(entry.begin()), std::make_move_iterator(entry.end()));
    entry = std::move(prologue);

    if (!function.epilogue) throw CompileError(function.location, "frame lowering", "function lacks epilogue block");
    auto& epilogue = function.blocks[*function.epilogue].instructions;
    std::vector<MInstruction> lowered;
    for (auto iterator = layout.calleeSavedOffsets.rbegin(); iterator != layout.calleeSavedOffsets.rend(); ++iterator)
        append(lowered, loadAt(iterator->first, iterator->second, function.location));
    if (layout.raOffset) append(lowered, loadAt(PhysReg::Ra, *layout.raOffset, function.location));
    append(lowered, adjustStack(layout.frameSize, function.location));
    lowered.insert(lowered.end(), std::make_move_iterator(epilogue.begin()), std::make_move_iterator(epilogue.end()));
    epilogue = std::move(lowered);
    function.stage = MIRStage::AfterFrameLowering;
    return layout;
}

} // namespace toyc

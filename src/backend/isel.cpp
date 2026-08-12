#include "toyc/backend/isel.hpp"

#include <algorithm>
#include <map>
#include <utility>

#include "toyc/backend/phi_lowering.hpp"
#include "toyc/opt/ir_utils.hpp"
#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

PhysReg argumentRegister(std::size_t index) {
    return static_cast<PhysReg>(static_cast<int>(PhysReg::A0) + static_cast<int>(index));
}

RegMask callerClobbers() {
    return {PhysReg::Ra, PhysReg::T0, PhysReg::T1, PhysReg::T2, PhysReg::T3, PhysReg::T4,
            PhysReg::T5, PhysReg::T6, PhysReg::A0, PhysReg::A1, PhysReg::A2, PhysReg::A3,
            PhysReg::A4, PhysReg::A5, PhysReg::A6, PhysReg::A7};
}

MInstruction binary(MOpcode opcode, MOperand destination, MOperand left,
                    MOperand right, SourceLocation location) {
    return MInstruction{opcode, {destination}, {left, right}, {}, {}, location};
}

bool isCompare(IROp op) {
    return op >= IROp::ICmpLT && op <= IROp::ICmpNE;
}

MachineFunction lowerFunction(const IRFunction& ir, bool fuseCompareBranches) {
    MachineFunction machine;
    machine.function = ir.function;
    machine.entry = ir.entry;
    machine.location = ir.location;
    machine.vregCount = ir.valueCount;
    machine.blocks.resize(ir.blocks.size());
    for (const auto& block : ir.blocks) {
        machine.blocks[block.id].id = block.id;
        machine.blocks[block.id].predecessors.assign(block.predecessors.begin(), block.predecessors.end());
        machine.blocks[block.id].successors.assign(block.successors.begin(), block.successors.end());
    }
    std::vector<std::vector<std::pair<VirtualReg, VirtualReg>>> edgeCopies(ir.blocks.size());
    std::vector<bool> fused(ir.valueCount, false);
    std::vector<BlockId> definitionBlocks(ir.valueCount, ir.entry);
    for (const auto& block : ir.blocks)
        for (const auto& instruction : block.instructions)
            if (instruction.result) definitionBlocks[*instruction.result] = block.id;
    if (fuseCompareBranches) {
        const auto uses = buildUseLists(ir);
        for (const auto& block : ir.blocks) {
            const auto* branch = std::get_if<BranchValue>(&*block.terminator);
            if (!branch || uses[branch->condition].size() != 1 ||
                !uses[branch->condition][0].terminator ||
                definitionBlocks[branch->condition] != block.id)
                continue;
            const auto* definition = findDefinition(ir, branch->condition);
            if (definition && isCompare(definition->op)) fused[branch->condition] = true;
        }
    }
    for (const auto& block : ir.blocks) for (const auto& instruction : block.instructions) {
        if (instruction.op != IROp::Phi) break;
        for (const auto& input : instruction.phiInputs)
            edgeCopies[input.predecessor].push_back({vreg(*instruction.result), vreg(input.value)});
    }
    auto fresh = [&]() { return vreg(machine.vregCount++); };
    for (const auto& block : ir.blocks) {
        auto& output = machine.blocks[block.id].instructions;
        for (const auto& instruction : block.instructions) {
            if (instruction.op == IROp::Phi) continue;
            if (instruction.result && fused[*instruction.result]) continue;
            const auto destination = [&]() -> MOperand { return vreg(*instruction.result); };
            const auto operand = [&](std::size_t index) -> MOperand { return vreg(instruction.operands[index]); };
            switch (instruction.op) {
            case IROp::Param: {
                const auto index = static_cast<std::uint32_t>(*instruction.immediate);
                if (index < 8)
                    output.push_back({MOpcode::COPY, {destination()}, {argumentRegister(index)}, {}, {}, instruction.location});
                else
                    output.push_back({MOpcode::LW, {destination()}, {StackSlot{StackSlotKind::IncomingArgument, index - 8}}, {}, {}, instruction.location});
                break;
            }
            case IROp::Constant: output.push_back({MOpcode::LI, {destination()}, {Immediate{*instruction.immediate}}, {}, {}, instruction.location}); break;
            case IROp::Copy: output.push_back({MOpcode::COPY, {destination()}, {operand(0)}, {}, {}, instruction.location}); break;
            case IROp::Add: output.push_back(binary(MOpcode::ADD, destination(), operand(0), operand(1), instruction.location)); break;
            case IROp::Sub: output.push_back(binary(MOpcode::SUB, destination(), operand(0), operand(1), instruction.location)); break;
            case IROp::Mul: output.push_back(binary(MOpcode::MUL, destination(), operand(0), operand(1), instruction.location)); break;
            case IROp::SDiv: output.push_back(binary(MOpcode::DIV, destination(), operand(0), operand(1), instruction.location)); break;
            case IROp::SRem: output.push_back(binary(MOpcode::REM, destination(), operand(0), operand(1), instruction.location)); break;
            case IROp::ICmpLT: output.push_back(binary(MOpcode::SLT, destination(), operand(0), operand(1), instruction.location)); break;
            case IROp::ICmpGT: output.push_back(binary(MOpcode::SLT, destination(), operand(1), operand(0), instruction.location)); break;
            case IROp::ICmpLE: case IROp::ICmpGE: {
                const MOperand temporary = fresh();
                const MOperand left = instruction.op == IROp::ICmpLE ? operand(1) : operand(0);
                const MOperand right = instruction.op == IROp::ICmpLE ? operand(0) : operand(1);
                output.push_back(binary(MOpcode::SLT, temporary, left, right, instruction.location));
                output.push_back(binary(MOpcode::XORI, destination(), temporary, Immediate{1}, instruction.location));
                break;
            }
            case IROp::ICmpEQ: case IROp::ICmpNE: {
                const MOperand temporary = fresh();
                output.push_back(binary(MOpcode::XOR, temporary, operand(0), operand(1), instruction.location));
                if (instruction.op == IROp::ICmpEQ)
                    output.push_back(binary(MOpcode::SLTIU, destination(), temporary, Immediate{1}, instruction.location));
                else
                    output.push_back(binary(MOpcode::SLTU, destination(), PhysReg::Zero, temporary, instruction.location));
                break;
            }
            case IROp::LogicalNot: output.push_back(binary(MOpcode::SLTIU, destination(), operand(0), Immediate{1}, instruction.location)); break;
            case IROp::LoadGlobal: {
                const MOperand address = fresh();
                output.push_back({MOpcode::LA, {address}, {GlobalRef{*instruction.global}}, {}, {}, instruction.location});
                output.push_back({MOpcode::LW, {destination()}, {address, Immediate{0}}, {}, {}, instruction.location});
                break;
            }
            case IROp::StoreGlobal: {
                const MOperand address = fresh();
                output.push_back({MOpcode::LA, {address}, {GlobalRef{*instruction.global}}, {}, {}, instruction.location});
                output.push_back({MOpcode::SW, {}, {operand(0), address, Immediate{0}}, {}, {}, instruction.location});
                break;
            }
            case IROp::Call: {
                for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                    if (index < 8) output.push_back({MOpcode::COPY, {argumentRegister(index)}, {operand(index)}, {}, {}, instruction.location});
                    else output.push_back({MOpcode::SW, {}, {operand(index), StackSlot{StackSlotKind::OutgoingArgument, static_cast<std::uint32_t>(index - 8)}}, {}, {}, instruction.location});
                }
                RegMask uses;
                for (std::size_t index = 0; index < std::min<std::size_t>(8, instruction.operands.size()); ++index) uses.insert(argumentRegister(index));
                output.push_back({MOpcode::CALL, {}, {FunctionRef{*instruction.callee}}, callerClobbers(), std::move(uses), instruction.location});
                machine.hasCalls = true;
                if (instruction.result) output.push_back({MOpcode::COPY, {destination()}, {PhysReg::A0}, {}, {}, instruction.location});
                break;
            }
            case IROp::Phi: break;
            }
        }
        if (!edgeCopies[block.id].empty()) {
            MInstruction copies; copies.opcode = MOpcode::PARALLEL_COPY;
            for (const auto& [destination, source] : edgeCopies[block.id]) {
                copies.defs.push_back(destination); copies.uses.push_back(source);
            }
            output.push_back(std::move(copies));
        }
        if (const auto* jump = std::get_if<IRJump>(&*block.terminator))
            output.push_back({MOpcode::JUMP, {}, {MachineBlockRef{jump->target}}, {}, {}, {}});
        else if (const auto* branch = std::get_if<BranchValue>(&*block.terminator)) {
            if (!fused[branch->condition]) {
                output.push_back({MOpcode::BRCOND, {}, {vreg(branch->condition), MachineBlockRef{branch->trueTarget}, MachineBlockRef{branch->falseTarget}}, {}, {}, {}});
            } else {
                const auto* comparison = findDefinition(ir, branch->condition);
                MOpcode opcode = MOpcode::BEQ;
                ValueId lhs = comparison->operands[0];
                ValueId rhs = comparison->operands[1];
                switch (comparison->op) {
                case IROp::ICmpEQ: opcode = MOpcode::BEQ; break;
                case IROp::ICmpNE: opcode = MOpcode::BNE; break;
                case IROp::ICmpLT: opcode = MOpcode::BLT; break;
                case IROp::ICmpGE: opcode = MOpcode::BGE; break;
                case IROp::ICmpGT: opcode = MOpcode::BLT; std::swap(lhs, rhs); break;
                case IROp::ICmpLE: opcode = MOpcode::BGE; std::swap(lhs, rhs); break;
                default: break;
                }
                output.push_back({opcode, {}, {vreg(lhs), vreg(rhs),
                    MachineBlockRef{branch->trueTarget}, MachineBlockRef{branch->falseTarget}},
                    {}, {}, comparison->location});
            }
        }
        else if (const auto* ret = std::get_if<ReturnValue>(&*block.terminator)) {
            if (ret->value) output.push_back({MOpcode::COPY, {PhysReg::A0}, {vreg(*ret->value)}, {}, {}, {}});
        }
    }
    const MBlockId epilogue = static_cast<MBlockId>(machine.blocks.size());
    machine.epilogue = epilogue;
    MachineBlock exit; exit.id = epilogue; exit.instructions.push_back({MOpcode::RET, {}, {}, {}, {}, {}});
    for (const auto& block : ir.blocks) if (std::holds_alternative<ReturnValue>(*block.terminator)) {
        machine.blocks[block.id].instructions.push_back({MOpcode::JUMP, {}, {MachineBlockRef{epilogue}}, {}, {}, {}});
        machine.blocks[block.id].successors = {epilogue};
        exit.predecessors.push_back(block.id);
    }
    machine.blocks.push_back(std::move(exit));
    return machine;
}

} // namespace

MachineModule InstructionSelector::lower(const IRModule& input) const {
    IRModule ir = input;
    splitCriticalEdges(ir);
    MachineModule result;
    (void)semantic_;
    for (const auto& function : ir.functions)
        result.functions.push_back(lowerFunction(function, options_.fuseCompareBranches));
    return result;
}

} // namespace toyc

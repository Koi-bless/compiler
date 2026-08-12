#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <variant>
#include <vector>

#include "toyc/frontend/token.hpp"
#include "toyc/support/ids.hpp"

namespace toyc {

enum class PhysReg {
    Zero, Ra, Sp,
    T0, T1, T2, T3, T4, T5, T6,
    S0, S1, S2, S3, S4, S5, S6, S7, S8, S9, S10, S11,
    A0, A1, A2, A3, A4, A5, A6, A7
};

struct VirtualReg { VRegId id{}; auto operator<=>(const VirtualReg&) const = default; };
struct Immediate { std::int32_t value{}; bool operator==(const Immediate&) const = default; };
enum class StackSlotKind { Spill, IncomingArgument, OutgoingArgument, CalleeSaved, ReturnAddress };
struct StackSlot { StackSlotKind kind = StackSlotKind::Spill; std::uint32_t index{}; auto operator<=>(const StackSlot&) const = default; };
struct GlobalRef { SymbolId id{}; bool operator==(const GlobalRef&) const = default; };
struct FunctionRef { FuncId id{}; bool operator==(const FunctionRef&) const = default; };
struct MachineBlockRef { MBlockId id{}; bool operator==(const MachineBlockRef&) const = default; };
using MOperand = std::variant<VirtualReg, PhysReg, Immediate, StackSlot,
                              GlobalRef, FunctionRef, MachineBlockRef>;
using RegMask = std::set<PhysReg>;

enum class MOpcode {
    LI, LA, COPY,
    ADD, ADDI, SUB, MUL, DIV, REM, SLLI,
    SLT, SLTU, XOR, XORI, SLTIU,
    LW, SW, CALL, BRCOND, BEQ, BNE, BLT, BGE, JUMP, RET, PARALLEL_COPY
};

struct MInstruction {
    MOpcode opcode = MOpcode::LI;
    std::vector<MOperand> defs;
    std::vector<MOperand> uses;
    RegMask implicitDefs;
    RegMask implicitUses;
    SourceLocation location{};
};

enum class MIRStage { PreRegisterAllocation, PostRegisterAllocation, AfterFrameLowering };

struct MachineBlock {
    MBlockId id{};
    std::vector<MInstruction> instructions;
    std::vector<MBlockId> predecessors;
    std::vector<MBlockId> successors;
};

struct MachineFunction {
    FuncId function{};
    MBlockId entry{};
    std::vector<MachineBlock> blocks;
    std::uint32_t vregCount{};
    std::uint32_t spillSlotCount{};
    std::vector<PhysReg> usedCalleeSaved;
    std::optional<MBlockId> epilogue;
    std::int32_t frameSize{};
    bool hasCalls = false;
    MIRStage stage = MIRStage::PreRegisterAllocation;
    SourceLocation location{};
};

struct MachineModule { std::vector<MachineFunction> functions; };

inline VirtualReg vreg(VRegId id) { return VirtualReg{id}; }
const char* physRegName(PhysReg reg);
bool isCalleeSaved(PhysReg reg);
bool isCallerSaved(PhysReg reg);

} // namespace toyc

#pragma once

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "toyc/frontend/ast.hpp"
#include "toyc/frontend/token.hpp"
#include "toyc/support/ids.hpp"

namespace toyc {

enum class IROp {
    Param, Constant, Copy,
    Add, Sub, Mul, SDiv, SRem,
    ICmpLT, ICmpGT, ICmpLE, ICmpGE, ICmpEQ, ICmpNE,
    LogicalNot, Phi, LoadGlobal, StoreGlobal, Call
};

struct PhiInput { BlockId predecessor{}; ValueId value{}; };

struct IRInstruction {
    InstId id{};
    IROp op = IROp::Constant;
    std::optional<ValueId> result;
    std::vector<ValueId> operands;
    std::vector<PhiInput> phiInputs;
    std::optional<std::int32_t> immediate;
    std::optional<SymbolId> global;
    std::optional<FuncId> callee;
    SourceLocation location{};
};

struct IRJump { BlockId target{}; };
struct BranchValue { ValueId condition{}; BlockId trueTarget{}; BlockId falseTarget{}; };
struct ReturnValue { std::optional<ValueId> value; };
struct IRUnreachable {};
using IRTerminator = std::variant<IRJump, BranchValue, ReturnValue, IRUnreachable>;

struct IRBlock {
    BlockId id{};
    std::vector<IRInstruction> instructions;
    std::optional<IRTerminator> terminator;
    std::vector<BlockId> predecessors;
    std::vector<BlockId> successors;
};

struct IRFunction {
    FuncId function{};
    ValueType returnType = ValueType::Void;
    BlockId entry{};
    std::vector<IRBlock> blocks;
    std::uint32_t valueCount{};
    std::uint32_t instructionCount{};
    SourceLocation location{};
};

struct IRModule { std::vector<IRFunction> functions; };

} // namespace toyc

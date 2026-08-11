#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "toyc/frontend/token.hpp"
#include "toyc/support/ids.hpp"

namespace toyc {

enum class TacOp {
    LoadImm, ReadVar, WriteVar,
    Add, Sub, Mul, Div, Rem,
    CmpLT, CmpGT, CmpLE, CmpGE, CmpEQ, CmpNE,
    LogicalNot, Call
};

struct TacInst {
    TacOp op = TacOp::LoadImm;
    std::optional<TempId> dst;
    std::vector<TempId> inputs;
    std::optional<std::int32_t> immediate;
    std::optional<SymbolId> symbol;
    std::optional<FuncId> callee;
    SourceLocation location{};
};

} // namespace toyc

#pragma once

#include <iosfwd>
#include <optional>
#include <variant>
#include <vector>

#include "toyc/frontend/ast.hpp"
#include "toyc/ir/tac.hpp"

namespace toyc {

struct SemanticResult;

struct Jump { BlockId target{}; };
struct Branch { TempId condition{}; BlockId trueTarget{}; BlockId falseTarget{}; };
struct Return { std::optional<TempId> value; };
struct Unreachable {};
using Terminator = std::variant<Jump, Branch, Return, Unreachable>;

struct CFGBlock {
    BlockId id{};
    std::vector<TacInst> instructions;
    std::optional<Terminator> terminator;
    std::vector<BlockId> predecessors;
    std::vector<BlockId> successors;
};

struct CFGFunction {
    FuncId function{};
    ValueType returnType = ValueType::Void;
    BlockId entry{};
    std::vector<CFGBlock> blocks;
    std::vector<SymbolId> localSymbols;
    TempId tempCount{};
    SourceLocation location{};
};

struct CFGModule { std::vector<CFGFunction> functions; };

void printCFG(std::ostream& output, const CFGModule& module,
              const SemanticResult& semantic);

} // namespace toyc

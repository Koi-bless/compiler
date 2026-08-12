#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "toyc/frontend/ast.hpp"
#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/cfg.hpp"

namespace toyc {

class CFGBuilder {
public:
    explicit CFGBuilder(const SemanticResult& semantic) : semantic_(semantic) {}
    CFGModule build(const CompUnit& unit);

private:
    const SemanticResult& semantic_;
    CFGFunction* function_ = nullptr;
    std::optional<BlockId> currentBlock_;
    std::vector<std::pair<BlockId, BlockId>> loopTargets_;
    std::vector<std::optional<TempId>> localTemps_;
    std::vector<const Declaration*> runtimeGlobalInitializers_;

    void buildFunction(const FunctionDecl& function);
    BlockId createBlock();
    TempId createTemp();
    TempId localTemp(SymbolId symbol) const;
    void addInstruction(TacInst instruction);
    void terminate(Terminator terminator);
    std::optional<TempId> emitExpr(const Expr& expression);
    TempId requireValue(const Expr& expression);
    void emitCondition(const Expr& expression, BlockId trueBlock, BlockId falseBlock);
    void emitStmt(const Stmt& statement);
};

} // namespace toyc

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "toyc/frontend/ast.hpp"
#include "toyc/support/diagnostic.hpp"
#include "toyc/support/ids.hpp"

namespace toyc {

struct Symbol {
    SymbolId id{};
    std::string name;
    bool isConst = false;
    bool isGlobal = false;
    std::optional<FuncId> owner;
    std::optional<std::int32_t> constValue;
    std::optional<std::int32_t> initialValue;
    SourceLocation location{};
};

struct FunctionSymbol {
    FuncId id{};
    std::string name;
    ValueType returnType = ValueType::Void;
    std::vector<ValueType> parameterTypes;
    std::vector<SymbolId> parameterSymbols;
    SourceLocation location{};
};

struct SemanticResult {
    std::vector<Symbol> symbols;
    std::vector<FunctionSymbol> functions;
};

class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(DiagnosticEngine& diagnostics) : diagnostics_(diagnostics) {}
    SemanticResult analyze(CompUnit& unit);

private:
    DiagnosticEngine& diagnostics_;
    SemanticResult result_;
    std::vector<std::unordered_map<std::string, SymbolId>> scopes_;
    std::unordered_map<std::string, FuncId> functions_;
    std::unordered_map<std::string, SourceLocation> globalNames_;
    std::optional<FuncId> currentFunction_;
    unsigned loopDepth_ = 0;

    void pushScope();
    void popScope();
    std::optional<SymbolId> lookupValue(const std::string& name) const;
    SymbolId addSymbol(const std::string& name, bool isConst, bool isGlobal,
                       SourceLocation location);
    void analyzeDeclaration(Declaration& declaration, bool isGlobal);
    void analyzeFunction(FunctionDecl& function);
    void analyzeBlock(BlockStmt& block);
    void analyzeStmt(Stmt& statement);
    ValueType analyzeExpr(Expr& expression, bool allowWideLiteral = false);
    void requireInt(Expr& expression, std::string_view context);
    void validateConstantExpression(const Expr& expression) const;
    std::int32_t evaluateConstant(const Expr& expression) const;
};

} // namespace toyc

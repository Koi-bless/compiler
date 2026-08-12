#include "toyc/frontend/semantic.hpp"

#include <bit>
#include <cstdint>
#include <limits>
#include <string>

namespace toyc {
namespace {

std::int32_t bits(std::uint32_t value) { return std::bit_cast<std::int32_t>(value); }
std::uint32_t bits(std::int32_t value) { return std::bit_cast<std::uint32_t>(value); }

} // namespace

void SemanticAnalyzer::pushScope() { scopes_.emplace_back(); }
void SemanticAnalyzer::popScope() { scopes_.pop_back(); }

std::optional<SymbolId> SemanticAnalyzer::lookupValue(const std::string& name) const {
    for (auto iterator = scopes_.rbegin(); iterator != scopes_.rend(); ++iterator) {
        const auto found = iterator->find(name);
        if (found != iterator->end()) return found->second;
    }
    return std::nullopt;
}

SymbolId SemanticAnalyzer::addSymbol(const std::string& name, bool isConst,
                                     bool isGlobal, SourceLocation location) {
    if (scopes_.back().contains(name))
        diagnostics_.fail(location, "semantic", "duplicate declaration of '" + name + "'");
    if (isGlobal && globalNames_.contains(name))
        diagnostics_.fail(location, "semantic", "global name '" + name + "' is already declared");
    const SymbolId id = static_cast<SymbolId>(result_.symbols.size());
    result_.symbols.push_back(Symbol{id, name, isConst, isGlobal, currentFunction_,
                                     std::nullopt, std::nullopt, location});
    scopes_.back().emplace(name, id);
    if (isGlobal) globalNames_.emplace(name, location);
    return id;
}

SemanticResult SemanticAnalyzer::analyze(CompUnit& unit) {
    result_ = {};
    scopes_.clear(); functions_.clear(); globalNames_.clear();
    currentFunction_.reset(); loopDepth_ = 0;
    pushScope();
    for (auto& item : unit.items) {
        if (auto* declaration = dynamic_cast<Declaration*>(item.get())) analyzeDeclaration(*declaration, true);
        else if (auto* function = dynamic_cast<FunctionDecl*>(item.get())) analyzeFunction(*function);
    }
    const auto main = functions_.find("main");
    if (main == functions_.end()) diagnostics_.fail(unit.range.begin, "semantic", "program must define exactly one int main()");
    const auto& mainFunction = result_.functions[main->second];
    if (mainFunction.returnType != ValueType::Int || !mainFunction.parameterTypes.empty())
        diagnostics_.fail(mainFunction.location, "semantic", "main must have signature int main()");
    popScope();
    return std::move(result_);
}

void SemanticAnalyzer::analyzeDeclaration(Declaration& declaration, bool isGlobal) {
    requireInt(*declaration.init, "declaration initializer");
    std::optional<std::int32_t> staticValue;
    if (declaration.isConst) {
        validateConstantExpression(*declaration.init);
        staticValue = evaluateConstant(*declaration.init);
    } else if (isGlobal && isConstantExpression(*declaration.init)) {
        staticValue = evaluateConstant(*declaration.init);
    }
    const SymbolId id = addSymbol(declaration.name, declaration.isConst, isGlobal,
                                  declaration.range.begin);
    declaration.resolvedSymbol = id;
    if (declaration.isConst) result_.symbols[id].constValue = staticValue;
    if (isGlobal) result_.symbols[id].initialValue = staticValue;
}

void SemanticAnalyzer::analyzeFunction(FunctionDecl& function) {
    if (globalNames_.contains(function.name))
        diagnostics_.fail(function.range.begin, "semantic", "global name '" + function.name + "' is already declared");
    const FuncId id = static_cast<FuncId>(result_.functions.size());
    FunctionSymbol symbol{id, function.name, function.returnType, {}, {}, function.range.begin};
    symbol.parameterTypes.assign(function.params.size(), ValueType::Int);
    result_.functions.push_back(std::move(symbol));
    functions_.emplace(function.name, id);
    globalNames_.emplace(function.name, function.range.begin);
    function.resolvedFunc = id;
    currentFunction_ = id;
    pushScope();
    for (auto& parameter : function.params) {
        const SymbolId parameterId = addSymbol(parameter.name, false, false, parameter.range.begin);
        parameter.resolvedSymbol = parameterId;
        result_.functions[id].parameterSymbols.push_back(parameterId);
    }
    analyzeBlock(*function.body);
    popScope();
    currentFunction_.reset();
}

void SemanticAnalyzer::analyzeBlock(BlockStmt& block) {
    pushScope();
    for (auto& item : block.items) analyzeStmt(*item);
    popScope();
}

void SemanticAnalyzer::analyzeStmt(Stmt& statement) {
    if (auto* block = dynamic_cast<BlockStmt*>(&statement)) analyzeBlock(*block);
    else if (auto* expression = dynamic_cast<ExprStmt*>(&statement)) (void)analyzeExpr(*expression->expr);
    else if (auto* assignment = dynamic_cast<AssignStmt*>(&statement)) {
        const auto id = lookupValue(assignment->name);
        if (!id) diagnostics_.fail(assignment->range.begin, "semantic", "use of undeclared name '" + assignment->name + "'");
        if (result_.symbols[*id].isConst) diagnostics_.fail(assignment->range.begin, "semantic", "cannot assign to constant '" + assignment->name + "'");
        assignment->resolvedSymbol = *id;
        requireInt(*assignment->value, "assignment value");
    } else if (auto* declaration = dynamic_cast<DeclStmt*>(&statement)) analyzeDeclaration(*declaration->declaration, false);
    else if (auto* branch = dynamic_cast<IfStmt*>(&statement)) {
        requireInt(*branch->condition, "if condition");
        analyzeStmt(*branch->thenStmt);
        if (branch->elseStmt) analyzeStmt(*branch->elseStmt);
    } else if (auto* loop = dynamic_cast<WhileStmt*>(&statement)) {
        requireInt(*loop->condition, "while condition");
        ++loopDepth_; analyzeStmt(*loop->body); --loopDepth_;
    } else if (dynamic_cast<BreakStmt*>(&statement) || dynamic_cast<ContinueStmt*>(&statement)) {
        if (loopDepth_ == 0) diagnostics_.fail(statement.range.begin, "semantic", "break/continue is only valid inside a loop");
    } else if (auto* ret = dynamic_cast<ReturnStmt*>(&statement)) {
        const auto returnType = result_.functions[*currentFunction_].returnType;
        if (returnType == ValueType::Int) {
            if (!ret->value) diagnostics_.fail(ret->range.begin, "semantic", "int function must return a value");
            requireInt(*ret->value, "return value");
        } else if (ret->value) {
            diagnostics_.fail(ret->range.begin, "semantic", "void function cannot return a value");
        }
    }
}

void SemanticAnalyzer::requireInt(Expr& expression, std::string_view context) {
    if (analyzeExpr(expression) != ValueType::Int)
        diagnostics_.fail(expression.range.begin, "semantic", std::string(context) + " cannot have void type");
}

ValueType SemanticAnalyzer::analyzeExpr(Expr& expression, bool allowWideLiteral) {
    if (auto* integer = dynamic_cast<IntegerExpr*>(&expression)) {
        if (integer->value > std::numeric_limits<std::int32_t>::max() &&
            !(allowWideLiteral && integer->value == std::int64_t{2147483648}))
            diagnostics_.fail(integer->range.begin, "semantic", "integer literal is outside the signed 32-bit range");
        return ValueType::Int;
    }
    if (auto* name = dynamic_cast<NameExpr*>(&expression)) {
        const auto id = lookupValue(name->name);
        if (!id) diagnostics_.fail(name->range.begin, "semantic", "use of undeclared name '" + name->name + "'");
        name->resolvedSymbol = *id;
        return ValueType::Int;
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(&expression)) {
        const bool allowWide = unary->op == UnaryOp::Negate;
        if (analyzeExpr(*unary->operand, allowWide) != ValueType::Int)
            diagnostics_.fail(unary->operand->range.begin, "semantic", "unary operand cannot have void type");
        return ValueType::Int;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(&expression)) {
        requireInt(*binary->lhs, "binary operand");
        requireInt(*binary->rhs, "binary operand");
        return ValueType::Int;
    }
    if (auto* call = dynamic_cast<CallExpr*>(&expression)) {
        const auto found = functions_.find(call->calleeName);
        if (found == functions_.end()) diagnostics_.fail(call->range.begin, "semantic", "call to undefined function '" + call->calleeName + "'");
        call->resolvedFunc = found->second;
        const auto& function = result_.functions[found->second];
        if (call->arguments.size() != function.parameterTypes.size())
            diagnostics_.fail(call->range.begin, "semantic", "wrong number of arguments in call to '" + call->calleeName + "'");
        for (auto& argument : call->arguments) requireInt(*argument, "function argument");
        return function.returnType;
    }
    diagnostics_.fail(expression.range.begin, "internal", "unknown expression node");
}

bool SemanticAnalyzer::isConstantExpression(const Expr& expression) const {
    if (dynamic_cast<const IntegerExpr*>(&expression)) return true;
    if (const auto* name = dynamic_cast<const NameExpr*>(&expression))
        return result_.symbols[*name->resolvedSymbol].isConst;
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression))
        return isConstantExpression(*unary->operand);
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression))
        return isConstantExpression(*binary->lhs) &&
               isConstantExpression(*binary->rhs);
    return false;
}

void SemanticAnalyzer::validateConstantExpression(const Expr& expression) const {
    if (dynamic_cast<const IntegerExpr*>(&expression)) return;
    if (const auto* name = dynamic_cast<const NameExpr*>(&expression)) {
        const auto& symbol = result_.symbols[*name->resolvedSymbol];
        if (!symbol.isConst)
            diagnostics_.fail(name->range.begin, "semantic",
                              "constant initializer depends on non-constant '" + name->name + "'");
        return;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        validateConstantExpression(*unary->operand);
        return;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        validateConstantExpression(*binary->lhs);
        validateConstantExpression(*binary->rhs);
        return;
    }
    diagnostics_.fail(expression.range.begin, "semantic",
                      "initializer is not a compile-time constant expression");
}

std::int32_t SemanticAnalyzer::evaluateConstant(const Expr& expression) const {
    if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expression)) {
        if (integer->value > std::numeric_limits<std::int32_t>::max())
            diagnostics_.fail(integer->range.begin, "semantic", "integer literal is outside the signed 32-bit range");
        return static_cast<std::int32_t>(integer->value);
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expression)) {
        const auto& symbol = result_.symbols[*name->resolvedSymbol];
        if (!symbol.constValue) diagnostics_.fail(name->range.begin, "semantic", "constant initializer depends on non-constant '" + name->name + "'");
        return *symbol.constValue;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        if (unary->op == UnaryOp::Negate) {
            if (const auto* integer = dynamic_cast<const IntegerExpr*>(unary->operand.get()); integer && integer->value == std::int64_t{2147483648})
                return std::numeric_limits<std::int32_t>::min();
        }
        const std::int32_t operand = evaluateConstant(*unary->operand);
        if (unary->op == UnaryOp::Plus) return operand;
        if (unary->op == UnaryOp::LogicalNot) return operand == 0 ? 1 : 0;
        return bits(std::uint32_t{0} - bits(operand));
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        const std::int32_t left = evaluateConstant(*binary->lhs);
        if (binary->op == BinaryOp::LogicalAnd && left == 0) return 0;
        if (binary->op == BinaryOp::LogicalOr && left != 0) return 1;
        const std::int32_t right = evaluateConstant(*binary->rhs);
        switch (binary->op) {
        case BinaryOp::Add: return bits(bits(left) + bits(right));
        case BinaryOp::Sub: return bits(bits(left) - bits(right));
        case BinaryOp::Mul: return bits(bits(left) * bits(right));
        case BinaryOp::Div:
            if (right == 0) diagnostics_.fail(binary->rhs->range.begin, "semantic", "division by zero in constant expression");
            if (left == std::numeric_limits<std::int32_t>::min() && right == -1)
                diagnostics_.fail(binary->range.begin, "semantic", "division overflow in constant expression");
            return static_cast<std::int32_t>(left / right);
        case BinaryOp::Rem:
            if (right == 0) diagnostics_.fail(binary->rhs->range.begin, "semantic", "remainder by zero in constant expression");
            if (left == std::numeric_limits<std::int32_t>::min() && right == -1) return 0;
            return static_cast<std::int32_t>(left % right);
        case BinaryOp::Less: return left < right ? 1 : 0;
        case BinaryOp::Greater: return left > right ? 1 : 0;
        case BinaryOp::LessEqual: return left <= right ? 1 : 0;
        case BinaryOp::GreaterEqual: return left >= right ? 1 : 0;
        case BinaryOp::Equal: return left == right ? 1 : 0;
        case BinaryOp::NotEqual: return left != right ? 1 : 0;
        case BinaryOp::LogicalAnd: return right != 0 ? 1 : 0;
        case BinaryOp::LogicalOr: return right != 0 ? 1 : 0;
        }
    }
    diagnostics_.fail(expression.range.begin, "semantic", "expression is not a compile-time constant");
}

} // namespace toyc

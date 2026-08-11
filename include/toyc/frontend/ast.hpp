#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "toyc/frontend/token.hpp"
#include "toyc/support/ids.hpp"

namespace toyc {

enum class ValueType { Int, Void };
enum class UnaryOp { Plus, Negate, LogicalNot };
enum class BinaryOp {
    Add, Sub, Mul, Div, Rem,
    Less, Greater, LessEqual, GreaterEqual, Equal, NotEqual,
    LogicalAnd, LogicalOr
};

struct Node {
    explicit Node(SourceRange sourceRange) : range(sourceRange) {}
    virtual ~Node() = default;
    SourceRange range;
};

struct Expr : Node { using Node::Node; };
using ExprPtr = std::unique_ptr<Expr>;

struct IntegerExpr final : Expr {
    IntegerExpr(SourceRange range, std::int64_t value) : Expr(range), value(value) {}
    std::int64_t value;
};

struct NameExpr final : Expr {
    NameExpr(SourceRange range, std::string name) : Expr(range), name(std::move(name)) {}
    std::string name;
    std::optional<SymbolId> resolvedSymbol;
};

struct UnaryExpr final : Expr {
    UnaryExpr(SourceRange range, UnaryOp op, ExprPtr operand)
        : Expr(range), op(op), operand(std::move(operand)) {}
    UnaryOp op;
    ExprPtr operand;
};

struct BinaryExpr final : Expr {
    BinaryExpr(SourceRange range, BinaryOp op, ExprPtr lhs, ExprPtr rhs)
        : Expr(range), op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}
    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
};

struct CallExpr final : Expr {
    CallExpr(SourceRange range, std::string calleeName, std::vector<ExprPtr> arguments)
        : Expr(range), calleeName(std::move(calleeName)), arguments(std::move(arguments)) {}
    std::string calleeName;
    std::optional<FuncId> resolvedFunc;
    std::vector<ExprPtr> arguments;
};

struct Stmt : Node { using Node::Node; };
using StmtPtr = std::unique_ptr<Stmt>;

struct TopLevel : Node { using Node::Node; };
using TopLevelPtr = std::unique_ptr<TopLevel>;

struct Declaration final : TopLevel {
    Declaration(SourceRange range, bool isConst, std::string name, ExprPtr init)
        : TopLevel(range), isConst(isConst), name(std::move(name)), init(std::move(init)) {}
    bool isConst;
    std::string name;
    ExprPtr init;
    std::optional<SymbolId> resolvedSymbol;
};

struct ParamDecl final : Node {
    ParamDecl(SourceRange range, std::string name) : Node(range), name(std::move(name)) {}
    std::string name;
    std::optional<SymbolId> resolvedSymbol;
};

struct BlockStmt final : Stmt {
    explicit BlockStmt(SourceRange range) : Stmt(range) {}
    std::vector<StmtPtr> items;
};

struct EmptyStmt final : Stmt { using Stmt::Stmt; };

struct ExprStmt final : Stmt {
    ExprStmt(SourceRange range, ExprPtr expr) : Stmt(range), expr(std::move(expr)) {}
    ExprPtr expr;
};

struct AssignStmt final : Stmt {
    AssignStmt(SourceRange range, std::string name, ExprPtr value)
        : Stmt(range), name(std::move(name)), value(std::move(value)) {}
    std::string name;
    std::optional<SymbolId> resolvedSymbol;
    ExprPtr value;
};

struct DeclStmt final : Stmt {
    DeclStmt(SourceRange range, std::unique_ptr<Declaration> declaration)
        : Stmt(range), declaration(std::move(declaration)) {}
    std::unique_ptr<Declaration> declaration;
};

struct IfStmt final : Stmt {
    IfStmt(SourceRange range, ExprPtr condition, StmtPtr thenStmt, StmtPtr elseStmt)
        : Stmt(range), condition(std::move(condition)), thenStmt(std::move(thenStmt)),
          elseStmt(std::move(elseStmt)) {}
    ExprPtr condition;
    StmtPtr thenStmt;
    StmtPtr elseStmt;
};

struct WhileStmt final : Stmt {
    WhileStmt(SourceRange range, ExprPtr condition, StmtPtr body)
        : Stmt(range), condition(std::move(condition)), body(std::move(body)) {}
    ExprPtr condition;
    StmtPtr body;
};

struct BreakStmt final : Stmt { using Stmt::Stmt; };
struct ContinueStmt final : Stmt { using Stmt::Stmt; };

struct ReturnStmt final : Stmt {
    ReturnStmt(SourceRange range, ExprPtr value) : Stmt(range), value(std::move(value)) {}
    ExprPtr value;
};

struct FunctionDecl final : TopLevel {
    FunctionDecl(SourceRange range, ValueType returnType, std::string name,
                 std::vector<ParamDecl> params, std::unique_ptr<BlockStmt> body)
        : TopLevel(range), returnType(returnType), name(std::move(name)),
          params(std::move(params)), body(std::move(body)) {}
    ValueType returnType;
    std::string name;
    std::vector<ParamDecl> params;
    std::unique_ptr<BlockStmt> body;
    std::optional<FuncId> resolvedFunc;
};

struct CompUnit final : Node {
    explicit CompUnit(SourceRange range) : Node(range) {}
    std::vector<TopLevelPtr> items;
};

} // namespace toyc

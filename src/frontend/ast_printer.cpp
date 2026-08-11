#include "toyc/frontend/ast_printer.hpp"

#include <ostream>
#include <string_view>

namespace toyc {
namespace {

std::string_view typeName(ValueType type) { return type == ValueType::Int ? "int" : "void"; }

std::string_view unaryName(UnaryOp op) {
    switch (op) { case UnaryOp::Plus: return "pos"; case UnaryOp::Negate: return "neg"; case UnaryOp::LogicalNot: return "not"; }
    return "unary";
}

std::string_view binaryName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "add"; case BinaryOp::Sub: return "sub";
    case BinaryOp::Mul: return "mul"; case BinaryOp::Div: return "div";
    case BinaryOp::Rem: return "rem"; case BinaryOp::Less: return "lt";
    case BinaryOp::Greater: return "gt"; case BinaryOp::LessEqual: return "le";
    case BinaryOp::GreaterEqual: return "ge"; case BinaryOp::Equal: return "eq";
    case BinaryOp::NotEqual: return "ne"; case BinaryOp::LogicalAnd: return "and";
    case BinaryOp::LogicalOr: return "or";
    }
    return "binary";
}

void expression(std::ostream& out, const Expr& expr) {
    if (const auto* value = dynamic_cast<const IntegerExpr*>(&expr)) out << value->value;
    else if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) out << name->name;
    else if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        out << '(' << unaryName(unary->op) << ' '; expression(out, *unary->operand); out << ')';
    } else if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        out << '(' << binaryName(binary->op) << ' '; expression(out, *binary->lhs);
        out << ' '; expression(out, *binary->rhs); out << ')';
    } else if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        out << "(call " << call->calleeName;
        for (const auto& argument : call->arguments) { out << ' '; expression(out, *argument); }
        out << ')';
    }
}

void indent(std::ostream& out, unsigned depth) { for (unsigned i = 0; i < depth * 2; ++i) out.put(' '); }

void declaration(std::ostream& out, const Declaration& decl) {
    out << "(decl " << (decl.isConst ? "const " : "var ") << decl.name << ' ';
    expression(out, *decl.init); out << ')';
}

void statement(std::ostream& out, const Stmt& stmt, unsigned depth) {
    indent(out, depth);
    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        out << "(block";
        for (const auto& item : block->items) { out << '\n'; statement(out, *item, depth + 1); }
        out << ')';
    } else if (dynamic_cast<const EmptyStmt*>(&stmt)) out << "(empty)";
    else if (const auto* value = dynamic_cast<const ExprStmt*>(&stmt)) { out << "(expr "; expression(out, *value->expr); out << ')'; }
    else if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) { out << "(assign " << assign->name << ' '; expression(out, *assign->value); out << ')'; }
    else if (const auto* decl = dynamic_cast<const DeclStmt*>(&stmt)) declaration(out, *decl->declaration);
    else if (const auto* branch = dynamic_cast<const IfStmt*>(&stmt)) {
        out << "(if "; expression(out, *branch->condition); out << '\n'; statement(out, *branch->thenStmt, depth + 1);
        if (branch->elseStmt) { out << '\n'; statement(out, *branch->elseStmt, depth + 1); }
        out << ')';
    } else if (const auto* loop = dynamic_cast<const WhileStmt*>(&stmt)) {
        out << "(while "; expression(out, *loop->condition); out << '\n'; statement(out, *loop->body, depth + 1); out << ')';
    } else if (dynamic_cast<const BreakStmt*>(&stmt)) out << "(break)";
    else if (dynamic_cast<const ContinueStmt*>(&stmt)) out << "(continue)";
    else if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) { out << "(return"; if (ret->value) { out << ' '; expression(out, *ret->value); } out << ')'; }
}

} // namespace

void printAst(std::ostream& output, const CompUnit& unit) {
    for (const auto& item : unit.items) {
        if (const auto* decl = dynamic_cast<const Declaration*>(item.get())) {
            declaration(output, *decl); output << '\n';
        } else if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get())) {
            output << "(function " << typeName(function->returnType) << ' ' << function->name << " (";
            for (std::size_t i = 0; i < function->params.size(); ++i) { if (i != 0) output << ' '; output << function->params[i].name; }
            output << ")\n"; statement(output, *function->body, 1); output << ")\n";
        }
    }
}

} // namespace toyc

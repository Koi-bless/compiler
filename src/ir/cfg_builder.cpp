#include "toyc/ir/cfg_builder.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string_view>
#include <utility>

#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

std::int32_t signedBits(std::uint32_t value) { return std::bit_cast<std::int32_t>(value); }
std::uint32_t unsignedBits(std::int32_t value) { return std::bit_cast<std::uint32_t>(value); }

bool isConstantForm(const Expr& expression, const SemanticResult& semantic) {
    if (dynamic_cast<const IntegerExpr*>(&expression)) return true;
    if (const auto* name = dynamic_cast<const NameExpr*>(&expression))
        return semantic.symbols[*name->resolvedSymbol].isConst;
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression))
        return isConstantForm(*unary->operand, semantic);
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression))
        return isConstantForm(*binary->lhs, semantic) &&
               isConstantForm(*binary->rhs, semantic);
    return false;
}

std::optional<std::int32_t> constantValue(const Expr& expression,
                                          const SemanticResult& semantic) {
    if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expression)) {
        if (integer->value > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
        return static_cast<std::int32_t>(integer->value);
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expression))
        return semantic.symbols[*name->resolvedSymbol].constValue;
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        if (unary->op == UnaryOp::Negate) {
            if (const auto* integer = dynamic_cast<const IntegerExpr*>(unary->operand.get());
                integer && integer->value == std::int64_t{2147483648})
                return std::numeric_limits<std::int32_t>::min();
        }
        const auto operand = constantValue(*unary->operand, semantic);
        if (!operand) return std::nullopt;
        if (unary->op == UnaryOp::Plus) return operand;
        if (unary->op == UnaryOp::LogicalNot) return *operand == 0 ? 1 : 0;
        return signedBits(std::uint32_t{0} - unsignedBits(*operand));
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        const auto left = constantValue(*binary->lhs, semantic);
        if (!left) return std::nullopt;
        if (binary->op == BinaryOp::LogicalAnd && *left == 0) return 0;
        if (binary->op == BinaryOp::LogicalOr && *left != 0) return 1;
        const auto right = constantValue(*binary->rhs, semantic);
        if (!right) return std::nullopt;
        switch (binary->op) {
        case BinaryOp::Add: return signedBits(unsignedBits(*left) + unsignedBits(*right));
        case BinaryOp::Sub: return signedBits(unsignedBits(*left) - unsignedBits(*right));
        case BinaryOp::Mul: return signedBits(unsignedBits(*left) * unsignedBits(*right));
        case BinaryOp::Div:
            if (*right == 0 || (*left == std::numeric_limits<std::int32_t>::min() && *right == -1)) return std::nullopt;
            return static_cast<std::int32_t>(*left / *right);
        case BinaryOp::Rem:
            if (*right == 0) return std::nullopt;
            if (*left == std::numeric_limits<std::int32_t>::min() && *right == -1) return 0;
            return static_cast<std::int32_t>(*left % *right);
        case BinaryOp::Less: return *left < *right ? 1 : 0;
        case BinaryOp::Greater: return *left > *right ? 1 : 0;
        case BinaryOp::LessEqual: return *left <= *right ? 1 : 0;
        case BinaryOp::GreaterEqual: return *left >= *right ? 1 : 0;
        case BinaryOp::Equal: return *left == *right ? 1 : 0;
        case BinaryOp::NotEqual: return *left != *right ? 1 : 0;
        case BinaryOp::LogicalAnd: return *right != 0 ? 1 : 0;
        case BinaryOp::LogicalOr: return *right != 0 ? 1 : 0;
        }
    }
    return std::nullopt;
}

TacOp operation(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return TacOp::Add; case BinaryOp::Sub: return TacOp::Sub;
    case BinaryOp::Mul: return TacOp::Mul; case BinaryOp::Div: return TacOp::Div;
    case BinaryOp::Rem: return TacOp::Rem; case BinaryOp::Less: return TacOp::CmpLT;
    case BinaryOp::Greater: return TacOp::CmpGT; case BinaryOp::LessEqual: return TacOp::CmpLE;
    case BinaryOp::GreaterEqual: return TacOp::CmpGE; case BinaryOp::Equal: return TacOp::CmpEQ;
    case BinaryOp::NotEqual: return TacOp::CmpNE;
    case BinaryOp::LogicalAnd: case BinaryOp::LogicalOr: break;
    }
    throw CompileError({}, "internal", "logical operator must be lowered through control flow");
}

std::string_view tacName(TacOp op) {
    switch (op) {
    case TacOp::Param: return "param"; case TacOp::LoadImm: return "imm";
    case TacOp::Copy: return "copy"; case TacOp::Add: return "add";
    case TacOp::Sub: return "sub"; case TacOp::Mul: return "mul";
    case TacOp::Div: return "div"; case TacOp::Rem: return "rem";
    case TacOp::CmpLT: return "lt"; case TacOp::CmpGT: return "gt";
    case TacOp::CmpLE: return "le"; case TacOp::CmpGE: return "ge";
    case TacOp::CmpEQ: return "eq"; case TacOp::CmpNE: return "ne";
    case TacOp::LogicalNot: return "not"; case TacOp::LoadGlobal: return "load_global";
    case TacOp::StoreGlobal: return "store_global"; case TacOp::Call: return "call";
    }
    return "unknown";
}

} // namespace

CFGModule CFGBuilder::build(const CompUnit& unit) {
    CFGModule module;
    runtimeGlobalInitializers_.clear();
    for (const auto& item : unit.items) {
        const auto* declaration = dynamic_cast<const Declaration*>(item.get());
        if (!declaration || declaration->isConst) continue;
        const auto& symbol = semantic_.symbols[*declaration->resolvedSymbol];
        if (!symbol.initialValue) runtimeGlobalInitializers_.push_back(declaration);
    }
    for (const auto& item : unit.items) {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get())) {
            module.functions.push_back({});
            function_ = &module.functions.back();
            buildFunction(*function);
        }
    }
    function_ = nullptr;
    return module;
}

void CFGBuilder::buildFunction(const FunctionDecl& function) {
    function_->function = *function.resolvedFunc;
    function_->returnType = function.returnType;
    function_->location = function.range.begin;
    function_->entry = createBlock();
    currentBlock_ = function_->entry;
    function_->localSymbols.clear();
    localTemps_.assign(semantic_.symbols.size(), std::nullopt);
    for (const auto& symbol : semantic_.symbols) {
        if (symbol.owner && *symbol.owner == function_->function) {
            function_->localSymbols.push_back(symbol.id);
            if (!symbol.isConst) localTemps_[symbol.id] = createTemp();
        }
    }
    const auto& parameters = semantic_.functions[function_->function].parameterSymbols;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        addInstruction(TacInst{TacOp::Param, localTemp(parameters[index]), {},
                               static_cast<std::int32_t>(index), {}, {}, function.range.begin});
    }
    if (function.name == "main") {
        for (const Declaration* declaration : runtimeGlobalInitializers_) {
            const TempId value = requireValue(*declaration->init);
            addInstruction(TacInst{TacOp::StoreGlobal, {}, {value}, {},
                                   *declaration->resolvedSymbol, {},
                                   declaration->range.begin});
        }
    }
    emitStmt(*function.body);
    if (currentBlock_) {
        if (function.returnType == ValueType::Int)
            throw CompileError(function.range.begin, "semantic", "int function has a reachable path without a return value");
        terminate(Return{std::nullopt});
    }
}

BlockId CFGBuilder::createBlock() {
    const BlockId id = static_cast<BlockId>(function_->blocks.size());
    CFGBlock block;
    block.id = id;
    function_->blocks.push_back(std::move(block));
    return id;
}

TempId CFGBuilder::createTemp() { return function_->tempCount++; }

TempId CFGBuilder::localTemp(SymbolId symbol) const {
    if (symbol >= localTemps_.size() || !localTemps_[symbol])
        throw CompileError({}, "internal", "local symbol has no CFG temporary");
    return *localTemps_[symbol];
}

void CFGBuilder::addInstruction(TacInst instruction) {
    if (!currentBlock_) throw CompileError(instruction.location, "internal", "instruction emitted without an active block");
    auto& block = function_->blocks[*currentBlock_];
    if (block.terminator) throw CompileError(instruction.location, "internal", "instruction emitted after terminator");
    block.instructions.push_back(std::move(instruction));
}

void CFGBuilder::terminate(Terminator terminator) {
    if (!currentBlock_) throw CompileError({}, "internal", "terminator emitted without an active block");
    auto& block = function_->blocks[*currentBlock_];
    std::vector<BlockId> targets;
    if (const auto* jump = std::get_if<Jump>(&terminator)) targets.push_back(jump->target);
    else if (const auto* branch = std::get_if<Branch>(&terminator)) {
        targets.push_back(branch->trueTarget);
        if (branch->falseTarget != branch->trueTarget) targets.push_back(branch->falseTarget);
    }
    block.terminator = std::move(terminator);
    block.successors = targets;
    for (const BlockId target : targets) {
        auto& predecessors = function_->blocks[target].predecessors;
        if (std::find(predecessors.begin(), predecessors.end(), block.id) == predecessors.end())
            predecessors.push_back(block.id);
    }
    currentBlock_.reset();
}

TempId CFGBuilder::requireValue(const Expr& expression) {
    const auto result = emitExpr(expression);
    if (!result) throw CompileError(expression.range.begin, "internal", "void expression used as a value after semantic analysis");
    return *result;
}

std::optional<TempId> CFGBuilder::emitExpr(const Expr& expression) {
    if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expression)) {
        const TempId dst = createTemp();
        addInstruction(TacInst{TacOp::LoadImm, dst, {}, static_cast<std::int32_t>(integer->value), {}, {}, expression.range.begin});
        return dst;
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expression)) {
        const auto& symbol = semantic_.symbols[*name->resolvedSymbol];
        if (!symbol.isConst && !symbol.isGlobal) return localTemp(symbol.id);
        const TempId dst = createTemp();
        if (symbol.isConst)
            addInstruction(TacInst{TacOp::LoadImm, dst, {}, symbol.constValue, {}, {}, expression.range.begin});
        else
            addInstruction(TacInst{TacOp::LoadGlobal, dst, {}, {}, symbol.id, {}, expression.range.begin});
        return dst;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        if (unary->op == UnaryOp::Negate) {
            if (const auto* integer = dynamic_cast<const IntegerExpr*>(unary->operand.get()); integer && integer->value == std::int64_t{2147483648}) {
                const TempId dst = createTemp();
                addInstruction(TacInst{TacOp::LoadImm, dst, {}, std::numeric_limits<std::int32_t>::min(), {}, {}, expression.range.begin});
                return dst;
            }
        }
        const TempId operand = requireValue(*unary->operand);
        if (unary->op == UnaryOp::Plus) return operand;
        const TempId dst = createTemp();
        if (unary->op == UnaryOp::LogicalNot)
            addInstruction(TacInst{TacOp::LogicalNot, dst, {operand}, {}, {}, {}, expression.range.begin});
        else {
            const TempId zero = createTemp();
            addInstruction(TacInst{TacOp::LoadImm, zero, {}, 0, {}, {}, expression.range.begin});
            addInstruction(TacInst{TacOp::Sub, dst, {zero, operand}, {}, {}, {}, expression.range.begin});
        }
        return dst;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        if (binary->op == BinaryOp::LogicalAnd || binary->op == BinaryOp::LogicalOr) {
            const TempId result = createTemp();
            const BlockId trueBlock = createBlock();
            const BlockId falseBlock = createBlock();
            const BlockId mergeBlock = createBlock();
            emitCondition(expression, trueBlock, falseBlock);
            currentBlock_ = trueBlock;
            const TempId one = createTemp();
            addInstruction(TacInst{TacOp::LoadImm, one, {}, 1, {}, {}, expression.range.begin});
            addInstruction(TacInst{TacOp::Copy, result, {one}, {}, {}, {}, expression.range.begin});
            terminate(Jump{mergeBlock});
            currentBlock_ = falseBlock;
            const TempId zero = createTemp();
            addInstruction(TacInst{TacOp::LoadImm, zero, {}, 0, {}, {}, expression.range.begin});
            addInstruction(TacInst{TacOp::Copy, result, {zero}, {}, {}, {}, expression.range.begin});
            terminate(Jump{mergeBlock});
            currentBlock_ = mergeBlock;
            return result;
        }
        const TempId lhs = requireValue(*binary->lhs);
        const TempId rhs = requireValue(*binary->rhs);
        const TempId dst = createTemp();
        addInstruction(TacInst{operation(binary->op), dst, {lhs, rhs}, {}, {}, {}, expression.range.begin});
        return dst;
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
        std::vector<TempId> arguments;
        arguments.reserve(call->arguments.size());
        for (const auto& argument : call->arguments) arguments.push_back(requireValue(*argument));
        std::optional<TempId> dst;
        if (semantic_.functions[*call->resolvedFunc].returnType == ValueType::Int) dst = createTemp();
        addInstruction(TacInst{TacOp::Call, dst, std::move(arguments), {}, {}, call->resolvedFunc, expression.range.begin});
        return dst;
    }
    throw CompileError(expression.range.begin, "internal", "unknown expression during CFG lowering");
}

void CFGBuilder::emitCondition(const Expr& expression, BlockId trueBlock, BlockId falseBlock) {
    if (isConstantForm(expression, semantic_)) {
        if (const auto value = constantValue(expression, semantic_)) {
            terminate(Jump{*value != 0 ? trueBlock : falseBlock});
            return;
        }
    }
    if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expression)) {
        terminate(Jump{integer->value != 0 ? trueBlock : falseBlock});
        return;
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expression)) {
        const auto& symbol = semantic_.symbols[*name->resolvedSymbol];
        if (symbol.constValue) { terminate(Jump{*symbol.constValue != 0 ? trueBlock : falseBlock}); return; }
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression); unary && unary->op == UnaryOp::LogicalNot) {
        emitCondition(*unary->operand, falseBlock, trueBlock); return;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        if (binary->op == BinaryOp::LogicalAnd) {
            const BlockId rhs = createBlock();
            emitCondition(*binary->lhs, rhs, falseBlock);
            currentBlock_ = function_->blocks[rhs].predecessors.empty()
                                ? std::optional<BlockId>{}
                                : std::optional<BlockId>{rhs};
            if (currentBlock_) emitCondition(*binary->rhs, trueBlock, falseBlock);
            return;
        }
        if (binary->op == BinaryOp::LogicalOr) {
            const BlockId rhs = createBlock();
            emitCondition(*binary->lhs, trueBlock, rhs);
            currentBlock_ = function_->blocks[rhs].predecessors.empty()
                                ? std::optional<BlockId>{}
                                : std::optional<BlockId>{rhs};
            if (currentBlock_) emitCondition(*binary->rhs, trueBlock, falseBlock);
            return;
        }
    }
    terminate(Branch{requireValue(expression), trueBlock, falseBlock});
}

void CFGBuilder::emitStmt(const Stmt& statement) {
    if (!currentBlock_) return;
    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
        for (const auto& item : block->items) emitStmt(*item);
    } else if (const auto* expression = dynamic_cast<const ExprStmt*>(&statement)) {
        (void)emitExpr(*expression->expr);
    } else if (const auto* assignment = dynamic_cast<const AssignStmt*>(&statement)) {
        const TempId value = requireValue(*assignment->value);
        const auto& symbol = semantic_.symbols[*assignment->resolvedSymbol];
        if (symbol.isGlobal)
            addInstruction(TacInst{TacOp::StoreGlobal, {}, {value}, {}, symbol.id, {}, statement.range.begin});
        else
            addInstruction(TacInst{TacOp::Copy, localTemp(symbol.id), {value}, {}, {}, {}, statement.range.begin});
    } else if (const auto* declaration = dynamic_cast<const DeclStmt*>(&statement)) {
        if (!declaration->declaration->isConst) {
            const TempId value = requireValue(*declaration->declaration->init);
            const auto& symbol = semantic_.symbols[*declaration->declaration->resolvedSymbol];
            if (symbol.isGlobal)
                addInstruction(TacInst{TacOp::StoreGlobal, {}, {value}, {}, symbol.id, {}, statement.range.begin});
            else
                addInstruction(TacInst{TacOp::Copy, localTemp(symbol.id), {value}, {}, {}, {}, statement.range.begin});
        }
    } else if (const auto* branch = dynamic_cast<const IfStmt*>(&statement)) {
        const BlockId thenBlock = createBlock();
        const BlockId elseBlock = createBlock();
        const BlockId mergeBlock = createBlock();
        emitCondition(*branch->condition, thenBlock, elseBlock);
        currentBlock_ = function_->blocks[thenBlock].predecessors.empty()
                            ? std::optional<BlockId>{}
                            : std::optional<BlockId>{thenBlock};
        emitStmt(*branch->thenStmt);
        if (currentBlock_) terminate(Jump{mergeBlock});
        currentBlock_ = function_->blocks[elseBlock].predecessors.empty()
                            ? std::optional<BlockId>{}
                            : std::optional<BlockId>{elseBlock};
        if (branch->elseStmt) emitStmt(*branch->elseStmt);
        if (currentBlock_) terminate(Jump{mergeBlock});
        if (function_->blocks[mergeBlock].predecessors.empty()) currentBlock_.reset();
        else currentBlock_ = mergeBlock;
    } else if (const auto* loop = dynamic_cast<const WhileStmt*>(&statement)) {
        const BlockId condition = createBlock();
        const BlockId body = createBlock();
        const BlockId exit = createBlock();
        terminate(Jump{condition});
        currentBlock_ = condition; emitCondition(*loop->condition, body, exit);
        loopTargets_.emplace_back(condition, exit);
        currentBlock_ = body; emitStmt(*loop->body);
        if (currentBlock_) terminate(Jump{condition});
        loopTargets_.pop_back();
        currentBlock_ = function_->blocks[exit].predecessors.empty() ? std::optional<BlockId>{} : std::optional<BlockId>{exit};
    } else if (dynamic_cast<const BreakStmt*>(&statement)) {
        terminate(Jump{loopTargets_.back().second});
    } else if (dynamic_cast<const ContinueStmt*>(&statement)) {
        terminate(Jump{loopTargets_.back().first});
    } else if (const auto* ret = dynamic_cast<const ReturnStmt*>(&statement)) {
        std::optional<TempId> value;
        if (ret->value) value = requireValue(*ret->value);
        terminate(Return{value});
    }
}

void printCFG(std::ostream& output, const CFGModule& module, const SemanticResult& semantic) {
    for (const auto& function : module.functions) {
        output << "function @" << semantic.functions[function.function].name << ":\n";
        for (const auto& block : function.blocks) {
            output << "bb" << block.id << ": ; preds = [";
            for (std::size_t i = 0; i < block.predecessors.size(); ++i) { if (i != 0) output << ", "; output << "bb" << block.predecessors[i]; }
            output << "]\n";
            for (const auto& instruction : block.instructions) {
                output << "  "; if (instruction.dst) output << '%' << *instruction.dst << " = ";
                output << tacName(instruction.op);
                if (instruction.immediate.has_value()) output << ' ' << *instruction.immediate;
                if (instruction.symbol.has_value()) output << " $" << *instruction.symbol;
                if (instruction.callee.has_value()) output << " @" << semantic.functions[*instruction.callee].name;
                for (const TempId input : instruction.inputs) output << " %" << input;
                output << '\n';
            }
            if (!block.terminator) { output << "  <no terminator>\n"; continue; }
            output << "  ";
            if (const auto* jump = std::get_if<Jump>(&*block.terminator)) output << "jump bb" << jump->target;
            else if (const auto* branch = std::get_if<Branch>(&*block.terminator)) output << "branch %" << branch->condition << ", bb" << branch->trueTarget << ", bb" << branch->falseTarget;
            else if (const auto* ret = std::get_if<Return>(&*block.terminator)) { output << "return"; if (ret->value) output << " %" << *ret->value; }
            else output << "unreachable";
            output << '\n';
        }
    }
}

} // namespace toyc

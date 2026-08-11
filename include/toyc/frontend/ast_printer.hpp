#pragma once

#include <iosfwd>

#include "toyc/frontend/ast.hpp"

namespace toyc {

void printAst(std::ostream& output, const CompUnit& unit);

} // namespace toyc

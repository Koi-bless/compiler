#pragma once

#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/cfg.hpp"
#include "toyc/ir/ir.hpp"

namespace toyc {

class SSABuilder {
public:
    explicit SSABuilder(const SemanticResult& semantic) : semantic_(semantic) {}
    IRModule build(const CFGModule& cfg) const;

private:
    const SemanticResult& semantic_;
};

} // namespace toyc

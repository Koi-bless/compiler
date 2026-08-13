#pragma once

#include <set>
#include <vector>

#include "toyc/ir/ir.hpp"

namespace toyc {

// Conservative, module-local summary used to reason about calls.  A function
// is only considered removable when it writes no global state, cannot trap,
// and is proven to return.
struct FunctionEffects {
    std::set<SymbolId> reads;
    std::set<SymbolId> writes;
    bool mayTrap = false;
    bool mayNotReturn = false;
    bool recursive = false;

    bool readNone() const { return reads.empty() && writes.empty(); }
    bool readOnly() const { return writes.empty(); }
    bool removableCall() const {
        return writes.empty() && !mayTrap && !mayNotReturn;
    }
};

struct FunctionEffectAnalysis {
    std::vector<FunctionEffects> functions;

    const FunctionEffects* lookup(FuncId function) const {
        return function < functions.size() ? &functions[function] : nullptr;
    }
};

FunctionEffectAnalysis analyzeFunctionEffects(const IRModule& module);

} // namespace toyc

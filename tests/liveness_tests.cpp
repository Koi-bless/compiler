#include "test_support.hpp"
#include "toyc/backend/liveness.hpp"

int main() {
    TestPipeline pipeline("int id(int x){return x;}int main(){int a=3;int b=id(4);return a+b;}");
    const auto& function = pipeline.machine.functions[1];
    const auto liveness = toyc::computeLiveness(function);
    bool crossesCall = false;
    for (const auto& interval : liveness.intervals) crossesCall = crossesCall || interval.crossesCall;
    check(crossesCall, "value live across call was not identified");
    check(liveness.blocks.size() == function.blocks.size(), "block liveness result has wrong size");
}

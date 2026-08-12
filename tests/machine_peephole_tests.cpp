#include <algorithm>

#include "test_support.hpp"
#include "toyc/backend/machine_peephole.hpp"

namespace {

std::size_t countOpcode(const toyc::MachineFunction& function, toyc::MOpcode opcode) {
    return static_cast<std::size_t>(std::count_if(
        function.blocks[0].instructions.begin(), function.blocks[0].instructions.end(),
        [&](const auto& instruction) { return instruction.opcode == opcode; }));
}

toyc::MachineFunction postRA(std::vector<toyc::MInstruction> instructions) {
    toyc::MachineFunction function;
    function.stage = toyc::MIRStage::PostRegisterAllocation;
    function.spillSlotCount = 2;
    function.blocks.resize(1);
    function.blocks[0].id = 0;
    function.blocks[0].instructions = std::move(instructions);
    return function;
}

} // namespace

int main() {
    const toyc::StackSlot spill0{toyc::StackSlotKind::Spill, 0};
    auto forwarding = postRA({
        {toyc::MOpcode::SW, {}, {toyc::PhysReg::T0, spill0}, {}, {}, {}},
        {toyc::MOpcode::LW, {toyc::PhysReg::T1}, {spill0}, {}, {}, {}},
        {toyc::MOpcode::COPY, {toyc::PhysReg::A0}, {toyc::PhysReg::T1}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    });
    const auto forwardResult = toyc::runPostRAPeephole(forwarding);
    toyc::verifyMIR(forwarding, toyc::MIRStage::PostRegisterAllocation);
    check(forwardResult.loadsForwarded == 1 && countOpcode(forwarding, toyc::MOpcode::LW) == 0,
          "spill load was not forwarded");
    check(!toyc::runPostRAPeephole(forwarding).changed,
          "post-RA peephole is not idempotent");

    auto stores = postRA({
        {toyc::MOpcode::SW, {}, {toyc::PhysReg::T0, spill0}, {}, {}, {}},
        {toyc::MOpcode::SW, {}, {toyc::PhysReg::T1, spill0}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    });
    const auto storeResult = toyc::runPostRAPeephole(stores);
    check(storeResult.storesRemoved == 1 && countOpcode(stores, toyc::MOpcode::SW) == 1,
          "overwritten local spill store was not removed");

    auto overwritten = postRA({
        {toyc::MOpcode::LI, {toyc::PhysReg::T0}, {toyc::Immediate{1}}, {}, {}, {}},
        {toyc::MOpcode::LI, {toyc::PhysReg::T0}, {toyc::Immediate{2}}, {}, {}, {}},
        {toyc::MOpcode::COPY, {toyc::PhysReg::A0}, {toyc::PhysReg::T0}, {}, {}, {}},
        {toyc::MOpcode::COPY, {toyc::PhysReg::A0}, {toyc::PhysReg::A0}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    });
    toyc::runPostRAPeephole(overwritten);
    check(countOpcode(overwritten, toyc::MOpcode::LI) == 1,
          "unused overwritten physical definition remains");
    check(countOpcode(overwritten, toyc::MOpcode::COPY) == 1,
          "physical self copy remains");

    const toyc::StackSlot outgoing{toyc::StackSlotKind::OutgoingArgument, 0};
    auto outgoingStore = postRA({
        {toyc::MOpcode::SW, {}, {toyc::PhysReg::T0, outgoing}, {}, {}, {}},
        {toyc::MOpcode::SW, {}, {toyc::PhysReg::T1, outgoing}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    });
    toyc::runPostRAPeephole(outgoingStore);
    check(countOpcode(outgoingStore, toyc::MOpcode::SW) == 2,
          "outgoing argument store was treated as a dead spill store");

    auto redefinedSource = postRA({
        {toyc::MOpcode::SW, {}, {toyc::PhysReg::T0, spill0}, {}, {}, {}},
        {toyc::MOpcode::LI, {toyc::PhysReg::T0}, {toyc::Immediate{9}}, {}, {}, {}},
        {toyc::MOpcode::LW, {toyc::PhysReg::T1}, {spill0}, {}, {}, {}},
        {toyc::MOpcode::COPY, {toyc::PhysReg::A0}, {toyc::PhysReg::T1}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    });
    toyc::runPostRAPeephole(redefinedSource);
    check(countOpcode(redefinedSource, toyc::MOpcode::LW) == 1,
          "spill load was forwarded after its source register was redefined");

    auto ordinaryStoreBarrier = postRA({
        {toyc::MOpcode::SW, {}, {toyc::PhysReg::T0, spill0}, {}, {}, {}},
        {toyc::MOpcode::SW, {}, {toyc::PhysReg::T2, toyc::PhysReg::T3,
                                 toyc::Immediate{0}}, {}, {}, {}},
        {toyc::MOpcode::LW, {toyc::PhysReg::T1}, {spill0}, {}, {}, {}},
        {toyc::MOpcode::COPY, {toyc::PhysReg::A0}, {toyc::PhysReg::T1}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    });
    toyc::runPostRAPeephole(ordinaryStoreBarrier);
    check(countOpcode(ordinaryStoreBarrier, toyc::MOpcode::LW) == 1,
          "spill load was forwarded across an unknown store barrier");

    auto liveOut = postRA({
        {toyc::MOpcode::LI, {toyc::PhysReg::T0}, {toyc::Immediate{7}}, {}, {}, {}},
        {toyc::MOpcode::RET, {}, {}, {}, {}, {}}
    });
    toyc::runPostRAPeephole(liveOut);
    check(countOpcode(liveOut, toyc::MOpcode::LI) == 1,
          "block-tail physical definition was removed without live-out proof");

    auto wrongStage = forwarding;
    wrongStage.stage = toyc::MIRStage::AfterFrameLowering;
    expectCompileError([&] { toyc::runPostRAPeephole(wrongStage); }, "requires post-RA");
}

#include "toyc/backend/mir.hpp"

namespace toyc {

const char* physRegName(PhysReg reg) {
    switch (reg) {
    case PhysReg::Zero: return "zero"; case PhysReg::Ra: return "ra"; case PhysReg::Sp: return "sp";
    case PhysReg::T0: return "t0"; case PhysReg::T1: return "t1"; case PhysReg::T2: return "t2";
    case PhysReg::T3: return "t3"; case PhysReg::T4: return "t4"; case PhysReg::T5: return "t5"; case PhysReg::T6: return "t6";
    case PhysReg::S0: return "s0"; case PhysReg::S1: return "s1"; case PhysReg::S2: return "s2";
    case PhysReg::S3: return "s3"; case PhysReg::S4: return "s4"; case PhysReg::S5: return "s5";
    case PhysReg::S6: return "s6"; case PhysReg::S7: return "s7"; case PhysReg::S8: return "s8";
    case PhysReg::S9: return "s9"; case PhysReg::S10: return "s10"; case PhysReg::S11: return "s11";
    case PhysReg::A0: return "a0"; case PhysReg::A1: return "a1"; case PhysReg::A2: return "a2";
    case PhysReg::A3: return "a3"; case PhysReg::A4: return "a4"; case PhysReg::A5: return "a5";
    case PhysReg::A6: return "a6"; case PhysReg::A7: return "a7";
    }
    return "?";
}

bool isCalleeSaved(PhysReg reg) { return reg >= PhysReg::S1 && reg <= PhysReg::S11; }
bool isCallerSaved(PhysReg reg) {
    return (reg >= PhysReg::T0 && reg <= PhysReg::T6) ||
           (reg >= PhysReg::A0 && reg <= PhysReg::A7) || reg == PhysReg::Ra;
}

} // namespace toyc

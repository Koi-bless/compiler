#pragma once
#include "toyc/opt/pass.hpp"
namespace toyc { PassResult runDCE(IRFunction& function, bool preserveMayTrap = true); }

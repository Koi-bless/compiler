#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "toyc/frontend/token.hpp"

namespace toyc {

class CompileError final : public std::runtime_error {
public:
    CompileError(SourceLocation location, std::string_view category,
                 std::string_view message);
    [[nodiscard]] SourceLocation location() const noexcept { return location_; }
    [[nodiscard]] const std::string& category() const noexcept { return category_; }

private:
    SourceLocation location_;
    std::string category_;
};

class DiagnosticEngine {
public:
    [[noreturn]] void fail(SourceLocation location, std::string_view category,
                           std::string_view message) const;
};

} // namespace toyc

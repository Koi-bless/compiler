#include "toyc/support/diagnostic.hpp"

#include <string>

namespace toyc {
namespace {

std::string formatDiagnostic(SourceLocation location, std::string_view category,
                             std::string_view message) {
    return std::to_string(location.line) + ":" +
           std::to_string(location.column) + ": " + std::string(category) +
           " error: " + std::string(message);
}

} // namespace

CompileError::CompileError(SourceLocation location, std::string_view category,
                           std::string_view message)
    : std::runtime_error(formatDiagnostic(location, category, message)),
      location_(location), category_(category) {}

[[noreturn]] void DiagnosticEngine::fail(SourceLocation location,
                                          std::string_view category,
                                          std::string_view message) const {
    throw CompileError(location, category, message);
}

} // namespace toyc

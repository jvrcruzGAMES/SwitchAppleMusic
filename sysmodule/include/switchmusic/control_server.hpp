#pragma once
#include <switch.h>
#include <functional>
#include <string>
#include <string_view>
namespace smusic {
Result createControlPort(Handle *port);
// The caller owns the registered service port. Returns only on a fatal kernel error.
Result runControlServer(Handle port, const std::function<std::string(std::string_view)> &handler);
} // namespace smusic

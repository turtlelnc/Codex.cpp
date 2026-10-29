#pragma once
#include <string_view>
// Immutable bytes compiled into the executable. Empty means no such resource.
std::string_view codex_resource(std::string_view name);

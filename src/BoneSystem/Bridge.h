#pragma once
#include <reframework/API.h>

namespace lifetimeguard::bonesystem {
// A startup companion for the independent release and the WMP package without WMP.
void bind(const REFrameworkPluginInitializeParam *param);
void start(lua_State *main_state, const char *bootstrap_path, bool wmp_fallback);
} // namespace lifetimeguard::bonesystem

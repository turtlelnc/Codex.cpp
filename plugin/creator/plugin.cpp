#define PLUGIN_BUILD_MODULE
#include "native.h"
#include <cstring>

static int status(void*, const char*, size_t, const PluginCallV1* call) {
  const char* result = "{\"exit_code\":0,\"output\":\"@PLUGIN_ID@ is ready. Replace this starter with the requested feature.\"}";
  call->emit(call->emit_context, "result", result, std::strlen(result));
  return 0;
}
extern "C" PLUGIN_EXPORT int codex_plugin_init_v1(const PluginHostV1* host, PluginModuleV1* module) {
  if (!host || host->version != 1 || host->size < sizeof(PluginHostV1)) return 1;
  module->version=1; module->size=sizeof(PluginModuleV1); module->id="@PLUGIN_ID@";
  return host->bind(host->context, "tools.@PLUGIN_TOOL@", "", status, nullptr);
}

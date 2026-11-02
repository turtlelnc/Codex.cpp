#define PENGUIN_BUILD_MODULE
#include "native.h"
#include <cstring>

static int status(void*, const char*, size_t, const PenguinCallV1* call) {
  const char* result = "{\"exit_code\":0,\"output\":\"@PENGUIN_ID@ is ready. Replace this starter with the requested feature.\"}";
  call->emit(call->emit_context, "result", result, std::strlen(result));
  return 0;
}
extern "C" PENGUIN_EXPORT int codex_penguin_init_v1(const PenguinHostV1* host, PenguinModuleV1* module) {
  if (!host || host->version != 1 || host->size < sizeof(PenguinHostV1)) return 1;
  module->version=1; module->size=sizeof(PenguinModuleV1); module->id="@PENGUIN_ID@";
  return host->bind(host->context, "tools.@PENGUIN_TOOL@", "", status, nullptr);
}

// Explicitly trusted C ABI module. Only declarative codecs are returned: no
// request credentials, cookies, approvals or model data are given to this hook.
#define PLUGIN_BUILD_MODULE
#include "native.h"
#include <cstring>
static int catalog(void*,const char*,size_t,const PluginCallV1* call){
#ifdef CODEX_PROTOCOL_FIXTURE
  const char* json=R"({"adapters":[{"wire_version":0,"canonical_version":1,"profile":"codex-http-20261102","client":true,"server":true,"full_snapshots":true},{"wire_version":7,"canonical_version":1,"profile":"test-wire-seven","client":true,"server":true,"full_snapshots":true,"request_renames":{"message":"text"},"response_renames":{"timeline":"history"}}]})";
#elif defined(CODEX_PROTOCOL_BAD_FIXTURE)
  const char* json=R"({"adapters":[{"wire_version":8,"canonical_version":1,"profile":"unsafe-binding","client":true,"server":true,"full_snapshots":true,"request_renames":{"permit":"allow"}}]})";
#else
  const char* json=R"({"adapters":[{"wire_version":0,"canonical_version":1,"profile":"codex-http-20261102","client":true,"server":true,"full_snapshots":true}]})";
#endif
  call->emit(call->emit_context,"result",json,std::strlen(json));return 0;
}
extern "C" PLUGIN_EXPORT int codex_plugin_init_v1(const PluginHostV1* host,PluginModuleV1* module){
  if(!host||host->version!=1||host->size<sizeof(PluginHostV1)||!module)return 1;
  module->version=1;module->size=sizeof(PluginModuleV1);
#ifdef CODEX_PROTOCOL_FIXTURE
  module->id="team-protocol-fixture";
#elif defined(CODEX_PROTOCOL_BAD_FIXTURE)
  module->id="team-protocol-bad-fixture";
#else
  module->id="team-protocol-compat";
#endif
  return host->bind(host->context,"team.protocol.adapters","builtin.protocol",catalog,nullptr);
}

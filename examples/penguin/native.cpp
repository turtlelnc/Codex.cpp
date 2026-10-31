// A C++17 plugin with no host/compiler C++ ABI dependency.
#define PENGUIN_BUILD_MODULE
#include "../../penguin/native.h"
#include <cstring>
#include <string>

static void result(const PenguinCallV1* call, const char* value) {
  call->emit(call->emit_context, "result", value, std::strlen(value));
}
static int model(void*, const char* input, size_t length, const PenguinCallV1* call) {
  if (call->poll && call->poll(call->emit_context)) return 1;
  const std::string request(input, length);
  if (request.find("run_penguin_tool") != std::string::npos && request.find("Native Penguin tool is active.") == std::string::npos) {
    result(call, R"({"id":"penguin-demo","text":null,"calls":[{"name":"penguin_status","call_id":"penguin-demo-tool","arguments":"{}"}]})");
    return 0;
  }
  const char* text = "### Penguin\nThis reply came from a native module.";
  call->emit(call->emit_context, "text", text, std::strlen(text));
  result(call, R"({"id":"penguin-demo","text":"### Penguin\nThis reply came from a native module.","calls":[]})");
  return 0;
}
static int catalog(void*, const char*, size_t, const PenguinCallV1* call) {
  result(call, R"([{"id":"penguin-demo","description":"Offline native module","default_effort":"","efforts":[]}])");
  return 0;
}
static int tool(void*, const char*, size_t, const PenguinCallV1* call) {
  result(call, R"({"exit_code":0,"output":"Native Penguin tool is active."})");
  return 0;
}
// A transparent middleware: keeps the builtin driver, including approvals,
// streaming, persistence, cancellation and task limits.
static int turn(void*, const char* input, size_t length, const PenguinCallV1* call) {
  return call->next(call->next_context, input, length, call->emit, call->emit_context);
}
extern "C" PENGUIN_EXPORT int codex_penguin_init_v1(const PenguinHostV1* host, PenguinModuleV1* out) {
  if (!host || host->version != 1 || host->size < sizeof(PenguinHostV1) || !out) return 1;
  out->version=1; out->size=sizeof(PenguinModuleV1); out->id="demo-penguin";
  return host->bind(host->context,"model.request","builtin.model",model,nullptr) ||
    host->bind(host->context,"model.catalog","builtin.model",catalog,nullptr) ||
    host->bind(host->context,"agent.turn","builtin.agent",turn,nullptr) ||
    host->bind(host->context,"tools.penguin_status","",tool,nullptr);
}

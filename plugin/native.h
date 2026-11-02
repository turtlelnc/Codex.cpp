#ifndef CODEX_PLUGIN_NATIVE_V1_H
#define CODEX_PLUGIN_NATIVE_V1_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
/* Strings are UTF-8, lengths exclude NUL. Callbacks and buffers are borrowed only
   for the duration of a call. Never throw exceptions across this C boundary. */
typedef void (*PluginEmitV1)(void*, const char* channel, const char* bytes, size_t length);
typedef int (*PluginNextV1)(void*, const char* json, size_t length, PluginEmitV1, void*);
typedef struct PluginCallV1 {
  uint32_t version, size;
  PluginEmitV1 emit;
  void* emit_context;
  PluginNextV1 next;
  void* next_context;
  /* Cooperatively pump UI/cancellation during blocking native work. */
  int (*poll)(void* emit_context);
} PluginCallV1;
typedef int (*PluginHandlerV1)(void* state, const char* json, size_t length, const PluginCallV1*);
typedef struct PluginHostV1 {
  uint32_t version, size;
  void* context;
  int (*bind)(void*, const char* service, const char* replaces, PluginHandlerV1, void* state);
} PluginHostV1;
typedef struct PluginModuleV1 {
  uint32_t version, size;
  const char* id;
  void* state;
  void (*dispose)(void*);
} PluginModuleV1;
typedef int (*PluginInitV1)(const PluginHostV1*, PluginModuleV1*);
/* Export this symbol. Register only services declared in the manifest. */
#if defined(PLUGIN_BUILD_MODULE)
PLUGIN_EXPORT int codex_plugin_init_v1(const PluginHostV1*, PluginModuleV1*);
#endif
#ifdef __cplusplus
}
#endif
#endif

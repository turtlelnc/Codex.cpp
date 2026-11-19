#ifndef CODEX_PLUGIN_ISOLATED_V1_H
#define CODEX_PLUGIN_ISOLATED_V1_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One invocation per process. The input is a borrowed byte buffer containing
   only the selected file. Write text to stdout and return zero on success. */
#if !defined(_WIN32)
__attribute__((visibility("default")))
#endif
int codex_plugin_read_v1(const char* input, size_t size);
/* Request the selected file once. The buffer must hold at least 64 KiB.
   Call before writing to stdout. The context is valid for this invocation. */
typedef struct CodexIsolatedReadV2 {
  void* context;
  int (*read_selected)(void* context, char* bytes, size_t capacity, size_t* actual);
} CodexIsolatedReadV2;
#if !defined(_WIN32)
__attribute__((visibility("default")))
#endif
int codex_plugin_read_v2(const CodexIsolatedReadV2* broker);
#ifdef __cplusplus
}
#endif
#endif

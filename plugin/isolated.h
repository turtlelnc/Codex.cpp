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
#ifdef __cplusplus
}
#endif
#endif

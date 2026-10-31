#ifndef CODEX_PENGUIN_NATIVE_V1_H
#define CODEX_PENGUIN_NATIVE_V1_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(_WIN32)
#define PENGUIN_EXPORT __declspec(dllexport)
#else
#define PENGUIN_EXPORT __attribute__((visibility("default")))
#endif
/* Strings are UTF-8, lengths exclude NUL. Callbacks and buffers are borrowed only
   for the duration of a call. Never throw exceptions across this C boundary. */
typedef void (*PenguinEmitV1)(void*, const char* channel, const char* bytes, size_t length);
typedef int (*PenguinNextV1)(void*, const char* json, size_t length, PenguinEmitV1, void*);
typedef struct PenguinCallV1 {
  uint32_t version, size;
  PenguinEmitV1 emit;
  void* emit_context;
  PenguinNextV1 next;
  void* next_context;
  /* Cooperatively pump UI/cancellation during blocking native work. */
  int (*poll)(void* emit_context);
} PenguinCallV1;
typedef int (*PenguinHandlerV1)(void* state, const char* json, size_t length, const PenguinCallV1*);
typedef struct PenguinHostV1 {
  uint32_t version, size;
  void* context;
  int (*bind)(void*, const char* service, const char* replaces, PenguinHandlerV1, void* state);
} PenguinHostV1;
typedef struct PenguinModuleV1 {
  uint32_t version, size;
  const char* id;
  void* state;
  void (*dispose)(void*);
} PenguinModuleV1;
typedef int (*PenguinInitV1)(const PenguinHostV1*, PenguinModuleV1*);
/* Export this symbol. Register only services declared in the manifest. */
#if defined(PENGUIN_BUILD_MODULE)
PENGUIN_EXPORT int codex_penguin_init_v1(const PenguinHostV1*, PenguinModuleV1*);
#endif
#ifdef __cplusplus
}
#endif
#endif

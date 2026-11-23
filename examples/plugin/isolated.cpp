// The isolated form of plugin_status requests only the selected input and
// reports its byte count. No host service override is registered.
#include "../../plugin/isolated.h"
#include <cstdio>
extern "C" int codex_plugin_read_v2(const CodexIsolatedReadV2* broker) {
  char bytes[64 * 1024];
  size_t size = 0;
  if (!broker || !broker->read_selected ||
      broker->read_selected(broker->context, bytes, sizeof bytes, &size)) return 1;
  std::printf("Isolated Plugin tool is active. Selected file: %zu bytes.\n", size);
  return 0;
}

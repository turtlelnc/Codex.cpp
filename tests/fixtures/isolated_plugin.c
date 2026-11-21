#include "../../plugin/isolated.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

int codex_plugin_read_v1(const char* input, size_t size) {
  if (size == 5 && memcmp(input, "crash", 5) == 0) abort();
  if (size == 4 && memcmp(input, "wait", 4) == 0) {
#ifndef _WIN32
    sleep(10);
#endif
    return 1;
  }
#ifndef _WIN32
  if (size > 6 && memcmp(input, "check:", 6) == 0) {
    int port;
    char target[4096];
    if (sscanf(input + 6, "%d %4095[^\n]", &port, target) != 2) return 1;
    FILE* f = fopen(target, "rb");
    if (f) { fclose(f); return 1; }
    f = fopen(target, "ab");
    if (f) { fclose(f); return 1; }
    if (getenv("CODEX_TEST_HOST_CREDENTIAL")) return 1;
    pid_t child = fork();
    if (child == 0) _exit(0);
    if (child > 0) { waitpid(child, NULL, 0); return 1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      struct sockaddr_in addr = {0};
      addr.sin_family = AF_INET;
      addr.sin_port = htons((unsigned short)port);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      int connected = connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0;
      close(fd);
      if (connected) return 1;
    }
    puts("boundaries-ok");
    return 0;
  }
#endif
  return fwrite(input, 1, size, stdout) == size ? 0 : 1;
}

int codex_plugin_read_v2(const CodexIsolatedReadV2* broker) {
  char bytes[65537];
  size_t size = 0;
  if (broker->read_selected(broker->context, bytes, sizeof(bytes) - 1, &size)) return 1;
  bytes[size] = 0;
  return codex_plugin_read_v1(bytes, size);
}

#!/bin/sh
set -eu

# Hosted Ubuntu runners use this mirror list. A broken regional DNS endpoint
# must not prevent installing the sandbox/browser dependencies required by tests.
mirror_list=/etc/apt/apt-mirrors.txt
if [ ! -f "$mirror_list" ]; then exit 0; fi
if timeout 10 getent ahostsv4 azure.archive.ubuntu.com >/dev/null; then exit 0; fi
if ! timeout 10 getent ahostsv4 archive.ubuntu.com >/dev/null; then
  echo 'Ubuntu mirror DNS is unavailable; dependency installation cannot proceed.' >&2
  exit 1
fi
echo 'Azure Ubuntu mirror DNS failed; using the official Ubuntu HTTPS mirrors.'
printf '%s\n' https://archive.ubuntu.com/ubuntu https://security.ubuntu.com/ubuntu |
  sudo tee "$mirror_list" >/dev/null

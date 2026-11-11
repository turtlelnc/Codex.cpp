#!/bin/sh
set -eu

# sudo drops proxy/CA environment variables. Preserve the supplied connection
# settings for APT without logging proxy credentials or changing TLS verification.
http_proxy_url=${http_proxy:-${HTTP_PROXY:-${https_proxy:-${HTTPS_PROXY:-}}}}
https_proxy_url=${https_proxy:-${HTTPS_PROXY:-$http_proxy_url}}
escape_config() { printf '%s' "$1" | sed 's/\\/\\\\/g;s/"/\\"/g'; }
if [ -n "$http_proxy_url$https_proxy_url" ]; then
  config=$(mktemp)
  trap 'rm -f "$config"' EXIT HUP INT TERM
  if [ -n "$http_proxy_url" ]; then printf 'Acquire::http::Proxy "%s";\n' "$(escape_config "$http_proxy_url")" >> "$config"; fi
  if [ -n "$https_proxy_url" ]; then printf 'Acquire::https::Proxy "%s";\n' "$(escape_config "$https_proxy_url")" >> "$config"; fi
  if [ -n "${SSL_CERT_FILE:-}" ] && [ -f "$SSL_CERT_FILE" ]; then
    printf 'Acquire::https::CaInfo "%s";\n' "$(escape_config "$SSL_CERT_FILE")" >> "$config"
  fi
  sudo install -m 600 "$config" /etc/apt/apt.conf.d/99-codex-ci-proxy
  echo 'APT will use the supplied proxy settings and certificate trust.'
fi

# Probe HTTPS through the inherited proxy, not direct DNS: proxied connections
# can work even when a runner cannot resolve public domains itself.
mirror_list=/etc/apt/apt-mirrors.txt
if [ ! -f "$mirror_list" ]; then exit 0; fi
probe() { curl --fail --silent --show-error --head --connect-timeout 5 --max-time 15 "$1/dists/jammy/InRelease" >/dev/null; }
if probe https://azure.archive.ubuntu.com/ubuntu; then exit 0; fi
if ! probe https://archive.ubuntu.com/ubuntu; then
  echo 'Ubuntu mirrors are unreachable over HTTPS; dependency installation cannot proceed.' >&2
  exit 1
fi
echo 'Regional Ubuntu mirror is unreachable; using the official Ubuntu HTTPS mirrors.'
printf '%s\n' https://archive.ubuntu.com/ubuntu https://security.ubuntu.com/ubuntu |
  sudo tee "$mirror_list" >/dev/null

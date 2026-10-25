#!/bin/sh
# Install on a dedicated Debian 13/Ubuntu Linux host, as root. Existing configs are preserved.
set -eu
if [ "$(id -u)" -ne 0 ] || [ "$#" -ne 1 ]; then
  echo "Usage: sudo sh server/install.sh /absolute/path/to/built/codex-cpp" >&2
  exit 2
fi
server_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
agent_binary=$1
if [ ! -f "$agent_binary" ] || [ ! -x "$agent_binary" ]; then
  echo "Supply a compiled, executable codex-cpp binary" >&2
  exit 2
fi
apt-get update
apt-get install -y --no-install-recommends openssh-server tmux bubblewrap curl git python3 procps
getent group codex-remote >/dev/null || groupadd --system codex-remote
install -d -m 0755 /usr/local/libexec/codex-server /etc/codex-server/keys /srv/codex-users
install -m 0755 "$agent_binary" /usr/local/bin/codex-cpp
install -m 0755 "$server_dir/gateway.py" /usr/local/libexec/codex-remote-gateway
install -m 0755 "$server_dir/add-user.py" /usr/local/sbin/codex-server-add-user
for module in gateway.py collab_daemon.py collab_worker.py collab_client.py; do
  install -m 0755 "$server_dir/$module" "/usr/local/libexec/codex-server/$module"
done
install -d -m 0700 /var/lib/codex-collab
install -d -m 0711 /srv/codex-projects
install -m 0644 "$server_dir/codex-collab.service" /etc/systemd/system/codex-collab.service
if [ ! -e /etc/codex-server/sshd_config ]; then
  install -m 0644 "$server_dir/sshd_config" /etc/codex-server/sshd_config
fi
if [ ! -e /etc/codex-server/ssh_host_ed25519_key ]; then
  ssh-keygen -q -t ed25519 -N '' -f /etc/codex-server/ssh_host_ed25519_key
fi
if [ ! -e /etc/security/limits.d/codex-server.conf ]; then
  install -m 0644 "$server_dir/limits.conf" /etc/security/limits.d/codex-server.conf
fi
install -m 0644 "$server_dir/codex-server.service" /etc/systemd/system/codex-server.service
# openssh-server provides this on normal hosts; needed before sshd config validation.
install -d -m 0755 /run/sshd /run/codex-server
/usr/sbin/sshd -t -f /etc/codex-server/sshd_config
systemctl daemon-reload
systemctl enable --now codex-collab.service codex-server.service
printf '%s\n' 'Service installed on port 2222. The host administrative SSH configuration was not changed.'
printf '%s\n' 'Add individual accounts with codex-server-add-user USER /path/to/public-key.pub.'

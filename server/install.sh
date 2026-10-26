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
for module in gateway.py collab_daemon.py collab_worker.py collab_client.py web_server.py local_agent.py; do
  install -m 0755 "$server_dir/$module" "/usr/local/libexec/codex-server/$module"
done
install -d -m 0755 /usr/local/libexec/codex-server/web
install -m 0644 "$server_dir/../web/index.html" "$server_dir/../web/app.js" "$server_dir/../web/style.css" /usr/local/libexec/codex-server/web/
if ! getent passwd codex-web >/dev/null; then
  useradd --system --user-group --home-dir /var/lib/codex-web --shell /usr/sbin/nologin codex-web
fi
install -d -m 0700 -o codex-web -g codex-web /var/lib/codex-web
install -m 0644 "$server_dir/codex-web.service" /etc/systemd/system/codex-web.service
install -d -m 0755 /etc/systemd/system/codex-collab.service.d
cat > /etc/systemd/system/codex-collab.service.d/web.conf <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/bin/python3 -I /usr/local/libexec/codex-server/collab_daemon.py --web-user codex-web
EOF
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
printf '%s\n' 'Web gateway installed but not started; see server/WEB.md to configure HTTPS and accounts.'
printf '%s\n' 'Service installed on port 2222. The host administrative SSH configuration was not changed.'
printf '%s\n' 'Add individual accounts with codex-server-add-user USER /path/to/public-key.pub.'

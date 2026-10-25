# syntax=docker/dockerfile:1
FROM debian:trixie-slim
RUN --mount=type=secret,id=proxy_ca,mode=0444,required=true \
    sed -i 's|http://deb.debian.org|https://deb.debian.org|g' /etc/apt/sources.list.d/debian.sources && \
    apt-get -o Acquire::https::CaInfo=/run/secrets/proxy_ca update && \
    apt-get -o Acquire::https::CaInfo=/run/secrets/proxy_ca install -y --no-install-recommends ca-certificates openssh-server python3 tmux bubblewrap curl git && \
    groupadd --system codex-remote && \
    mkdir -p /etc/codex-server/keys /srv/codex-users /run/sshd /run/codex-server /usr/local/libexec && \
    rm -f /etc/ssh/ssh_host_* && rm -rf /var/lib/apt/lists/*
COPY gateway.py /usr/local/libexec/codex-remote-gateway
COPY add-user.py /usr/local/sbin/codex-server-add-user
COPY codex-cpp /usr/local/bin/codex-cpp
COPY sshd_config /etc/codex-server/sshd_config
RUN chmod 755 /usr/local/libexec/codex-remote-gateway /usr/local/sbin/codex-server-add-user /usr/local/bin/codex-cpp
CMD ["/bin/sh", "-c", "ssh-keygen -q -t ed25519 -N '' -f /etc/codex-server/ssh_host_ed25519_key && exec /usr/sbin/sshd -D -e -f /etc/codex-server/sshd_config"]

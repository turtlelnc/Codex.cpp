FROM codex-ssh-test:local
COPY gateway.py /usr/local/libexec/codex-remote-gateway
COPY gateway.py collab_daemon.py collab_client.py collab_worker.py /usr/local/libexec/codex-server/
COPY codex-cpp /usr/local/bin/codex-cpp
COPY client-codex-cpp /test/codex-client
COPY collab_model.py /test/model.py
RUN chmod 755 /test/codex-client /usr/local/bin/codex-cpp /usr/local/libexec/codex-remote-gateway /usr/local/libexec/codex-server/*.py && mkdir -p /run/codex-collab /var/lib/codex-collab /srv/codex-projects && chmod 700 /var/lib/codex-collab && chmod 711 /srv/codex-projects
CMD ["/bin/sleep", "infinity"]

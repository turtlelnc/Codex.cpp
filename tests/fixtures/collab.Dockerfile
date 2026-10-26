FROM codex-ssh-test:local
COPY gateway.py /usr/local/libexec/codex-remote-gateway
COPY gateway.py collab_daemon.py collab_client.py collab_worker.py web_server.py local_agent.py /usr/local/libexec/codex-server/
COPY codex-cpp /usr/local/bin/codex-cpp
COPY client-codex-cpp /test/codex-client
COPY collab_model.py /test/model.py
COPY web/ /usr/local/libexec/codex-server/web/
COPY web_integration_fixture.py /test/web-check.py
COPY web_test_forwarder.py /test/web_test_forwarder.py
RUN useradd --system --user-group --home-dir /var/lib/codex-web --shell /usr/sbin/nologin codex-web && mkdir -p /var/lib/codex-web && chown codex-web:codex-web /var/lib/codex-web && chmod 700 /var/lib/codex-web
RUN chmod 755 /usr/local/libexec/codex-server/web && chmod 644 /usr/local/libexec/codex-server/web/* /test/web-check.py /test/web_test_forwarder.py && chmod 755 /test/codex-client /usr/local/bin/codex-cpp /usr/local/libexec/codex-remote-gateway /usr/local/libexec/codex-server/*.py && mkdir -p /run/codex-collab /var/lib/codex-collab /srv/codex-projects && chmod 700 /var/lib/codex-collab && chmod 711 /srv/codex-projects
CMD ["/bin/sleep", "infinity"]

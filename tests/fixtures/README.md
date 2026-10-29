`localhost-test.crt` and `localhost-test.key` are public test fixtures, generated
only for loopback HTTPS regression tests. They are not service credentials and
must never be used to deploy a server. The certificate covers 127.0.0.1 and expires
in 2036; the standalone test checks rejection by system roots and acceptance only
when explicitly provided as its CA.

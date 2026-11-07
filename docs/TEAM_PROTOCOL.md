# Team protocol negotiation and compatibility plugins

Software versions and wire protocols are separate. This first implementation uses
canonical protocol **1**. `GET /api/protocol` advertises the application, mode,
accepted protocol IDs and capability names without credentials. It is still
subject to Host/Origin/rebinding checks. Authenticated `/api/me` includes the same
description for the browser. This is HTTP API negotiation, not SSH transport or
host-key authentication, and does not replace existing authentication/TLS.

The native team client discovers and validates the protocol before sending login
credentials, prefers native protocol 1, and checks scoped-read/full-snapshot
capabilities. Writes additionally require `approval.bound.v1`, `request-id.v1`
and `team.write.v1`. Missing write capabilities produce a visibly read-only
connection. RPCs carry `X-Codex-Protocol`; the hosted server checks that selection
before dispatch. Unknown/unnegotiated versions return 426 without executing the
operation. A 426 clears the native connection: it does not replay a write.

The server still performs membership, owner, scope, approval-token, boolean,
request-ID, fingerprint and Git checks. Capability names describe API semantics;
they do not grant project permissions. Login and logout retain their existing
session/CSRF checks. Local single-user RPCs without a header remain compatible;
new bundled browser code selects protocol 1. An old remote browser without
handshake metadata is read-only; an explicitly incompatible advertised version is
refused. These client restrictions do not repair an old server's vulnerabilities.

## Official legacy adapter

The optional `team-protocol-compat` native module supports the unversioned
Codex HTTP API profile represented by main `ae7ffc7` / v2026.11.02.1. Its wire ID
is **0**. It preserves the existing packet fields and forces full snapshots, so
old clients never need to interpret a newer delta envelope.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target codex-cpp team-protocol-compat --parallel 2
./build/codex-cpp --plugin "$PWD/build/team-protocol-compat/plugin.json" \
  serve --state-dir ./team-state --host 127.0.0.1 --port 9080
```

For a multi-config Windows build, use `--config Release`,
`build/Release/codex-cpp.exe` and
`build/team-protocol-compat/Release/plugin.json`. Linux fully static musl hosts
cannot load native DLLs; use the normal glibc/native-plugin build.

- **Old client → new server:** an administrator explicitly loads the module on
  the server. Legacy requests then pass through the current core authorization
  and mutation checks. Missing approval tokens, task request IDs or fingerprints
  are rejected; the adapter invents none of them.
- **New client → old server:** explicitly load the module on the client before
  opening `/team`. Only the recognized missing-endpoint response is eligible for
  legacy fallback, and `/api/me` must identify a hosted service before credentials
  are sent. Because the old server cannot negotiate safety capabilities, this
  connection is **read-only**. It cannot send tasks, approve or write Git/files.
- A malformed, failed or explicitly incompatible handshake is never treated as a
  legacy server. There is no automatic module download or activation.

At least one side must contain this new adapter interface. A module cannot add an
extension point to an arbitrary old executable that never implemented it. Legacy
support is for the specified API family, not a promise of arbitrary-version
interoperability. Native modules remain fully trusted host code. Installation or
updating a module requires an explicit startup/restart, but not replacement of
the host executable. Plugins are not hot-loaded over the network.

## Extending a codec without replacing the host

Native modules bind `team.protocol.adapters`, replacing `builtin.protocol`, and
return a catalog when called with `{}`. No credentials, cookies, user packets or
unfiltered events are passed to this service. The host validates a maximum of 15
codecs (16 wire IDs including native 1) and applies their declarative maps itself.

```json
{"adapters":[{
  "wire_version":7,
  "canonical_version":1,
  "profile":"example-wire-seven",
  "client":true,
  "server":true,
  "full_snapshots":true,
  "request_renames":{"message":"text"},
  "response_renames":{"timeline":"history"}
}]}
```

`request_renames` maps peer fields to canonical fields; `response_renames` maps
canonical fields to peer fields. They are optional, bounded, one-to-one top-level
renames. Ambiguous fields and chained maps fail. They cannot rename operation,
actor, project, scope, task IDs, approval decisions/tokens, session IDs, optimistic
editor bases, Git source/target/fingerprints, permission/sandbox, or the pending,
events and error fields. All authorization/redaction happens before response
encoding. Wire 1 and its capabilities cannot be replaced by a plugin catalog.

Future codecs can bridge non-security packet differences while retaining the
canonical security bindings and authentication endpoints. A different approval
or deduplication model needs a supported core contract; a codec cannot assert it
into existence. A server's claimed capabilities are not cryptographic evidence
that an untrusted server behaves correctly.

`team-protocol-fixture` and `team-protocol-bad-fixture` are **test-only** build
targets. The first demonstrates working wire-7 field conversion; the second
proves that an approval-field map is refused before the server listens. Neither
fixture is part of the official legacy profile or default build.

## Verification

`tests/team_protocol_test.cjs` uses actual server/client processes and native
modules: native/native handshake, server-side absent/unknown version rejection,
legacy activation, core rejection of missing mutation bindings, no credential
submission or downgrade on bad handshakes, old-peer/capability read-only gating,
and real client/server field translation. `webui_quality_test.cjs` also checks
browser read-only/incompatible peers and preserved draft input. Existing hosted,
TUI, auth/CSRF, approvals and plugin tests remain enabled. All model/login data in
these tests are fixtures, with no real OAuth or paid model request.

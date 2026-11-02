# Portable profile and host-managed updates

The host selects `CODEX_CPP_HOME` and project session paths before plugin startup.
Without portable options, existing paths and APIs remain unchanged. Prefixes are
inherited by trusted worker processes. Portable profiles are local-only; hosted
services retain their separate state directory and membership isolation.

Export/import, plugin sharing and executable installation are host CLI operations,
not registered Agent tools or WebUI RPC operations. The portable directory is
protected by the existing file-tool checks, Web workspace checks and restricted
process sandbox. Native plugins remain fully trusted host code.

Shared runtime leases permit Web workers; export and installation require an
exclusive lease. Export excludes updater caches. Import validates the complete
package, extracts to a private staging directory with exclusive file creation,
and publishes only into a new directory. Imported enabled-plugin configuration
is replaced with an empty list and MCP autostart configuration is quarantined.
No imported plugin is initialized by the transfer commands.

Transfer format 1 uses bounded, length-prefixed file records. Encrypted packs use
PBKDF2-HMAC-SHA256 (600000 iterations), a random 16-byte salt, AES-256-GCM, a random
12-byte nonce, a 16-byte authentication tag and fixed format/KDF AAD. Windows uses
BCrypt; other platforms use OpenSSL. Plain packages provide SHA-256 integrity,
not authentication. Runtime storage is plaintext; there is no startup password.

The updater fetches fixed official GitHub Release metadata, selects an exact
platform/CPU/runtime asset, verifies its GitHub SHA-256 and size, and stages it.
Only explicit installation executes the selected candidate's format handshake.
The handshake checks compatibility, not safety. GitHub HTTPS/digests are not an
independent publisher signature. Installation rechecks the candidate and target,
records recovery metadata before replacing the program and retains a sibling
backup. Data does not move into the executable. Windows uses an exiting-parent
helper because an executing image cannot replace itself. Cache/helper/backup
cleanup remains manual; locks are local filesystem locks, not distributed locks.

## Regression coverage

- `portable_data_test.cjs`: real sessions, exclusive export locks, relocation,
  independent Node crypto interoperability in both directions, Unicode passwords,
  malformed paths/collisions, tamper, credential exclusion, imported Plugin/MCP
  quarantine, permissions and static Plugin sharing.
- `portable_update_test.cjs`: actual disposable executable replacement and rollback,
  matching credentials, wrong SHA/CPU, required consent, active task refusal,
  post-replacement metadata failure recovery and cache tamper rejection.
- `portable_download_test.cjs` with `codex-self-test`: real HTTPS redirect and
  incremental writes, cancellation, size bounds, untrusted CA/HTTP rejection and
  atomic cleanup. Arbitrary test origins exist only in the self-test entry point.
- `portable_web_test.cjs`: real Chromium startup/reload, centralized sessions,
  protected data rejection, terminal pause, restart and service shutdown. This
  does not validate graphical desktop browser launching or a live Agent handoff.

The native CI matrix runs transfer, replacement and transport tests on Linux,
macOS and Windows; the browser job runs the portable Web bridge on Linux.
Tests use fixtures and no paid model API.

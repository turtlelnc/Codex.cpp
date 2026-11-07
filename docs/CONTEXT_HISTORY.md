# Model context and visible conversation history

Multiple tool calls can exceed the model input budget and trigger compaction.
Previously, compaction replaced the same `.items.jsonl` file used by WebUI with
a synthetic user-message summary. This both discarded typed tool history and
rendered the internal summary as if the user had submitted it.

Compaction now appends a versioned `codex_context_checkpoint` record containing
the replacement model input. Original user messages, assistant messages, calls
and results remain in the append-only journal. Ordinary records continue through
`journal.append`; compaction checkpoints use the same validated plugin service.
The existing `journal.replace` service remains for explicit replacement/migration.

On resume, the runtime applies each checkpoint to its in-memory model context,
then adds following records. Checkpoint metadata never reaches a provider as an
API input item. Unsupported versions and nested checkpoints are rejected. Checkpoints exceeding
the existing 12,000,000-byte journal-record limit fail before changing history or
model context.
The context budget and incremental-summary behavior remain unchanged. A fork
copies the full journal, including checkpoints, and restores the same model
context. No additional runtime or front/backend protocol dependency is needed.

WebUI derives its bounded visible timeline from original journal records and
ignores checkpoint records. Their byte positions remain stable, preserving tool
card identity and access to full pre-compaction logs after refresh or restart.
The journal grows with retained history; model resume scans it sequentially.
This change does not add older-message pagination or a seek index.

For legacy compacted sessions, a first-row `[prior conversation summary]` user
record is classified as internal context only when a matching compaction entry
in the protected transcript corroborates its provenance. It appears as a
collapsed, labelled internal-summary detail, not a user bubble or session title.
A literal marker without that evidence remains a user message. The legacy probe
uses the existing bounded transcript reader; missing/oversized transcripts are
not guessed. History already overwritten by older versions cannot be recreated
from a summary; its original transcript remains available when preserved.

Readers remain compatible with old journals. New checkpoint journals require
this updated runtime for model continuation: older executables do not know how
to reconstruct their compacted model context. Use matching updated binaries
when moving session data between devices or rolling back application code.

Regression coverage:
- Core tests: immutable journal prefix, valid checkpoint restoration, malformed
  checkpoint rejection without writes, retained full logs, legacy provenance,
  and genuine user messages containing the old marker.
- `compaction_history_test.cjs`: real Agent/provider/WebUI processes, six reads
  triggering multiple automatic compactions, no disappearing tool rows or
  synthetic user summaries, browser reload, service restart and task continuation.
- `context_budget_test.cjs`: model budgets, incremental summaries, failed-summary
  retention, recent images, and full-journal fork/resume after compaction.

# Agent instructions

These instructions apply to any AI coding agent working in this
repository (GitHub Copilot, Codex, Claude Code, Cursor, etc.).

## Git commits

- Never run `git add` and `git commit` in the same command. Staging and
  committing must be separate user-visible steps.
- Do not add bot, tool, or runtime-generated trailers to commit
  messages. This includes `Co-Authored-By`, `Copilot-Session`, and any
  similar attribution.
- Every commit-message line has a hard maximum of 72 characters,
  including the subject, body, and bullets. Verify all line lengths
  before committing.
- Keep message length proportional to the change. Small or mechanical
  changes get a concise one-line subject. Features get an imperative
  subject, a blank line, and a body explaining the key behaviors and
  rationale. Use bullets for supporting changes when appropriate.
- Before every commit, present the exact proposed message as a heredoc
  and wait for explicit user approval:

  ```bash
  git commit -F - <<'MSG'
  Subject

  Body
  MSG
  ```

  Do not invoke `git commit` until the user approves that exact message.
- Only commit or push when explicitly requested.

## Post-feature review

- After completing a feature the user requested, automatically run a
  review in a separate subagent (for example, Copilot's `rubber-duck`
  agent, or the equivalent review subagent in other tools). Do not
  wait for the user to ask.
- Present the findings as recommended fixes ordered by priority
  (highest first), each with a short rationale, then ask the user
  which ones to apply. Do not apply any fix until the user chooses.
- After applying the chosen fixes, ask the user once whether to run
  another review. If they agree, repeat the same review, prioritize,
  and apply cycle; otherwise stop.

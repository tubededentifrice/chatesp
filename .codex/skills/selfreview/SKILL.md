---
name: selfreview
description: Skeptically review recent ChatESP repository changes against their intent, autofix defects and omissions, and run the applicable project quality gates before handoff. Use after a non-trivial firmware, hardware, iOS, protocol, security, test, tool, or documentation edit; invoke with `autofix` as the mandatory final step of each implementation plan.
---

# Self-review ChatESP changes

Review task-owned changes adversarially. Find concrete defects, omissions, and
regressions. Do not expand into unrelated cleanup.

## Select the mode

- **Interactive**: report findings, ask which findings to fix, apply the
  selected fixes, and run the applicable gates.
- **Autofix**: do not ask for triage. Fix each BUG, MISSING, and RISKY finding in
  scope, then run the applicable gates. Use this mode for `$selfreview autofix`
  and at the end of an implementation plan.

Do not stage, commit, push, or invoke selfreview again. Do not expand the
user's authority. Stop for destructive work, external changes, or a material
product choice that the user must make.

## Establish scope and intent

Read `AGENTS.md` and the original task or plan. Inspect:

```sh
git status --short
git diff
git diff --cached
```

If `HEAD` exists, inspect `git log --oneline -5`. An unborn repository is valid.
Include task-owned untracked files. Exclude unrelated changes. List intended
behavior, hardware and protocol effects, user-visible effects, privacy and
power effects, and claimed verification.

## Investigate

Read each full changed file, its callers, tests, protocol peers, and affected
documents. Search for shared constants and duplicated contracts. In autofix
mode, stay in the current execution mode and continue without user triage.

## Review adversarially

### State, time, and resources

- Trace normal, empty, invalid, timeout, disconnect, cancellation, reboot,
  sleep, wake, and `millis()` wrap paths.
- Use monotonic subtraction for elapsed time. Clear thread state on sleep.
- Check task ownership, synchronization, cleanup, stack use, heap lifetime,
  repeated NVS writes, and bounded buffer or container growth.
- Require fixed limits for recording, history, JSON, images, output, tools,
  retries, and waits.

### Hardware and failure independence

- Keep pins and board-revision behavior in the official BSP board layer.
- Check display rotation, AMOLED shutdown, touch mapping, buttons, wake source,
  ES8311 audio format, Wi-Fi, BLE, PSRAM, and peripheral cleanup when affected.
- A missing optional service must not prevent UI recovery or sleep.
- Mark current draw, audio quality, radio, display, and long-duration claims as
  unverified until a physical test supplies evidence.

### Network, tools, and model behavior

- Validate TLS, endpoints, status codes, content types, JSON, audio format,
  redirects, image size and format, cancellation, and response limits.
- Keep providers behind narrow interfaces. Do not give tools credentials.
- Check tool argument validation, result limits, maximum rounds, and model-loop
  recovery after a tool error.
- Keep the model prompt and output limit consistent with concise spoken answers.

### BLE, security, and privacy

- Treat BLE writes, Wi-Fi values, provider settings, chat text, audio, tool
  arguments, URLs, and network responses as untrusted.
- Require an authenticated encrypted BLE link, versioned packets, exact length,
  revision, fingerprint, validation, and an application acknowledgement.
- Store iOS secrets in Keychain and device secrets in encrypted NVS. Never log
  or commit a credential, private content, signing data, personal path, stable
  device identifier, or precise location.
- Check that raw microphone audio stays in memory and is deleted after use.

### Completeness

- Require tests for new pure logic and negative paths.
- Keep firmware, iOS, `README.md`, architecture, hardware, protocol, security,
  and acceptance tests consistent.
- Keep Python, PlatformIO, Git, and GitHub Action dependencies immutable,
  exact, and older than the two-week cooldown.
- Check warnings, memory use, dead code, naming, and source licenses.

## Record and fix findings

Use these severities:

- **BUGS**: causes an error, unsafe behavior, or wrong result.
- **MISSING**: leaves the requested implementation incomplete.
- **RISKY**: fails under a specific credible condition.
- **NITPICKS**: optional polish with no material effect.

Give each finding a `file:line`, manifestation, and concrete fix. Do not invent
findings. In autofix mode, fix all material findings in scope. Apply a NITPICK
only when it is safe and tightly scoped. Recheck each affected path.

## Run quality gates

Discover narrower checks, then run the applicable gates:

```sh
python3 tools/check_dependency_age.py
uv sync --locked
uv run --locked python -m unittest discover -s tests -p 'test_*.py'
uv run --locked python tools/check_secrets.py
uv run --locked python tools/pio.py test -e native
uv run --locked python tools/pio.py run
```

Run firmware gates when firmware or its claims change. Run the unsigned generic
iOS build and test-bundle commands in `ios/README.md` when iOS or BLE changes.
Do not weaken or skip a failed test. Report fixed findings, exact checks and
results, unverified physical gates, and blockers. Return without publication.

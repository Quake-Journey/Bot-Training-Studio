# Standalone completion work

Deferred joint feature (PO, 2026-10-08): shared automatic initial map baseline,
online server observations and Studio baseline build/import/refinement/export.
See [the specification, reviewed r3](ONLINE_BASELINE_SPEC.md). Initial structure
supports map-only input or map plus demos from the outset, using one reusable
baseline that assists subsequent demo analysis. No separate map-only job is
mandatory when recordings are already supplied. The technical review makes
the missing no-demo builder, dynamic-physics parity, map-rule migration,
incremental budgets and transactional storage explicit requirements.
Implement the game-runtime
integration only after q3t2 completion and delivery of 11.2; no next version
number is assigned. This is a specification, not an implemented capability,
and does not change the current application version or readiness claims.

Owner: Bot Training Studio. No edits to OpenTDM-X game module or Claude's q3t2 lane.

Requested scope: all application/model work independent of changing game DLL.
Completion requires evidence for every applicable item; a prototype or pipeline
success does not close tactical learning or gameplay validation.

- [x] Standalone native demo decoder and physics tools, source provenance/build.
- [x] Project/library lifecycle: BSP + game/trick recordings, fresh/update/cancel.
- [ ] Native DM2/MVD2 import and bounded ZIP/RAR intake, resumable provenance.
- [ ] Map geometry/items/visibility and observed route/trick extraction.
- [ ] Sequence datasets, grouped holdouts, unknown-state masks and quality gates.
- [ ] Multi-task movement/weapon/tactic models with replay and donor learning.
- [ ] Resource calibration, mixed precision, OOM recovery, CPU fallback.
- [x] Durable completed-epoch checkpoint/resume/cancellation and checkpoint rotation.
- [ ] Donor identity review, style statistics, correctly attributed phrases.
- [ ] Offline movement/route/shot qualification, uncertainty and failure reports.
- [x] Portable candidate DATA package compiler/validator and offline rollback library.
- [ ] Complete UI workflows and managed runtime installation/bundling.
- [x] Persistent background Windows CPU/RAM/GPU/VRAM meter with separate job CPU/RAM.
- [x] Background protocol processing, bounded UI event delivery and confirmed cooperative exit; real checkpoint-cancellation checks.
- [x] System/Russian/English UI selection and paired, packaged DOCX user guides with freshness checks.
- [x] Windows EXE/window icon; packaged CPU/NVIDIA libraries and reuse/automatic installation of Python alone, with asynchronous cancellation and verified computation. AMD/Intel packaging remains unqualified.
- [x] Persistent CPU/GPU selection independent of Python readiness; centralized version, bilingual first-launch/manual history and verified GitHub updates with cancellation and rollback.
- [ ] Real demo/map regression tests, clean-machine/package checks and docs.

Game DLL loading/execution remains excluded. Actual online match quality with a
new compiled knowledge package requires the future coordinated runtime change.

0.2 evidence: docs/VALIDATION_0.2_RU.md. Unchecked items above have partial
implementations and must not be described as completed capabilities. In
particular, auxiliary tactical heads failed their baseline comparisons; the
compiler excludes them even when movement predictions passed. CPU/CUDA local
runtime builds exist; this is not AMD/Intel or clean-machine certification.

PO corrected experiment order: learn/validate ONLY q2duel5 and ztn2dm3 now.
Wait for Claude to finish q3t2, then add q3t2 and its teleport experience;
only afterwards evaluate/adapt q2duel1 with retention checks. Both q3t2 and
q2duel1 are excluded from current training; do not bypass this sequencing.
q2duel1 source archives: demos.q2players.org.zip and EDL.zip (user-local corpus;
never upload the recordings with public source).

2026-10-07 continuation: docs/VALIDATION_OUTCOMES_RU.md records terminal-aware
v3 observations, per-target masks, observed ammo/shot history, outcome heads,
conditional training-only donor evidence, five grouped folds and real sequential
map training. These finish a data/model milestone, NOT the entire checklist.
Weapon decisions, exact resource targets, full tactical policies, full trick
recovery and gameplay qualification remain open. No game DLL changes.

Continued rather than stopping at the outcome milestone: timed HUD pickup
edges, frame-commit observation order, packet item models, past-only equipment
memory and independent shot/pickup decisions are implemented. The decision
experiment is accessible through the UI and isolated worker, including its own
resource calibration and checkpoint/resume. See VALIDATION_DECISIONS_RU.md.
Its weights are NOT exported to the game. Decision updates now replay old
training evidence, preserve whole-group holdouts and check old-map retention;
validation regressions cannot select the next update checkpoint. This does
not close optimal weapon choice, item control or full gameplay validation.

Next independent work is action feasibility and outcome qualification, not
another claim based on action imitation accuracy: validate weapon timing,
trace/projectile candidates and target availability with explicit unknowns;
then compare feasible actions and measured outcomes. The current next-shot
classifier is dominated by weapon persistence and cannot certify a tactical
teacher. Item labels still name types rather than unique pickups/timers.
Full dynamic tricks, donor dialogue review, clean-machine packaging and
AMD/Intel hardware validation remain open. These are not completed by the
decision/replay milestone, and no game-module integration is authorized here.

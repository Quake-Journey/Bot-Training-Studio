# OpenTDM-X / Bot Training Studio: automatic map baseline and continual experience

Date: 2026-10-08. Requirement owner: ly. Status: implementation specification, NOT an implemented feature or gameplay qualification.

Revision: **r4 / Beta 12 assignment**. Retains the r2 technical review and r3 map-only/map-plus-demo workflows. Adds explicit PO authorization for Beta 12, strict prepared-map exclusion, donor-independent server knowledge and bounded in-match persistence. This is a specification, not an implemented capability.

**Scheduling and ownership (latest PO instruction, 2026-10-08):** q3t2 / Beta 11.2 is accepted and CLOSED. Claude is now assigned the server baseline implementation for **Beta 12 by ly**. This instruction supersedes the previous deferral and 11.1/11.2 number freezes for new development; preserve accepted 11.2 packages and reference binaries. Codex owns Bot Training Studio and local models. Claude supplies the common native core, server/host adapters, versioned interchange and standalone contract fixtures; coordinate the Studio consumer with Codex rather than rewriting that application. Native local checks belong to implementation; interactive PO server launch, production deployment, new Ubuntu compilation and publication remain separate actions under project rules. Do not infer permission for overnight hours of matches from earlier sessions.

## 0. Beta 12 selection and sharing contract (r4, normative)

SEL-01: Resolve knowledge at map load after effective rules/entities are available, before any bot begins planning. Use exactly one map-level mode: PREPARED, BASELINE_RESUME, BASELINE_DISCOVER, or BLOCKED_WITH_REASON. The choice is independent of bot nickname, donor, skill, style, roster order and which human is playing.

SEL-02: **A compatible prepared map package disables baseline for that map.** q2duel5, ztn2dm3 and q3t2 from accepted 11.2 are initial protected examples, not a hard-coded three-name exemption. Any later prepared compatible map package has the same priority. In PREPARED mode there is no baseline bootstrap, exploration effect, baseline match gate, baseline observation collection, post-match learner or baseline disk write. Common warmup/match rule parity still applies. Ordinary existing combat diagnostics are not baseline data.

SEL-03: Prepared means an actually decodable map-specific gameplay basis admitted by the existing loader and required runtime capabilities, not any file whose name contains the map or a global donor/chat/model file. Define the exact legacy and future package predicates before coding and record the selected file hashes. Optional absent trick files must not invalidate an otherwise accepted 11.2 map; a lone movement recording does not prove a complete ready map. Wrong BSP/rules, corrupt or partially installed prepared data is not silently accepted. Report the failure; do not blend half a prepared graph with baseline. Never mark an incompatible package ready merely to avoid learning. Distinguish absent data from invalid installation; partial/corrupt prepared installations require a visible blocked diagnostic or an explicit administrator-selected clean baseline path, not a silent fallback that hides the defect.

SEL-04: Otherwise reuse a compatible committed server baseline, or resume/build one if absent. An old saved baseline beside a newly installed prepared package is retained but inactive and cannot change prepared-map behavior. If prepared data is removed, revalidate any retained baseline against current dependencies before reuse. Never delete it as a side effect of lookup. A supported new map needs no per-map C change or human demos for the initial stage.

SHARE-01: One authoritative knowledge store/job per server map variant serves **all bots**. Every completed baseline-mode game contributes to that store, including games played by another donor, skill or style and bot-only games. Human-only games on an active baseline map may provide movement evidence too. The next bot consumes the latest accepted generation without repeating initial learning. Retain donor/style/skill/PA and participant/life/match attribution as context, not as separate graphs; do not copy one donor's preferences into every other donor. Shared experience must not merge players' trajectories or attribute a death to the wrong bot.

SHARE-02: Collection continues for the map while it remains in BASELINE mode, including after first readiness and across map rotation/restart when revisited. A single bot's disconnect, replacement, death or session reset does not end or erase map learning. Stopping collection after the first graph is built, or saving separate knowledge under each bot's name, fails the feature.

SAVE-01: Save on the **server's filesystem**, not a player's machine and not only RAM. During initial discovery periodically checkpoint bounded resumable work. During matches seal/flush bounded observation chunks incrementally, without synchronous full-graph writes or waiting for the final frag to persist everything. Record the configured flush interval and measured worst-case loss window; never claim every received event is power-loss durable. On shutdown/map change flush whatever bounded work is safely possible. A crash recovers verified chunks/partial-match physical witnesses without pretending that an interrupted match has a complete score/outcome.

SAVE-02: Persisting evidence is not activating a learned policy. A running match uses its admitted immutable generation; ordinary live hazard avoidance remains immediate. At match end evaluate the shared evidence, retain useful pending candidates, validate additions and commit atomically. The following match uses the latest accepted generation. If work is pending, keep the prior playable generation; log which specific change was consumed later. Every match gets an accounted result, including no useful additions or rejected evidence. No useless unbounded journal that the runtime never reads.

The r4 selection contract overrides earlier additive-baseline wording for prepared maps. Shared generic combat mechanisms remain common; online learning cannot rewrite global aim/combat code or change accepted prepared maps automatically.

## Кратко для ly

В Beta 12 на новой карте без готовых совместимых bot-файлов мод сам строит первичную базу геометрии, предметов и выполнимых маршрутов. Пока она не проверена, остаётся warmup: боты исследуют карту, не воюют, мигают зелёным и показывают статус обучения. Матч с ботом не начинается. После проверки база сохраняется на сервере в `opentdm-x/bots/baseline/maps/<map>/`, бот возвращается к обычной игре и сразу использует общие навыки стрельбы, ухода, добивания и контроля. Готовые карты q2duel5, ztn2dm3 и q3t2 исключены: для них baseline не запускается и наблюдения baseline не записываются.

На baseline-карте каждый матч любого бота пополняет единую базу карты; данные сохраняются порциями уже во время игры и переживают смену бота, карты и перезапуск сервера. Новые маршруты и изменения тактики сначала проверяются, затем подключаются между матчами. Увиденный прыжок ещё не считается освоенным триксом. Студия должна импортировать эти же файлы и строить первичную базу самостоятельно через общее ядро; нейросеть работает локально у пользователя, на сервере её нет. Studio и модели ведёт Codex, серверную реализацию Beta 12 — Claude.

Первичное построение в студии можно запускать без демок или сразу с демками. Без демок получается начальная структура, аналогичная серверному baseline; она упрощает и улучшает последующий разбор записей. Если демки уже выбраны, геометрическая основа и наблюдения из них объединяются в одном процессе: не нужно сначала отдельно заканчивать обучение без демок.

Первичная готовность означает пригодность к игре, а не мгновенное мастерство профессионала. Неизвестная обязательная механика, непроверенный выход с респавна или невозможность пройти к важным предметам не должны скрываться сообщением «обучение завершено». Для обычных поддерживаемых карт ручное программирование маршрутов не требуется. Warmup получает те же правила начального стека, оружия, боезапаса и доступных предметов, что и матч: выдача отсутствующего на карте рейлгана прекращается.

The normative implementation requirements follow in English for Claude. All configuration/API/file names proposed below are design names, not commands available in the current mod.

## 1. Outcome and boundaries

BL-01: An operator installs the official precompiled Windows DLL or Ubuntu SO and generic bot resources, loads a previously unknown supported map and enables the usual bots. No map-specific bot files, demos, Studio, developer, compiler, Python, model download, GPU or external service are prerequisites for initial playable behavior.

BL-02: With no compatible map knowledge, the mod automatically inventories the actual map, builds/qualifies useful traversals and derives an initial map-conditioned policy. Visible bots explore in warmup until the mandatory readiness gate passes. All configured donors and tactical styles use this shared map knowledge; learning is one job per map identity, not one job per bot/profile/skill.

BL-03: A compatible saved baseline is reused on subsequent visits. Human play and bot outcomes enrich it; after completed matches, validated additions become available to the next match. A map rotation, restart or crash must not erase admitted knowledge.

BL-04: The same native baseline builder, representation, validators and policy compiler serve the mod and Studio. Studio additionally trains its local models from map/demo/server evidence. Online learning is bounded experience collection, physical verification and restricted adaptation, not neural training in the server frame.

BL-05: Preserve existing map quality, donor identity, chosen default/arcade/defence/chaotic mode, skill, ping adaptation and authoritative game physics. Neither donor/style nor a low skill creates a different map graph; capabilities and action choice may differ. Existing bot-only behavior such as Strange's mercy remains isolated to its profile.

“Any map” means automatic discovery on maps whose mandatory mechanisms are supported by the official runtime, with no per-map C branches. An arbitrary custom scripted entity cannot be understood merely from its name. Detect missing capabilities, show the reason and retain partial work; do not spawn an inert fighting bot or claim full support. Adding a genuinely new mechanism requires an official capability update, not user compilation. Standard walking/crouching/jumps/drops, swimming, ladders, doors/buttons, lifts/moving platforms, teleporters and supported jump pads are mandatory scope, not optional follow-ups.

## 2. Current evidence and required changes

Inspected the bot worktree and Studio source on 2026-10-08, not just earlier reports. The original r2/r3 inspection preceded q3t2 acceptance; q3t2 / 11.2 is now closed. The r4 review rechecked map loaders/OTX_Spawn and Studio mechanism work. Record exact accepted-11.2 reference and implementation source/data identities in qualification evidence; older observations are design findings, not assertions that later code is unchanged.

- `server-mods/opentdm-x/otx.c::load_navigation/load_movement/load_connectors` loads map-named files, validates BSP correspondence and reports absent/mismatched data. This is not autonomous new-map generation. `otx_nav.c` has a bounded transactional decoder, directed routing and exposure-aware escape routing; reuse these invariants. The existing navigation file decoder admits edge flags 0..4; runtime connectors add other edges. New transition types therefore require a versioned representation/loader, not writing new meanings into old flags.
- `inc/shared/q2px_bot_host.h` exposes bot creation/input and synchronous server-thread prediction. Current Pmove prediction is useful for locomotion; it does not itself simulate every game trigger, projectile, rocket impulse, item touch or moving-world event. An engine refusal due to forecast budget is “not evaluated”, not “impossible”.
- `g_tdm_client.c::TDM_SetInitialItems`, MM_WARMUP, currently grants almost all weapons except BFG, ample ammo and body armour. The match branch ordinarily starts with blaster; ITDM has its own rail rule. This directly explains why learning from present warmup inventory is incorrect.
- Studio `worker/opentdm_x_trainer/maps.py` reads IBSP38 placement data and explicitly does not assert live availability. `native/engine/` includes native map/trace/Pmove/demo components with a source manifest. They are a starting point, not proof of complete dynamic-map simulation or a shared online builder.
- More specifically, Studio `knowledge.py::analyze` seeds cells/edges from recording states. With no recordings it provides no observation seeds; there is currently no zero-demonstration BSP-to-route builder. `physics.c::OTXF_NavSettle` rejects water and limits floor settling; `OTXF_NavLinks` checks short ordinary links with 100 ms commands and static mover stops. `mapgen_pmove.c` initializes stock parameters with `PmoveInit`, defaults gravity to 800, and explicitly does not carry a real moving ground entity through `MapGenPmove_Step`. Reusing those files unchanged cannot satisfy arbitrary rules, swimming, live lift dynamics or RJ validation.
- Current `otx.c` still guards some item, pursuit, recovery and connector decisions with q2duel5/ztn2dm3 map-name tests. For example, `otx_item_doctrine` is enabled only when the name is q2duel5. A generic graph does not automatically inherit those behaviors. Section 8 requires an explicit migration/compatibility inventory.
- The mod currently declares only the read-side prefix of `FILESYSTEM_API_V1`. The paired engine's full `filesystem_api_v1_t` supplies write/flush but no rename/replace operation. Engine-internal `FS_RenameFile*` functions are not automatically available to a game DLL. Transactional persistence therefore needs an explicit supported host operation or a crash-safe journal design; do not invent a usable rename callback.
- Existing design `design_codex_2026-10-07_bot_training_studio_runtime_contract.md` already requires a generic bounded data loader, no user compilation, physics/rules identity and atomic generations. This specification adds the online producer and continual evidence loop; it does not replace the Studio factory/user-model separation.

The original [Quake II game API](https://github.com/id-Software/Quake-2/blob/master/game/game.h) exposes traces, contents, Pmove, game-frame and entity-spawn hooks. It does not expose the engine's complete internal BSP or an isolated complete game-world simulator. Consequently, use the existing filesystem extension for bounded BSP input and explicit host adapters for authoritative rules/dynamic simulation; never cast engine-private structures. Original [trigger code](https://github.com/id-Software/Quake-2/blob/master/game/g_trigger.c) changes velocity/gravity and invokes target logic outside ordinary free-space routing. Our inference: a geometrically clear segment alone cannot certify those transitions.

These findings establish feasibility boundaries, not a measured learning duration or bot strength. No new server test was run for this specification.

Review probe: in an isolated process, the existing packaged physics DLL was opened against q2duel5 and ztn2dm3, without starting a client or dedicated server. Twenty measured calls per map checked a repeated batch of 32 local candidate links. Median/max batch times were 0.71225/0.76750 ms on q2duel5 and 0.12755/0.13920 ms on ztn2dm3. This is neither map discovery nor a slow-server benchmark, and repeated local candidates do not measure coverage. It nevertheless shows that even an existing native batch can exceed the draft 0.5 ms slice: `step()` must resume inside rollout/batch work. The review evidence records BSP/DLL hashes and scope; timings are observations on this workstation only.

## 3. Shared native core and host contract

Provide one versioned, portable C core owned with OpenTDM-X, consumed as a pinned source component by Studio. No third repository is required. Preserve provenance/licensing and source hashes. Studio must not maintain a Python rewrite of reachability or a divergent copy of physics rules. Python may orchestrate jobs and model training; C# owns the UI. Link the core into the mod and an isolated headless Studio worker; never load an arbitrary downloaded game DLL into the desktop UI.

Keep the interface small. Proposed conceptual operations:

| Operation | Contract |
|---|---|
| `open_map(snapshot, capabilities, previous)` | Validate identities; initialize/resume an immutable input snapshot and staged generation. |
| `step(budget)` | Advance resumable discovery/qualification/compilation with bounded work; return progress, yielded, blocked or complete. |
| `observe(event_batch)` | Consume bounded authoritative observations; no synchronous full rebuild. |
| `finalize_match(summary)` | Seal evidence, produce validated candidate adaptations and a report. |
| `validate_generation(candidate)` | Check format, coverage, physics evidence, policy invariants, dependencies and resource bounds. |
| `commit_generation(candidate)` | Transactionally activate at an allowed boundary; never partially install a generation. |

These are API semantics, not mandatory exact C signatures. Explicitly distinguish completion, invalid input, temporarily unavailable host budget, stale snapshot, missing capability, storage failure and cancellation. Every long operation, including decoding and commit preparation, must yield; a time check only outside one huge graph search is insufficient.

Store each search/rollout cursor and its simulated player/world state between steps. One `step` must be able to stop between candidate links and between command/physics substeps, not only between batches of dozens of links. Deterministic work-budget mode is required for comparable tests, alongside wall-clock protection. A job cannot hold a borrowed live world pointer across a yield: refresh or reject stale dynamic snapshots.

The current analyzer has process-global `world/movers/scene_trace` state in addition to thread-local Pmove bindings. For Studio multitasking, either create explicit per-job contexts in the shared core or run each native job in its own isolated worker process with a bounded scheduler. Do not invoke that existing DLL concurrently for different maps and assume thread-local Pmove alone makes it safe. In the server, there is one core context for the active map, shared by its bots.

Host adapters provide:

- Actual BSP bytes/hash and resolved entity overrides from the same search paths as the running map; bounded filesystem read/write and transactional replacement under the mod root.
- Canonical final match rules, legal item definitions, item suppression/replacement, spawn inventories, pickup/respawn semantics and movement/weapon parameters.
- Player hull/stance collision, contents and hazards including playerclip, water, slime/lava, void/trigger_hurt, local gravity and moving geometry; deterministic Pmove with the effective settings.
- Versioned typed door/lift/teleporter/pad/button events and isolated forward simulation for supported interactions. A live `edict_t *` is not an exported stable identity.
- Projectile/impulse support for rocket/grenade-assisted traversal, including legal ammo, weapon readiness, self-damage, health/armour, damage rules and landing. Pmove alone cannot validate RJ.
- Monotonic time, frame budget, actual executed inputs, event timestamps and map/session generation tokens. No engine callbacks from worker threads.

Do not impose a new engine extension when an existing documented facility suffices. Where the current API cannot provide isolated correct simulation, specify and ship the minimal official host extension/worker capability. No fake success through simplified gravity, ignored triggers, teleporting probes or infinite armour.

Native parity is an observable contract, not shared source filenames. Replay identical initial state, executed commands and world events through the actual paired runtime and the offline host; compare positions/velocities/stance, trigger activation and arrival, mover/ground state, inventory/pickups and damage at relevant substeps. Declare tolerances derived from actual coordinate/time quantization. Divergence at a required transition fails that capability. Include configured gravity, input durations, crouch/jump-held state, ground contact and effective movement parameters; do not validate only stock 800-gravity, 100 ms walking and generalize it to every server.

Studio's offline host uses the same rules evaluator and compatible native physics/collision/dynamic-entity adapters. A BSP alone does not reveal a server's cvar/rules overrides: allow an exported rules/capability snapshot, otherwise visibly use a named default rules target. Compatibility must be established against the actual runtime before export is marked installable.

## 4. Warmup and match rule parity

RULE-01: Replace separate warmup grants with one canonical effective rules evaluator consumed by warmup, initial learning, match start, respawn, item spawning/filtering, bot planning and Studio snapshots. This applies to humans and bots when the mod's bot gameplay is enabled, on known and unknown maps. Keep an intentional instagib/other mode loadout when that is the match rule; do not hard-code “never rail”.

RULE-02: Match parity means the same legal spawn inventory/health/armour/ammo and the same world weapon/item set, replacements, availability conditions, physics and pickup/respawn rules. It does not mean preserving warmup's current transient item timers, dropped weapons, corpses or scores into the match. At match start perform the normal authoritative world/inventory reset; discard transient warmup observations.

RULE-03: An item absent now because someone picked it up is not absent from the map's resource set. Separate static placement, rules-enabled resource, currently spawned entity and predicted respawn time. A human pickup during initial learning must not erase that resource or permanently poison its reachability evidence.

RULE-04: Resolve the rules snapshot after map/config/entity overrides are effective. Immediately before countdown/match start recheck the snapshot fingerprint. A meaningful change cancels the stale countdown, invalidates affected qualification and visibly rebuilds only the necessary layer. Rules must not diverge between learning and the first live match.

RULE-05: Training probes obtain special traversal resources only in an isolated simulation with legal acquisition prerequisites. Visible learning bots use the real warmup inventory, acquire resources normally and are not granted otherwise unavailable guns. No temporary all-weapons warmup that contaminates the graph. Probes do not consume humans' live items or alter live doors/movers in an offline shadow world.

## 5. Lifecycle and player-visible behavior

Map-level state progression:

```text
LOAD -> IDENTIFY -> LOOKUP
  compatible prepared package -> PREPARED (baseline OFF, no collector/writes)
  compatible generation -> bounded validation -> READY_WARMUP
  absent/incompatible -> DISCOVER -> QUALIFY -> COMPILE -> COMMIT -> READY_WARMUP
  interrupted -> CHECKPOINT -> resume applicable unfinished work
  missing capability / exhausted limit / I/O failure -> BLOCKED_WITH_REASON
READY_WARMUP -> ordinary countdown -> MATCH (collect evidence)
MATCH_END -> seal evidence -> bounded validation -> atomic next generation
```

LEARN-01: During initial DISCOVER/QUALIFY/COMPILE/COMMIT, all participating bots visibly remain in learning mode and do not fight. Use one shared scheduler to distribute useful physical exploration, not duplicate every probe for every roster bot. Discovery starts at map load; visible traversal begins when a normal bot seat is available. Do not silently consume extra playing slots or evict a human to obtain a probe.

LEARN-02: Block every path to a match while mandatory initial qualification is incomplete: player `ready`, votes/administrative start, bot auto-ready, countdown completion and scheduled bot matches. A refused start returns the phase/reason. An attempted ready is not silently queued to launch later. Administrative map change/disable-bots remains possible; ordinary human-only gameplay must not become permanently unavailable due to a failed bot job.

Use one authoritative `can_start_bot_match` readiness predicate at both `TDM_BeginCountdown` and `TDM_BeginMatch`, before state changes or level reset, plus all bot scheduler entry points. Checking only the ready console command is insufficient. The gate applies when bots would participate in the match; a human-only match does not require bot route readiness. If a human match starts while analysis is incomplete, exploration bots leave active play and discovery pauses or continues only as budgeted isolated work. Never insert an unready bot into that running match. With no bot seat, report `waiting for exploration seat` and do whatever BSP/isolated work is legal; do not fabricate a valid spawned bot for the present prediction API.

LEARN-03: Display a moderate green blinking shell on learning bots and an overhead status label: `Initial map learning` / `Первичное изучение карты`, phase and useful progress. Do not change permanent nicknames, hitboxes, aim, movement speed or gameplay physics to display it. Existing shell/effect flags must be restored correctly, not cleared indiscriminately. The current engine defines `EF_COLOR_SHELL` and `RF_SHELL_GREEN`; use their compatible rendering path for the shell. A world-anchored text label is a separate client presentation capability: implement negotiated official-client support if needed, without pretending console/centerprint alone is an overhead label. Older supported clients receive a clear HUD/console fallback and must not disconnect; document that visual limitation. MVD playback must preserve the status or its explicit fallback.

LEARN-04: On joining the server or entering play/spectating, show the current shared job status once, then rate-limited phase/progress updates. Example console semantics: map and rules target; current phase; completed/total required spawn/resource routes; unresolved mandatory transitions; resource wait reason; saved generation. Do not advertise an exact percentage/ETA before work size is known, or flood reliable messages every server frame. Overhead/HUD updates are coalesced; console defaults to phase changes and at most one progress line per five seconds.

LEARN-05: Humans can move in warmup while learning. Learning bots do not retaliate; this is the explicit user-requested exception to the ordinary combat “always return fire” requirement. They remain subject to normal physical damage and game rules. Being killed or blocked resumes exploration safely and does not restart map learning, certify a failed edge or count as a combat-policy failure. Prevent a human occupying a doorway from permanently marking that doorway unreachable.

No combat does not forbid the legal self-directed shot required for a qualified traversal/RJ probe. Such a shot must be part of the exploration action, with actual resources/damage and a clear probe area; defer it when a human makes the probe unsafe. Never aim at a human or use “training” to run combat control fire. Do not disable projectile damage or otherwise alter physical outcomes to make a probe pass.

LEARN-06: On readiness, commit the complete baseline, remove learning effects/labels, clear learning movement/input intents, restore normal inventory through a normal respawn/reset where necessary and print the saved generation/coverage. Match startup returns to the usual policy; no hidden automatic human match launch from an earlier refused ready. Normal bot-only automatic matches may resume under the existing rules.

LEARN-07: On failure/time limit, stop repeated unsafe wandering, retain resumable partial data, show the exact missing route/capability/resource/storage reason and keep affected bots non-combatant or spectators. Do not indefinitely blink with no explanation. An operator can inspect/resume/rebuild or disable automatic bot learning without deleting earlier accepted generations. No normal player can delete baseline files or force a costly rebuild.

LEARN-08: Map changes/unloads cancel old callbacks and queued probe inputs via generation tokens, save bounded work at safe points, and restore rendering/state. Human disconnect, switching opponents, donor change and tactical vote do not reset shared learning. A background post-match update for an already-ready map must not unnecessarily block the next match; if unfinished, keep the accepted generation and continue later.

## 6. Discovery and physically executable routes

DISC-01: Seed exploration from every legal player spawn, enabled important item approach and recognized transition endpoint. Derive bounded walkable/crouchable surfaces and candidate connections from native collision/BSP data; use directed edges, local refinement and spatial indexing. Do not fill the entire BSP bounding box with an unrestricted dense voxel grid or perform all-pairs traces. Recognize disconnected areas without inventing an exit.

The zero-demo bootstrap is an explicit new component, not a call to the existing observation-only analyzer. It is a required input-independent capability, not a requirement to exclude available demonstrations or finish a separate no-demo job first. When recordings are supplied initially, their attributed observations seed/refine the same frontier and transition candidates alongside collision-based discovery:

1. Inventory resolved map entities and collision support surfaces. Spawn seeds use the same mode/filter/spawn-placement adjustment as the game, not every `info_player_*` origin treated as a valid standing player. Project pickup and transition seeds onto achievable approach regions with correct hull clearance.
2. Find candidate standing/crouch supports on relevant collision surfaces, including multiple floor heights over the same XY location. World XY raycasts finding only the topmost surface are insufficient for bridges/stacked rooms.
3. Expand bounded local frontiers from those supports using spatial neighbors, legal step/crouch/jump/drop candidates and recognized transitions. Verify connections under native dynamics, storing witnessed input/control and exit conditions; do not require a human to have already visited each frontier.
4. Refine around uncovered legal spawns/resources/transitions and build both resource-to-spawn and escape connectivity. Do not let discovery stop at the first already-connected room or classify an undiscovered part as decorative.
5. Use visible bots to execute necessary exploration/qualification paths once available. Physical exploration is not random wandering until a graph happens to appear. It supplements collision-based discovery and confirms actual executor operation; it does not teleport a bot through unproved edges.

If a full collision-surface enumerator is unavailable, implement and qualify a bounded alternative that proves this same coverage. Supplying recordings to cover the gap does not satisfy no-demo initial learning. Floor settling that rejects a raw spawn origin is an unresolved seed to reconcile with actual spawn placement, not proof that the spawn is unusable.

The resulting baseline is also an analysis foundation: bind recorded positions to known regions/supports, distinguish ordinary motion from mover/teleport/pad transitions, identify resource approaches and focus physical verification on novel trajectories. Reuse previously validated unchanged structure. Do not require every observation to fit an existing edge: an off-graph valid human movement is a candidate for extension/refinement, not evidence to discard just because the initial baseline missed it. Geometry and recordings constrain and improve each other, with provenance and uncertainty preserved.

DISC-02: Classify standing/crouching movement, stairs, jumps, safe drops, ladders, swimming and water exits, doors/buttons, lift boarding/wait/riding/exits, moving platforms, teleports and jump-pad flights. Record direction, entry state, exit/landing region, duration distribution, clearance, activation/phase conditions, reversibility, resource costs, hazard exposure and recovery. A one-way drop/teleport is not a bidirectional corridor. Crossing a trigger is an event, not ordinary interpolation between distant points.

DISC-03: Native rollout must include player hull, collision masks, legal movement input/timing, quantization and relevant dynamic interactions. A standing point, region or route is not a validated pickup: verify touching the item's actual pickup volume and the resulting inventory/pickup event. For moving resources or movers, bind to stable placement/transition IDs and current phase instead of an old entity slot.

DISC-04: Qualify required ordinary links and item approaches under a defined envelope of attainable entry positions/velocities, mover states and command timing. Require repeated successful traversal and small input/position perturbations, not a single lucky trace. Advanced trick admission includes resource prerequisites, turn/run-up space, landing margin, failure recovery and credible repeatability. No assumed universal numeric threshold: record the gate and measured trials for each capability class.

DISC-05: First readiness needs safe ordinary play and all mandatory connections, not discovery of every possible professional trick. Optional tricks remain discoverable later. If a jump/RJ is the only route to an enabled strategically important region, it is mandatory connectivity and cannot be deferred behind a green status. Route feasibility includes a legal resource-acquisition sequence; a route requiring rockets unreachable without that same route is not an exit.

DISC-06: Distinguish permanent geometry failure from temporary player blockage, absent ammo, wrong mover phase, host-budget refusal or uncertain observation. Retry with bounded backoff and meaningful new conditions. No same impossible jump every frame, no oscillation between goals, no repetitive ladder up/down, no waiting indefinitely below a lift. Recover to the last verified safe region using ordinary game movement; never make teleport/noclip rescue part of a playable learned route.

DISC-07: Generate candidate overlook, cover, ambush, intercept and control-shot locations from architecture and the actual resource/spawn set. For projectile/control fire record intended impact/bounce surface, opponent emergence region, timing and uncertainty. Validate real direct/splash/bounce geometry and friendly/self risk. “Shoot at map point” is insufficient, and candidates never authorize empty rocket/grenade spam with no plausible opponent evidence.

## 7. Readiness is measured coverage

READY requires all of the following, with machine-readable results:

1. Valid identities, supported mandatory mechanisms and effective match-rule parity; bounded complete decode and no unresolved format errors.
2. Every legal spawn has a physically validated escape/recovery route into its intended playable component. Any intentional isolated component has a supported transition; a stranded spawn fails readiness.
3. Every enabled strategically important permanent weapon/item has a qualified pickup approach from the appropriate reachable component. Cover armour tiers, mega, meaningful health including small overhealth items/shards, adrenaline and ammo pack where enabled, available primary guns and their ammo. Classify importance from actual definitions/rules and map resources, never a q2duel5-only item list.
4. Required connections between spawns, combat regions and principal resource clusters work in the actual physics. Unknown required mechanisms are failures, not counted as excluded successes. Truly decorative/unreachable placements need an explicit geometric explanation rather than a silent exclusion.
5. A bot using the actual executor can acquire a useful available weapon/ammo, improve stack, move between regions, fire through a legal opportunity, retreat and recover from a rejected transition. Pure graph connectivity is insufficient.
6. Initial map policy passes the common invariants in section 8. Save generation and report; announce “basic play ready”, separately showing optional areas/tricks still unqualified.

Coverage denominators derive from the effective map, not only successfully discovered nodes. A test claiming ztn2dm3 must verify actual loaded BSP/rules and runtime map name; requested harness arguments alone are not evidence. Basic readiness is separate from a later strength/competition qualification.

## 8. General skill transfer and map-conditioned tactics

Use three separate scopes: shared gameplay mechanisms/accepted knowledge; map architecture/resources/timing/position policy; donor and selected tactical mode. The initial policy compiler applies the first to the second while preserving the third. It must run without per-map hard-coded spawn routes or waiting for demos of the new map.

Required inherited mechanics include reliable shooting and weapon readiness; lead/splash/bounce selection; moving-target and lift prediction; firing while moving/retreating; threat avoidance; close combat without gratuitous weapon-switch delays; finishing a vulnerable enemy; spawn pressure; item pickup completion and ammo acquisition; escape/recovery after respawn; interception instead of tail-following; cover/exposure and sound-aware movement. Preserve legal skill/perception/PA behavior. Baseline does not secretly turn a low-skill bot into an omniscient zero-ping shooter.

Map-conditioned priorities use actual resource value, legal pickup, reachable ETA, respawn clock, relative stack/weapons, height/exposure, opponent opportunity and match score/time. Examples that must emerge from common rules:

- Control a nearby available principal armour/gun before making a long trip after a distant respawn. Do not abandon a one-step pickup to pursue an opponent across the map. Immediate attacks on a visible spawn can coexist with nearby control.
- Acquire useful ammo/health/armour along the route without unnecessary detours; include small items that prevent a one-rail death. Do not skip an attainable item touch because the route crossed a nearby graph node. Use current pickup rules/caps; no futile collection of an item the bot cannot take.
- Defend/recontest mega or principal armour using safe positions, timing and appropriate control fire. Do not release a weak enemy directly into a known imminent stack refill without comparing kill/denial options.
- Escape exposed lower positions against an upper rail/CG/RL threat, using useful return fire. Avoid a long open-distance rocket/SSG exchange against rail; use cover/projectile peeks, a suitable weapon, an alternate route or resource recovery. Do not flee automatically from a favourable close CG engagement merely because the opponent holds rail.
- Seek safe arms/stack after an unarmed respawn; return fire while moving. Pressure a vulnerable or genuinely poorly armed opponent when a feasible finish outweighs collection; retain essential control if the enemy is far away or the chase is unsafe.

Default, arcade, defence and chaotic retain their intended distinctions. Defence can take safe items and near-certain finishes. Arcade still requires useful weapons/ammo and viable engagement resources. Baseline does not overwrite the selected mode or donor with whichever human supplied the latest observation. It supplies feasible options and map-conditioned evidence to those policies.

A shared combat improvement found on a new map is eligible for old maps only through a separately validated official common-mechanism change. Online map experience must not silently rewrite the global aim/combat algorithm. Prepared handcrafted/Studio-qualified map packages are a protected comparison basis and disable online baseline completely (section 0); do not collect or activate an additive baseline layer on those maps. Within a baseline-mode map, verified new observations extend the server baseline subject to qualification.

### Required migration and initial-policy construction

Before claiming general skill transfer, inventory every map-name/coordinate/connector-index condition on the execution path of these mechanisms. Classify it as (a) genuine geometry/resource annotation to serialize as map data, (b) a general behavior accidentally map-gated to move into the common policy, or (c) a compatibility workaround retained only for its original qualified package. Record the old condition, new mechanism/features and old/new native scenario evidence. Do not blindly delete map guards or apply a map-specific threshold everywhere; preserve accepted maps through compatibility fixtures until replacement is proven.

The initial policy builder must concretely produce a resource/control graph, route ETAs and alternatives, cover/height/exposure annotations, spawn-recovery plans and item-cluster acquisition/control candidates. The runtime evaluates them using current inventory/stack, score/time, opponent evidence and weapon suitability. Resource priorities are recomputed from the actual rules, pickup gain and ETA; a demo frequency table is not mandatory and a default coefficient vector alone is not a learned map policy.

Keep three independent outputs: route/movement feasibility, tactical goal selection and legal combat execution. A goal change or temporarily hidden opponent must not reset viable fire continuity, trigger needless weapon redraws or strand the bot in a lift-wait state. Conversely, a generic finish request cannot erase an attainable high-value pickup or turn every respawn into an unarmed frontal attack. Verify these arbitration cases explicitly with available weapon/shot/cooldown state; physical cooldown/no safe line of fire is not the same as a bot choosing silence despite a ready feasible shot.

Promote a reusable baseline feature using map-relative geometry and capabilities, not a new `if map == ...` branch. A newly selected held-out map, with no C changes or demonstrations, must exercise the same mechanisms as the previously tuned maps. Renaming a known map with identical BSP is an identity/reuse test, not sufficient evidence of new-map competence.

## 9. Live collection and post-match learning

OBS-01: On BASELINE-mode maps only, collect bounded server-side events in the normal game, without requiring MVD/verbose console logging or a spectator recorder. Record map/rules/generation, match/life boundaries, sampled actual positions/velocity/stance, relevant executed input sequences, timestamps/substeps, view/aim, inventory/ammo/stack changes, pickup/shot/damage/death events, movers/triggers and selected bot action/rejection reasons. Input received from a human is not necessarily the input already executed; retain authoritative execution order and uncertainty. Capture useful movements by all relevant humans, not only the bot's current duel opponent, with participant separation. PREPARED mode does not run this collector (SEL-02).

OBS-02: Keep short rolling context around transitions/tricks, resource contests, attacks, escapes, failure/stuck episodes and meaningful control shots. Normal repetitive motion becomes compact counters/coverage updates. Deduplicate episodes and put limits on participants, sample frequency, duration, event rate and total pending bytes. No multi-gigabyte full-motion traces by default. Nickname/IP/chat is not required for baseline mechanics; use session-local participant IDs, omit addresses/secrets, and do not silently turn map learning into donor/chat harvesting.

OBS-03: Learn human traversals as candidates with entry/exit conditions. A snapshot displacement does not uniquely reveal input and is not proof of a reproducible trick. Prefer executed usercmd evidence where available; otherwise fit controls and qualify them. Exclude deaths/respawns, reconnects, map changes, admin teleports/noclip/cheats and corrupt/discontinuous samples from movement links. Correctly identify teleport/pad/mover transitions, real RJ/grenade impulses, water entry/exit and input/PA delay. A trajectory that ends in void/death is not a successful shortcut.

OBS-04: Diagnose own mistakes in context: failed item touch, stuck/reversing route, repeated deadly exposure, missed item timing, unproductive chase, excessive projectile spending, delayed viable return fire and bad weapon/position choice. Use state/action/outcome and alternative feasibility; a death alone is not proof that a route or every fight there is bad. Preserve successful counterexamples and uncertainty. Learning bots' warmup deaths and speculative simulation failures do not contaminate combat reward.

OBS-05: At match end seal an immutable event chunk, evaluate candidates through the common core, update conditional route-time/risk/success statistics and bounded map-policy preferences, then validate a candidate generation. Promote only supported, sufficiently evidenced changes. Sparse evidence uses prior/general rules; no one-game overfit, arbitrary code generation or per-opponent hard-coded exploits. Better loss, more frags against one weak opponent or a claimed causal explanation alone is insufficient.

OBS-06: Each completed match must be accounted for: accepted additions, retained candidates, rejected evidence or no useful new information, with reasons. “Learning” cannot mean merely accumulating files nobody consumes. At the next match load the newest committed compatible generation; if validation is still pending, use the last accepted one. Do not stall an otherwise ready match to retrain. Interrupted/aborted matches may contribute validated physical observations but are not represented as complete scored outcomes.

OBS-07: Running matches use an immutable installed knowledge generation. Live hazard checks and temporary failed-edge avoidance still react immediately; this is ordinary safety, not mid-match installation of an unvalidated policy. Persistent updates are activated at a warmup/map boundary and logged. Preserve a last-known-good generation and rollback/quarantine a newly observed invalid edge without destroying good unrelated knowledge.

OBS-08: Share map mechanics across donors. Preserve source attribution/confidence without replacing donor play identity. Potential general skill improvements go to a separate shared-candidate record for Studio/developer validation on old maps; do not auto-promote global behavior after a public-server match.

### Minimum executable learning mechanism

This feature needs a real bounded learner, not only a journal and not a claim that counters create understanding by themselves. For each admitted action class, define context features, observed costs/outcomes, the parameters that may change and the validation/promotion rule. At minimum:

- Route entries update conditional traversal time, collision/stall/failure evidence and recovery choices. A reproducible geometry/physics failure can quarantine a link; combat damage or temporary human blocking cannot permanently remove an essential connection.
- Human novel trajectories become executable candidates only after input fitting/replay and physical qualification; retain the actual witness and its admissible starting envelope. They can add links/alternatives, not only increment use counts.
- Item/position decisions update bounded context-conditioned preferences from pickup gain, arrival timing, exposure, damage/frag outcomes and continued escape options. Use only available evidence; an unobserved alternative outcome remains unknown. Native simulation may prove physical feasibility, but a scripted/no-response opponent does not prove tactical superiority.
- Compare proposed policy changes with the accepted policy on a retained representative scenario set and counterexamples, under the same current rules/capabilities. Hard invariants take precedence over learned preference weights. Admit changes only inside the tested context/envelope; retain prior behavior elsewhere.

Specify count/confidence thresholds and maximum update magnitude before implementation qualification, then test them on held-out human episodes. With sparse evidence, improve route witnesses/verified timings first and leave uncertain tactical weights at their prior. Never invent a negative reward for every retreat or a positive reward for every item taken; both are context-dependent. During a game, collect; after its conclusion, requalify/compile candidates incrementally and record which concrete new action/preference the next game will actually use.

Capture relevant executed usercmds at the mod's actual ClientThink/execution path, with pre/post state and effective angles/timing, and reconcile later trigger/weapon/damage events. A once-per-server-frame position trace cannot identify every short strafe/double-jump sequence. Use short per-participant rolling input buffers and seal only meaningful episodes, with explicit dropped-sample markers. Burst/packet/PA ordering and life/disconnect boundaries must survive export. A gap is missing evidence, never a shortcut inferred across it.

## 10. Persistent representation and recovery

Required root: `opentdm-x/bots/baseline/maps/<map>/`. Proposed layout (filenames/schema subject to implementation review):

```text
bots/baseline/maps/q2dm1/
  variants/<bsp-and-rules-id>/
    active.json
    generations/g000001/
      manifest.json
      world.bin
      routes.bin
      moves.bin
      tactics.bin
      statistics.bin
      coverage.json
      validation.json
    observations/                 bounded, sealed event chunks + pending candidates
    staging/                      incomplete generation/checkpoint, never active
    quarantine/                   bounded rejected-input metadata
```

Use a safe logical map name, strong BSP digest, resolved entity/rules/resource digest, physics/executor capabilities and schema versions. Account for movement/damage/weapon settings, timing/input assumptions, mode/entity overrides and generator identity. Map name alone, DLL display version or FNV compatibility checksum is insufficient. Different rule variants do not overwrite one another; reuse unchanged geometry only where its dependencies genuinely permit it.

Manifest records parent generation, active shared/factory basis IDs, payload sizes/hashes/counts, dependency/capability versions, supported/unresolved mechanisms, coverage denominators/results, validation provenance and evidence/stats summaries. Use stable logical item/transition IDs and fixed-width portable encoding with finite values, explicit bounds and byte order; never store native pointers, structure padding or absolute developer paths.

Write bounded staging payloads, verify their hashes and coverage, flush/close as appropriate, then atomically replace the active-generation pointer. Preserve the previous complete generation. All concurrent users/processes follow one-writer ownership for this map variant; a second server waits or keeps a read-only accepted snapshot, never writes racing statistics into the same file. Prefer separate baseline roots for separate independent servers, with explicit import/merge if desired.

Atomic replacement requires an actual host capability. Either add a narrow versioned, root-scoped transactional storage API to the paired engine, or specify a tested append-only commit journal with length/checksum/sequence records and a two-slot generation recovery rule. The existing read-prefix cast and `WriteFile/FlushFile` alone are not proof of atomic replacement or power-loss durability. Engine-free storage code may operate only on immutable owned buffers and validated paths; it cannot call engine APIs from an I/O thread. State the durability guarantee honestly and test process crash separately from simulated partial writes.

Avoid rewriting/copying the complete graph/world for every match. Reuse immutable payloads by digest inside the copied map root, seal bounded evidence chunks and compact statistics at controlled intervals. All references needed to import a map must be included within that map export or declared as checked dependencies; do not create invisible global objects outside `baseline/maps/<map>/` that break folder copying. Cap generation history/parent depth and retain active + last-known-good references when garbage collecting unreferenced objects. When a match adds no admitted change, record its result without creating a duplicate full generation.

A crash, disk full/read-only root, malformed/truncated input, stale checkpoint or process kill must not replace good data. On restart validate and reuse the last commit; resume only checkpoints with matching dependencies. Read-only deployments may run an existing accepted baseline but must clearly report that new experience is not durable. On a first-ever map, a volatile candidate can be shown as computed, but must not report persisted readiness or silently restart the same expensive discovery every rotation. Provide a writable configured baseline root or explain the required fix.

Factory/handcrafted prepared map packages remain immutable and take precedence over baseline (section 0). Generic donor/common-skill resources remain available as a basis on an unprepared baseline map; they do not themselves exempt that map. Installing prepared data deactivates retained online baseline without deleting it. If compatibility changes, preserve old data and migrate/requalify or retain it as an importable historical variant; never silently rebase it.

Incremental, rebuild and cancel operations must be distinct. Rebuild creates a new staged generation, retaining accepted history until replacement passes; it does not delete factory knowledge, Studio projects or another map's baseline. Deletion/retention is explicit administrative policy, never a response to ordinary map loading.

## 11. Server budgets, storage and bounded failure

Do not repeat the earlier verbose-log production lag or uncontrolled temporary-data growth. A few bots must not create several independent full-map search trees. One map job shares a bounded budget across all bot seats; normal network/game-frame work has priority. Use time deadlines AND trace/prediction/node-expansion/event quotas, including inner loops and memory allocations. On a busy/slow server yield fairly; budget refusal does not count as a failed route.

Initial **benchmark starting points, not shipping defaults, measured capabilities or existing cvars**. Select the release limits only after the complete-work measurements below:

| Resource | Initial target / behavior |
|---|---|
| Additional discovery/qualification CPU | Start calibration at a shared 0.5 ms per server frame / 1% of the interval envelope, whichever is smaller. Measure whether this permits useful completion; select a bounded adaptive warmup budget before shipping, without borrowing unbounded time from normal bot planning. |
| In-match collection | Bounded event-ring writes; target <=0.1 ms per frame for the whole collector, with allocation-free hot paths. Measure actual whole-frame tails. |
| Post-match work | Same incremental budget by default; no unbounded synchronous compile/save at the final frag. |
| Map learning working set | Evaluate a 64 MiB planning ceiling, then choose an explicit measured hard cap and refusal of oversized maps. Account for the full working set before release; no silent graph truncation. |
| Pending observations | Start with <=16 MiB per active map variant, deduplication and rotation; never grow without a cap. |
| Persisted baseline | Target <=64 MiB per ordinary map variant, active + previous generation accounted for; default total root quota 1 GiB. Retention limits must not silently delete the only accepted baseline of a map. |
| Initial warmup duration | Evaluate a progress warning at 120 s and checkpoint/continuation near 10 min. Choose a progress-aware bounded visit limit jointly with measured CPU/workload, never promise that every map finishes in ten minutes. |

**Review correction:** these draft numbers must not be frozen as simultaneous shipping guarantees. At 10 server frames/s, a 0.5 ms slice gives only 3 seconds of maximum compute over ten minutes, before I/O/other limits. There is no measurement showing that this can discover and robustly validate a complete unknown map. The isolated native probe above also exceeded one such slice for a single existing 32-link batch. Before selecting defaults, measure complete discovery, mandatory validation and save work on several held-out maps and a weak CPU; estimate completion using measured work and the permitted slice. Choose a bounded adaptive warmup budget and timeout/checkpoint policy together. Keep optional refinement deferrable and permit administrative continuation from a checkpoint. Do not abort a supported map at an arbitrary ten-minute threshold and present that as successful autonomous learning, or silently raise the budget until the server lags.

Memory/disk accounting includes parsed BSP/collision state, dynamic snapshots, frontier queues, route caches, rollout states, pending event rings and staging/previous payloads, not only the final graph. One configured limit may not be counted separately for every bot/variant. If the complete minimum working set exceeds the ceiling, report the measured requirement before starting expensive work; no dropping mandatory items/spawns to fit. The runtime already owns map collision state; explicitly account for any extra parser copy instead of assuming it is free. Root quota exhaustion pauses durable updates with a visible reason; it cannot silently discard another map's only baseline.

The first limited implementation must use measured traces/rollouts to calibrate quotas. If the core cannot meet them, reduce optional exploration/refine local regions/yield; do not increase arbitrary server lag or mark omitted mandatory work complete. An individual host call must also be bounded; filesystem stalls require bounded asynchronous I/O on immutable buffers or preallocated incremental writes, with no engine callbacks off-thread. Windows/Linux replacement and durability semantics need actual tests.

Separate budgets for initial learning and later matches; avoid expensive full-map rebuilds during play. Record phase/work progress, aggregate cost/overruns, peak memory, collection drops and disk bytes. Throttle UI/status messages independently. Forecast p99/max, total module/game-frame time and network scheduling are different measurements; UDP response time alone does not measure frame cost.

## 12. Studio import, refinement and return to the server

STUDIO-01: Offer an explicit baseline-folder import. A copied `baseline/maps/<map>/` is sufficient for known metadata, geometry/routes/conditions, admitted policy and retained evidence; no original server, assistant session or developer checkout required. It is not automatically the full BSP, every historical demo or a neural checkpoint. Require the matching locally owned BSP and rules/capability target for any missing collision data or physical requalification, with a clear UI explanation.

STUDIO-02: Read factory model/common-skill bases, imported online baseline and compatible user additions with provenance. An imported prepared map package and retained baseline may be inspected/compared in Studio, but server activation follows SEL-02, not additive baseline learning on a prepared map. Deduplicate observations by stable content/event identity, show available coverage/evidence and preserve the original files. Do not arithmetically sum conflicting policies or unrelated model overlays. Pinned bases remain pinned; newer Studio factory weights do not overwrite user training.

STUDIO-03: Provide “Initial map learning” as a background operation using the same core and headless host as the server, with two supported input paths:

- Map/rules only: build the initial structure without demos, equivalent in semantics to server first-map baseline. Save reusable structure, coverage and uncertainty so recordings can be added later.
- Map/rules plus game/trick demos immediately: build/refine that same structure with recorded trajectories and events in one coordinated job. Reuse an existing compatible baseline if present. Do not force the user to complete a separate geometry-only learning job before processing these demos.

The structure stage must work independently of recordings and a live public server. It supports subsequent demo analysis and model learning; it is not a competing replacement for demo-based training. Completed regions can assist analysis while other regions are still being discovered; enforce full readiness only when qualifying a gameplay export. The isolated worker may use more CPU for search while retaining the same legal physics and validators; GPU models are optional for deeper learning, not a dependency of the shared baseline builder.

STUDIO-04: Import baseline observations into subsequent map/model training alongside game/trick DM2/MVD2. Preserve evidence uncertainty, participant/life/match boundaries and grouped train/validation splits; observations and the MVD of that same match cannot leak across splits. Fine-tune the local teacher with retained prior-map experience and old-map validation; online server counters are useful evidence, not equivalent to complete model weights.

STUDIO-05: Export a versioned, bounded qualified data generation for the precompiled compatible runtime. Validate supported capabilities, BSP/rules identity, action conditions, policy invariants and old-map behavior. No generated executable script/C/DLL, Python/Torch requirement or model service on the server. Explicitly distinguish a refined baseline generation (eligible for continued server baseline learning) from a complete prepared map package (baseline disabled by SEL-02). Do not silently change class or overwrite either layer. Installing prepared data preserves the inactive baseline; any later deliberate Studio merge/refinement requires requalification and a recorded result, never automatic online additions to PREPARED mode.

STUDIO-06: Import/export includes coverage, validation, dependencies and only chosen bounded evidence. No server credentials/IPs/operator configs or commercial map assets without the operator's explicit resource choices. Default transfer is a local folder/package copy, not an automatic cloud upload. Full private training project backup is separate from compact gameplay export.

STUDIO-07: RU/EN controls, useful tooltips, progress/cancellation/resume, background job scheduling, resource meter and confirmed exit follow existing Studio rules. Add corresponding RU/EN DOCX user-guide sections when implemented. A green import or successful model epoch cannot be labelled “map mastered” before physical/gameplay qualification.

## 13. Integration stages inside one complete feature

Implement for assigned Beta 12 in dependency order, without presenting an early slice as the finished function. Claude owns steps 1-4 and runtime/portable-core qualification in step 6. Codex owns Studio application/model integration in step 5; agree versioned API/fixtures first, without waiting for a finished tactical neural model or having Claude edit the Studio UI:

1. Freeze a runtime capability/rules snapshot and baseline schema. Establish shared-core ownership/build provenance. Make warmup/match rule parity real and test it.
2. Add shared discovery/qualification, host adapters, readiness and resumable storage. Implement first-map lifecycle, match gate and player-visible status.
3. Derive an executable map policy using existing general combat/tactics; integrate the generic package loader without new per-map C branches.
4. Implement bounded live observation, error classification, post-match adaptation/validation, activation and recovery. Prove that a new match actually uses admitted additions.
5. Integrate Studio's independent baseline builder, folder import, model-training input and qualified runtime export; preserve base/user separation.
6. Complete unknown-map and existing-map qualification, performance/failure checks and RU/EN documentation before user test/release.

One official Beta 12 runtime update introduces the generic mechanisms. Subsequent ordinary supported maps need data generation only. If a blocker requires another engine capability, report the concrete missing mechanism and implement the minimal necessary host support within the assigned ownership boundaries rather than silently shipping an inert fallback. Coordinate paired-engine/client changes with their current lane owner; do not deploy the historical engine branch over Release.

## 14. Acceptance matrix

| ID | Scenario and required evidence |
|---|---|
| A01 | Remove only map-specific candidate data in an isolated test install; load a truly unseen supported map with fixed binaries/generic profiles and no demos. Observe warmup exploration, no bot combat/countdown bypass, shell and overhead label/fallback, verified readiness and saved generation; play a real match. Select at least one test map only after the binaries are frozen to detect disguised map-name scripting. |
| A02 | Repeat map load/restart: accepted generation reused, no full discovery/effects/match delay; partial interrupted job resumes when compatible. Match/config/entity/BSP/physics changes invalidate exactly the relevant qualification. |
| A03 | Compare canonical inventories, resources, caps, item suppression/respawn and physics in warmup and match. Reproduce q3t2's absent-match-rail case; no rail grant or imaginary rail route. Also cover instagib and an intentional rules variant. |
| A04 | Verify every supported transition class: teleports/pads, lifts rising/falling/blocked/reversing, doors/buttons, ladders, water exits, safe drops, RJ with adequate/inadequate stack/ammo, hurt/void edges. False direct links, budget-refused trials and failed item touches must never pass. |
| A05 | Human joins/leaves/blocks/kills a learning bot; bot/profile/skill/style change, map rotation, failed write and admin change during countdown. No reset storm, stale job action, forced human match or permanent hidden gate. |
| A06 | A human performs a previously unknown valid trick/route. Evidence captured without verbose logging, candidate physically checked, committed after match, loaded next match and actually executed by the bot. A failed/cheated/discontinuous trick is rejected; retain reasons and counters. |
| A07 | Repeated relevant bot mistakes with known alternatives improve the bounded map choice. Preserve successful counterexamples; one unrelated death does not ban the map's only route. New evidence must be consumed, not only written. |
| A08 | Verify meaningful item/weapon/ammo acquisition, finishing, respawn recovery, cover, intercept/denial and firing continuity on the new map. Assert actual pickups/discharges/times, not merely chosen goals/button states. Include the established close-combat and exposed-lower-position regressions. |
| A09 | Existing accepted q2duel5/ztn2dm3/q3t2: PREPARED mode, baseline OFF; preserve opening/control/movement/tricks, combat, donor/style/skill/PA, and Strange-only behavior. Compare against accepted 11.2 DLL/data, not a different requested map hidden behind harness defaults. The intended warmup-rule correction has its own assertions, not an unexplained regression. |
| A10 | Copy baseline folder to a clean Studio user project: import without server/developer paths, reconstruct/prove metadata, request missing BSP honestly, refine with demos, export and consume through the official unchanged DLL/SO. Independently build a baseline from BSP/rules without prior server files. |
| A11 | Cross-platform: the same admitted data works with paired Windows/Ubuntu runtime capabilities. Test actual Linux build only when separately authorized under project rules; do not infer portability from a byte-order declaration. |
| A12 | Crash/cancel/truncation/bad hash/schema/count/path/NaN/disk full/read-only/concurrent-writer/root-quota cases preserve accepted knowledge and give usable diagnostics. Simulate slow/denied host prediction without treating it as a failed move. |
| A13 | On a weak CPU, measure learning and gameplay with one/two bots plus a human and two server instances. Show learning cost, collector cost, whole-frame p95/p99/max, misses, storage and memory against learning disabled. No unlimited debug logging. |
| A14 | Repeatable 5-minute bot-vs-bot and recorded human scenarios on unknown/known maps, with fixed seeds/settings and actual loaded-map hashes. A proposed initial battery is twelve 5-minute games; runtime permission/scheduling is obtained separately. Bot-vs-bot wins alone do not prove human-opponent strength. |
| A15 | On one baseline map, bot A learns a novel validated route, then bot B with another donor/skill/style plays. Both use the same map-store identity; B actually executes the admitted route without initial relearning. Human-only observations and bot-only matches are accounted for with correct attribution. No donor personality, style or PA is overwritten. |
| A16 | Test prepared-data precedence on the three accepted maps and an additional prepared compatible fixture: no exploration/job/collector/writes. Pre-existing baseline remains unchanged and inactive. Installing prepared data suppresses a former baseline on next safe load; removing it revalidates retained knowledge. Global donor files, corrupt/partial/mismatched map files cannot impersonate prepared readiness. |
| A17 | Kill/restart the server mid-match after a sealed evidence chunk: recover verified shared observations within the declared loss window, preserve the active generation and exclude incomplete scored outcomes. Resume discovery checkpoint separately. Confirm later match consumption, not merely file growth; dropped samples are explicit. |
| A18 | Map rotation away/back, bot replacement, disconnect/reconnect, root quota and two independent server processes: compatible knowledge persists, one writer per root/variant, no per-donor resets or cross-process file corruption. Use separate default roots for independent servers. |

Extend A01/A04/A09/A13 with: no-recording graph bootstrap; stacked floors; raw-spawn versus actual player-placement reconciliation; water/crouch/ground state; non-stock gravity and different command intervals; native/offline differential traces through trigger/mover/RJ transitions; execution of previously map-gated common rules on a genuinely different held-out map; per-substep yielding inside a slow native batch; Studio concurrent jobs on different maps; post-match compaction with a self-contained copied folder; and actual countdown/match-entry gating with a human-only escape path. These are mandatory regression cases, not optional research notes.

Extend A10 with both initial-input workflows: (a) map-only baseline, then add demos; (b) supply the same map and demos together from the start. Both must retain demonstrated valid transitions and meet the same physical/coverage gates, without duplicate learning or mandatory user sequencing. Compare region/transition association and validation work against the observation-only analyzer on held-out annotated episodes to substantiate the intended analysis-quality benefit. Include a valid novel off-graph trajectory to prove that the foundation helps interpretation without hiding new discoveries.

Release completeness requires A01-A13 plus gameplay evidence proportionate to A14; explain unsupported mechanisms or failed gates explicitly. Do not declare the feature ready while mandatory route/item access, fire continuity, persistence or Studio interoperability remain unresolved. Player-visible presentation and perceived strength still require ly's acceptance.

## 15. Decisions to validate during implementation

### r2 review findings and disposition

| Finding in the first draft | r2 correction |
|---|---|
| Existing observation-based navigation could be mistaken for a no-demo builder. | Explicit new collision/frontier bootstrap and raw-spawn reconciliation in section 6; mandatory blind-map case. |
| Shared source could be mistaken for full dynamic/physics equivalence. | Actual stock/static limits identified; runtime/offline differential contract and substep state in section 3. |
| General experience transfer was underspecified despite map-name gates in live code. | Required gate inventory, classification, common-rule migration and actual map-policy outputs in section 8. |
| Post-match learning was described by intent more than an executable process. | Context/outcome/witness/update/promotion contract and executed-input collection in section 9. |
| 0.5 ms plus a ten-minute timeout was unsupported by end-to-end measurements. | Compute envelope calculated; isolated batch probe; joint budget/completion calibration required in section 11. |
| Transactional storage presumed a callback not present in the declared filesystem API. | Explicit supported storage operation or tested crash-safe journal in section 10. |
| Frequent generations could repeatedly copy full data or depend on objects outside the copied folder. | Immutable reuse, compaction, bounded history and self-contained export references in section 10. |
| Readiness could accidentally block human-only matches or be bypassed at match start. | Central gate at both countdown and match entry, seat-wait semantics and human-only behavior in section 5. |
| Existing native process-global state conflicts with concurrent Studio jobs. | Explicit contexts or isolated native worker processes and bounded scheduling in section 3. |

Review method: re-read relevant mod, paired engine and Studio implementation; trace graph seeds, native movement limitations, map-specific guards, start/reset ordering and filesystem ABI. Run a small isolated native timing probe (not bot matches), preserve its hashes/scope, then compare all original PO requirements against the corrected specification. No gameplay code, executable, server configuration or running server was changed during this review.

These are measured design questions, not reasons to leave requested functionality out:

- Establish a physically verified map-size/transition-complexity envelope and initial learning duration on weak and normal servers. Tune the proposed budgets with evidence; no promise of universal sub-second full-map learning.
- Determine which native BSP/mover/RJ simulation components can be extracted unchanged and which require official host support. Pmove parity and dynamic-world parity must be tested separately.
- Freeze the minimum negotiated overhead-text presentation capability and legacy fallback without unnecessary packet traffic or a hard requirement to replace every third-party client.
- Select confidence thresholds and bounded map-adaptation parameters using held-out gameplay and counterexamples, not invented success percentages. Global model/core updates remain separately qualified.

Implementation deliverables: shared core and adapters; official DLL/SO loader/executor integration; first-load lifecycle and visuals; canonical rules parity; baseline schema/storage/recovery; online collector and actual post-match updates; Studio import/build/refine/export; reproducible acceptance evidence and RU/EN documentation. The result must be a usable workflow, not a standalone navigation exporter awaiting manual DLL work.

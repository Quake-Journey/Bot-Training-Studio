# OpenTDM-X / Bot Training Studio: automatic map baseline and continual experience

Date: 2026-10-08. Requirement owner: ly. Status: implementation specification, NOT an implemented feature or gameplay qualification.

**Scheduling:** implement after completion/acceptance of q3t2 and delivery of 11.2. The PO has not assigned the following release number. Do not rename the current bot, interrupt Claude's q3t2 work, launch servers or treat this document as acceptance of that work. Preparing this specification does not authorize immediate game DLL/SO changes.

## Кратко для ly

На новой карте мод сам строит первичную базу геометрии, предметов и выполнимых маршрутов. Пока она не проверена, остаётся warmup: боты исследуют карту, не воюют, мигают зелёным и показывают статус обучения. Матч не начинается. После проверки база сохраняется в `opentdm-x/bots/baseline/maps/<map>/`, бот возвращается к обычной игре и сразу использует общие навыки стрельбы, ухода, добивания и контроля.

Каждый матч добавляет наблюдения за людьми и собственными удачами/ошибками. Новые маршруты и изменения тактики сначала проверяются, затем подключаются между матчами. Увиденный прыжок ещё не считается освоенным триксом. Студия импортирует эти же файлы и умеет строить первичную базу самостоятельно через то же общее ядро; нейросеть работает локально у пользователя, на сервере её нет.

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

Inspected the current bot worktree and Studio source on 2026-10-08, not just earlier reports. Claude's q3t2 work remains active; this is a structural inspection, not a frozen implementation or release baseline. Implementations must record their exact source/data identities in the qualification evidence.

- `server-mods/opentdm-x/otx.c::load_navigation/load_movement/load_connectors` loads map-named files, validates BSP correspondence and reports absent/mismatched data. This is not autonomous new-map generation. `otx_nav.c` has a bounded transactional decoder, directed routing and exposure-aware escape routing; reuse these invariants. The existing navigation file decoder admits edge flags 0..4; runtime connectors add other edges. New transition types therefore require a versioned representation/loader, not writing new meanings into old flags.
- `inc/shared/q2px_bot_host.h` exposes bot creation/input and synchronous server-thread prediction. Current Pmove prediction is useful for locomotion; it does not itself simulate every game trigger, projectile, rocket impulse, item touch or moving-world event. An engine refusal due to forecast budget is “not evaluated”, not “impossible”.
- `g_tdm_client.c::TDM_SetInitialItems`, MM_WARMUP, currently grants almost all weapons except BFG, ample ammo and body armour. The match branch ordinarily starts with blaster; ITDM has its own rail rule. This directly explains why learning from present warmup inventory is incorrect.
- Studio `worker/opentdm_x_trainer/maps.py` reads IBSP38 placement data and explicitly does not assert live availability. `native/engine/` includes native map/trace/Pmove/demo components with a source manifest. They are a starting point, not proof of complete dynamic-map simulation or a shared online builder.
- Existing design `design_codex_2026-10-07_bot_training_studio_runtime_contract.md` already requires a generic bounded data loader, no user compilation, physics/rules identity and atomic generations. This specification adds the online producer and continual evidence loop; it does not replace the Studio factory/user-model separation.

The original [Quake II game API](https://github.com/id-Software/Quake-2/blob/master/game/game.h) exposes traces, contents, Pmove, game-frame and entity-spawn hooks. It does not expose the engine's complete internal BSP or an isolated complete game-world simulator. Consequently, use the existing filesystem extension for bounded BSP input and explicit host adapters for authoritative rules/dynamic simulation; never cast engine-private structures. Original [trigger code](https://github.com/id-Software/Quake-2/blob/master/game/g_trigger.c) changes velocity/gravity and invokes target logic outside ordinary free-space routing. Our inference: a geometrically clear segment alone cannot certify those transitions.

These findings establish feasibility boundaries, not a measured learning duration or bot strength. No new server test was run for this specification.

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

Host adapters provide:

- Actual BSP bytes/hash and resolved entity overrides from the same search paths as the running map; bounded filesystem read/write and transactional replacement under the mod root.
- Canonical final match rules, legal item definitions, item suppression/replacement, spawn inventories, pickup/respawn semantics and movement/weapon parameters.
- Player hull/stance collision, contents and hazards including playerclip, water, slime/lava, void/trigger_hurt, local gravity and moving geometry; deterministic Pmove with the effective settings.
- Versioned typed door/lift/teleporter/pad/button events and isolated forward simulation for supported interactions. A live `edict_t *` is not an exported stable identity.
- Projectile/impulse support for rocket/grenade-assisted traversal, including legal ammo, weapon readiness, self-damage, health/armour, damage rules and landing. Pmove alone cannot validate RJ.
- Monotonic time, frame budget, actual executed inputs, event timestamps and map/session generation tokens. No engine callbacks from worker threads.

Do not impose a new engine extension when an existing documented facility suffices. Where the current API cannot provide isolated correct simulation, specify and ship the minimal official host extension/worker capability. No fake success through simplified gravity, ignored triggers, teleporting probes or infinite armour.

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
  compatible generation -> bounded validation -> READY_WARMUP
  absent/incompatible -> DISCOVER -> QUALIFY -> COMPILE -> COMMIT -> READY_WARMUP
  interrupted -> CHECKPOINT -> resume applicable unfinished work
  missing capability / exhausted limit / I/O failure -> BLOCKED_WITH_REASON
READY_WARMUP -> ordinary countdown -> MATCH (collect evidence)
MATCH_END -> seal evidence -> bounded validation -> atomic next generation
```

LEARN-01: During initial DISCOVER/QUALIFY/COMPILE/COMMIT, all participating bots visibly remain in learning mode and do not fight. Use one shared scheduler to distribute useful physical exploration, not duplicate every probe for every roster bot. Discovery starts at map load; visible traversal begins when a normal bot seat is available. Do not silently consume extra playing slots or evict a human to obtain a probe.

LEARN-02: Block every path to a match while mandatory initial qualification is incomplete: player `ready`, votes/administrative start, bot auto-ready, countdown completion and scheduled bot matches. A refused start returns the phase/reason. An attempted ready is not silently queued to launch later. Administrative map change/disable-bots remains possible; ordinary human-only gameplay must not become permanently unavailable due to a failed bot job.

LEARN-03: Display a moderate green blinking shell on learning bots and an overhead status label: `Initial map learning` / `Первичное изучение карты`, phase and useful progress. Do not change permanent nicknames, hitboxes, aim, movement speed or gameplay physics to display it. Existing shell/effect flags must be restored correctly, not cleared indiscriminately. The current engine defines `EF_COLOR_SHELL` and `RF_SHELL_GREEN`; use their compatible rendering path for the shell. A world-anchored text label is a separate client presentation capability: implement negotiated official-client support if needed, without pretending console/centerprint alone is an overhead label. Older supported clients receive a clear HUD/console fallback and must not disconnect; document that visual limitation. MVD playback must preserve the status or its explicit fallback.

LEARN-04: On joining the server or entering play/spectating, show the current shared job status once, then rate-limited phase/progress updates. Example console semantics: map and rules target; current phase; completed/total required spawn/resource routes; unresolved mandatory transitions; resource wait reason; saved generation. Do not advertise an exact percentage/ETA before work size is known, or flood reliable messages every server frame. Overhead/HUD updates are coalesced; console defaults to phase changes and at most one progress line per five seconds.

LEARN-05: Humans can move in warmup while learning. Learning bots do not retaliate; this is the explicit user-requested exception to the ordinary combat “always return fire” requirement. They remain subject to normal physical damage and game rules. Being killed or blocked resumes exploration safely and does not restart map learning, certify a failed edge or count as a combat-policy failure. Prevent a human occupying a doorway from permanently marking that doorway unreachable.

LEARN-06: On readiness, commit the complete baseline, remove learning effects/labels, clear learning movement/input intents, restore normal inventory through a normal respawn/reset where necessary and print the saved generation/coverage. Match startup returns to the usual policy; no hidden automatic human match launch from an earlier refused ready. Normal bot-only automatic matches may resume under the existing rules.

LEARN-07: On failure/time limit, stop repeated unsafe wandering, retain resumable partial data, show the exact missing route/capability/resource/storage reason and keep affected bots non-combatant or spectators. Do not indefinitely blink with no explanation. An operator can inspect/resume/rebuild or disable automatic bot learning without deleting earlier accepted generations. No normal player can delete baseline files or force a costly rebuild.

LEARN-08: Map changes/unloads cancel old callbacks and queued probe inputs via generation tokens, save bounded work at safe points, and restore rendering/state. Human disconnect, switching opponents, donor change and tactical vote do not reset shared learning. A background post-match update for an already-ready map must not unnecessarily block the next match; if unfinished, keep the accepted generation and continue later.

## 6. Discovery and physically executable routes

DISC-01: Seed exploration from every legal player spawn, enabled important item approach and recognized transition endpoint. Derive bounded walkable/crouchable surfaces and candidate connections from native collision/BSP data; use directed edges, local refinement and spatial indexing. Do not fill the entire BSP bounding box with an unrestricted dense voxel grid or perform all-pairs traces. Recognize disconnected areas without inventing an exit.

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

A shared combat improvement found on a new map is eligible for old maps only after common-mechanism regression checks. Online map experience must not silently rewrite the global aim/combat algorithm. Existing handcrafted/Studio-qualified map data is a protected comparison basis, not overwritten by the first weaker automatic graph. Merge additional verified observations into a separate layer and compare before activation.

## 9. Live collection and post-match learning

OBS-01: Collect bounded server-side events in the normal game, without requiring MVD/verbose console logging or a spectator recorder. Record map/rules/generation, match/life boundaries, sampled actual positions/velocity/stance, relevant executed input sequences, timestamps/substeps, view/aim, inventory/ammo/stack changes, pickup/shot/damage/death events, movers/triggers and selected bot action/rejection reasons. Input received from a human is not necessarily the input already executed; retain authoritative execution order and uncertainty. Capture useful movements by all relevant humans, not only the bot's current duel opponent, with participant separation.

OBS-02: Keep short rolling context around transitions/tricks, resource contests, attacks, escapes, failure/stuck episodes and meaningful control shots. Normal repetitive motion becomes compact counters/coverage updates. Deduplicate episodes and put limits on participants, sample frequency, duration, event rate and total pending bytes. No multi-gigabyte full-motion traces by default. Nickname/IP/chat is not required for baseline mechanics; use session-local participant IDs, omit addresses/secrets, and do not silently turn map learning into donor/chat harvesting.

OBS-03: Learn human traversals as candidates with entry/exit conditions. A snapshot displacement does not uniquely reveal input and is not proof of a reproducible trick. Prefer executed usercmd evidence where available; otherwise fit controls and qualify them. Exclude deaths/respawns, reconnects, map changes, admin teleports/noclip/cheats and corrupt/discontinuous samples from movement links. Correctly identify teleport/pad/mover transitions, real RJ/grenade impulses, water entry/exit and input/PA delay. A trajectory that ends in void/death is not a successful shortcut.

OBS-04: Diagnose own mistakes in context: failed item touch, stuck/reversing route, repeated deadly exposure, missed item timing, unproductive chase, excessive projectile spending, delayed viable return fire and bad weapon/position choice. Use state/action/outcome and alternative feasibility; a death alone is not proof that a route or every fight there is bad. Preserve successful counterexamples and uncertainty. Learning bots' warmup deaths and speculative simulation failures do not contaminate combat reward.

OBS-05: At match end seal an immutable event chunk, evaluate candidates through the common core, update conditional route-time/risk/success statistics and bounded map-policy preferences, then validate a candidate generation. Promote only supported, sufficiently evidenced changes. Sparse evidence uses prior/general rules; no one-game overfit, arbitrary code generation or per-opponent hard-coded exploits. Better loss, more frags against one weak opponent or a claimed causal explanation alone is insufficient.

OBS-06: Each completed match must be accounted for: accepted additions, retained candidates, rejected evidence or no useful new information, with reasons. “Learning” cannot mean merely accumulating files nobody consumes. At the next match load the newest committed compatible generation; if validation is still pending, use the last accepted one. Do not stall an otherwise ready match to retrain. Interrupted/aborted matches may contribute validated physical observations but are not represented as complete scored outcomes.

OBS-07: Running matches use an immutable installed knowledge generation. Live hazard checks and temporary failed-edge avoidance still react immediately; this is ordinary safety, not mid-match installation of an unvalidated policy. Persistent updates are activated at a warmup/map boundary and logged. Preserve a last-known-good generation and rollback/quarantine a newly observed invalid edge without destroying good unrelated knowledge.

OBS-08: Share map mechanics across donors. Preserve source attribution/confidence without replacing donor play identity. Potential general skill improvements go to a separate shared-candidate record for Studio/developer validation on old maps; do not auto-promote global behavior after a public-server match.

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

A crash, disk full/read-only root, malformed/truncated input, stale checkpoint or process kill must not replace good data. On restart validate and reuse the last commit; resume only checkpoints with matching dependencies. Read-only deployments may run an existing accepted baseline but must clearly report that new experience is not durable. On a first-ever map, a volatile candidate can be shown as computed, but must not report persisted readiness or silently restart the same expensive discovery every rotation. Provide a writable configured baseline root or explain the required fix.

Factory/handcrafted packages remain immutable. Online baseline is an additive local layer bound to its basis; an update to the mod/installed map package never silently discards it or rebases incompatible data. If compatibility changes, preserve old data and migrate/requalify or retain it as an importable historical variant.

Incremental, rebuild and cancel operations must be distinct. Rebuild creates a new staged generation, retaining accepted history until replacement passes; it does not delete factory knowledge, Studio projects or another map's baseline. Deletion/retention is explicit administrative policy, never a response to ordinary map loading.

## 11. Server budgets, storage and bounded failure

Do not repeat the earlier verbose-log production lag or uncontrolled temporary-data growth. A few bots must not create several independent full-map search trees. One map job shares a bounded budget across all bot seats; normal network/game-frame work has priority. Use time deadlines AND trace/prediction/node-expansion/event quotas, including inner loops and memory allocations. On a busy/slow server yield fairly; budget refusal does not count as a failed route.

Initial **engineering targets, not measured capabilities or existing cvars**:

| Resource | Initial target / behavior |
|---|---|
| Additional discovery/qualification CPU | At most 0.5 ms per server frame and at most 1% of the frame interval, whichever is smaller; shared across bots. Configure/tune after measurements, without borrowing unbounded time from normal bot planning. |
| In-match collection | Bounded event-ring writes; target <=0.1 ms per frame for the whole collector, with allocation-free hot paths. Measure actual whole-frame tails. |
| Post-match work | Same incremental budget by default; no unbounded synchronous compile/save at the final frag. |
| Map learning working set | Default planning ceiling 64 MiB, explicit hard cap and refusal of oversized maps. Exact required capacity must be measured before release; no silent graph truncation. |
| Pending observations | Start with <=16 MiB per active map variant, deduplication and rotation; never grow without a cap. |
| Persisted baseline | Target <=64 MiB per ordinary map variant, active + previous generation accounted for; default total root quota 1 GiB. Retention limits must not silently delete the only accepted baseline of a map. |
| Initial warmup duration | Progress/warning at 120 s; initial per-visit hard limit 10 min, checkpoint and explicit incomplete result. These are configurable engineering starting points, not a promise that every map finishes in that time. |

The first limited implementation must use measured traces/rollouts to calibrate quotas. If the core cannot meet them, reduce optional exploration/refine local regions/yield; do not increase arbitrary server lag or mark omitted mandatory work complete. An individual host call must also be bounded; filesystem stalls require bounded asynchronous I/O on immutable buffers or preallocated incremental writes, with no engine callbacks off-thread. Windows/Linux replacement and durability semantics need actual tests.

Separate budgets for initial learning and later matches; avoid expensive full-map rebuilds during play. Record phase/work progress, aggregate cost/overruns, peak memory, collection drops and disk bytes. Throttle UI/status messages independently. Forecast p99/max, total module/game-frame time and network scheduling are different measurements; UDP response time alone does not measure frame cost.

## 12. Studio import, refinement and return to the server

STUDIO-01: Offer an explicit baseline-folder import. A copied `baseline/maps/<map>/` is sufficient for known metadata, geometry/routes/conditions, admitted policy and retained evidence; no original server, assistant session or developer checkout required. It is not automatically the full BSP, every historical demo or a neural checkpoint. Require the matching locally owned BSP and rules/capability target for any missing collision data or physical requalification, with a clear UI explanation.

STUDIO-02: Read the accepted factory basis, online baseline and compatible user additions with provenance. Deduplicate observations by stable content/event identity, show available coverage/evidence and preserve the original files. Do not arithmetically sum conflicting policies or unrelated model overlays. Pinned bases remain pinned; newer Studio factory weights do not overwrite user training.

STUDIO-03: Provide “Initial map learning” as an independent background operation using the same core and headless host as the server. Studio needs no demos for the initial structure stage and no live public server. It must independently discover/qualify basics, not merely display imported routes. Its isolated worker may use more CPU for search while retaining the same legal physics and validators; GPU models are optional for deeper learning, not a dependency of the shared baseline builder.

STUDIO-04: Import baseline observations into subsequent map/model training alongside game/trick DM2/MVD2. Preserve evidence uncertainty, participant/life/match boundaries and grouped train/validation splits; observations and the MVD of that same match cannot leak across splits. Fine-tune the local teacher with retained prior-map experience and old-map validation; online server counters are useful evidence, not equivalent to complete model weights.

STUDIO-05: Export a versioned, bounded qualified data generation for the precompiled compatible runtime. Validate supported capabilities, BSP/rules identity, action conditions, policy invariants and old-map behavior. No generated executable script/C/DLL, Python/Torch requirement or model service on the server. Higher-quality Studio data must coexist with later online additions without blindly replacing either layer; conflicts require requalification and a recorded merge result.

STUDIO-06: Import/export includes coverage, validation, dependencies and only chosen bounded evidence. No server credentials/IPs/operator configs or commercial map assets without the operator's explicit resource choices. Default transfer is a local folder/package copy, not an automatic cloud upload. Full private training project backup is separate from compact gameplay export.

STUDIO-07: RU/EN controls, useful tooltips, progress/cancellation/resume, background job scheduling, resource meter and confirmed exit follow existing Studio rules. Add corresponding RU/EN DOCX user-guide sections when implemented. A green import or successful model epoch cannot be labelled “map mastered” before physical/gameplay qualification.

## 13. Integration stages inside one complete feature

Implement in dependency order after the scheduling gate, without presenting an early slice as the finished function:

1. Freeze a runtime capability/rules snapshot and baseline schema. Establish shared-core ownership/build provenance. Make warmup/match rule parity real and test it.
2. Add shared discovery/qualification, host adapters, readiness and resumable storage. Implement first-map lifecycle, match gate and player-visible status.
3. Derive an executable map policy using existing general combat/tactics; integrate the generic package loader without new per-map C branches.
4. Implement bounded live observation, error classification, post-match adaptation/validation, activation and recovery. Prove that a new match actually uses admitted additions.
5. Integrate Studio's independent baseline builder, folder import, model-training input and qualified runtime export; preserve base/user separation.
6. Complete unknown-map and existing-map qualification, performance/failure checks and RU/EN documentation before user test/release.

One official runtime update introduces the generic mechanisms. Subsequent ordinary supported maps need data generation only. If a blocker requires another engine capability, report the concrete missing mechanism and implement it within the authorized feature scope rather than silently shipping an inert fallback. No version number is assigned by this sequence.

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
| A09 | Existing accepted q2duel5/ztn2dm3 and, after acceptance, q3t2: preserve opening/control/movement/tricks, combat, donor/style/skill/PA, and Strange-only behavior. General improvements cannot weaken unrelated maps. Compare against the actual accepted DLL/data basis, not a different requested map hidden behind harness defaults. |
| A10 | Copy baseline folder to a clean Studio user project: import without server/developer paths, reconstruct/prove metadata, request missing BSP honestly, refine with demos, export and consume through the official unchanged DLL/SO. Independently build a baseline from BSP/rules without prior server files. |
| A11 | Cross-platform: the same admitted data works with paired Windows/Ubuntu runtime capabilities. Test actual Linux build only when separately authorized under project rules; do not infer portability from a byte-order declaration. |
| A12 | Crash/cancel/truncation/bad hash/schema/count/path/NaN/disk full/read-only/concurrent-writer/root-quota cases preserve accepted knowledge and give usable diagnostics. Simulate slow/denied host prediction without treating it as a failed move. |
| A13 | On a weak CPU, measure learning and gameplay with one/two bots plus a human and two server instances. Show learning cost, collector cost, whole-frame p95/p99/max, misses, storage and memory against learning disabled. No unlimited debug logging. |
| A14 | Repeatable 5-minute bot-vs-bot and recorded human scenarios on unknown/known maps, with fixed seeds/settings and actual loaded-map hashes. A proposed initial battery is twelve 5-minute games; runtime permission/scheduling is obtained separately. Bot-vs-bot wins alone do not prove human-opponent strength. |

Release completeness requires A01-A13 plus gameplay evidence proportionate to A14; explain unsupported mechanisms or failed gates explicitly. Do not declare the feature ready while mandatory route/item access, fire continuity, persistence or Studio interoperability remain unresolved. Player-visible presentation and perceived strength still require ly's acceptance.

## 15. Decisions to validate during implementation

These are measured design questions, not reasons to leave requested functionality out:

- Establish a physically verified map-size/transition-complexity envelope and initial learning duration on weak and normal servers. Tune the proposed budgets with evidence; no promise of universal sub-second full-map learning.
- Determine which native BSP/mover/RJ simulation components can be extracted unchanged and which require official host support. Pmove parity and dynamic-world parity must be tested separately.
- Freeze the minimum negotiated overhead-text presentation capability and legacy fallback without unnecessary packet traffic or a hard requirement to replace every third-party client.
- Select confidence thresholds and bounded map-adaptation parameters using held-out gameplay and counterexamples, not invented success percentages. Global model/core updates remain separately qualified.

Implementation deliverables: shared core and adapters; official DLL/SO loader/executor integration; first-load lifecycle and visuals; canonical rules parity; baseline schema/storage/recovery; online collector and actual post-match updates; Studio import/build/refine/export; reproducible acceptance evidence and RU/EN documentation. The result must be a usable workflow, not a standalone navigation exporter awaiting manual DLL work.

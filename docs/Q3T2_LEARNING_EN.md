# q3t2 learning: research milestone

8 October 2026. Source development and local model training; this document does
not announce a new application build. The published desktop remains
0.3.0-preview.4. The owner separately accepted OpenTDM-X Beta11.2 with q3t2.

## Added capabilities

- Map-independent teleport/push inventory: trigger volumes, destinations,
  launch vectors and explicit unresolved/ambiguous states.
- A separate movement-mechanism expert schema and store, preserving existing
  v3 factory models and user overlays.
- Transition recognition between network snapshots. A teleport is not an
  ordinary extreme-speed sample, a missing frame is not a teleport, and death
  followed by respawn is not one continuous trajectory.
- Observed mechanism witnesses carried in offline knowledge packages separately
  from executable route links. Observation does not automatically qualify a trick.
- Background worker jobs prepare_mechanisms/train_mechanisms, progress,
  cancellation, completed-epoch resume, factory-directory protection, bounded
  GPU OOM batch reduction and CPU fallback in automatic backend mode.
- A reproducible scripts/train_map_curriculum.py workflow for multiple projects,
  including continued main-model learning with replay/retention checks. The new
  mechanism expert currently trains fresh on the combined corpus or resumes an
  interrupted run; incremental expert-weight updates remain separate work.

These jobs are available in development worker/source scripts. No new desktop
button or updated bundled factory weights have been published in this milestone.

## Corpus and actual mechanics

Inputs: 64 verified complete current-q3t2 game files and all 29 separate trick
recordings. The game project contains 193646 combat observations. Existing
q2duel5/ztn2dm3 data remain in the curriculum. Older q3t2_b/q3t2_c recordings
and the q3t2_d alias are excluded from this run; filenames alone do not establish
geometry compatibility. The bound BSP SHA-256 is:

8d74955f318fca4c497e2378f06fa077e790262209ed1b0a9dfa181827c134ae

Six entry discs lead to two teleport destination groups; there are three push
pads. Disc and trigger bounds differ. Push velocity uses the game's factor of
10 and the full angle vector, including pitch. A pad target key alone does not
imply a target-directed ballistic launch: the accepted module's implementation
was inspected.

A player can descend onto a pad and launch within one 100-ms network interval.
The straight chord between recorded positions may miss the trigger completely.
Recognition therefore also checks the previous-velocity approach and requires
the characteristic launch impulse.

The prepared corpus contains 1754 teleport and 410 push edges, including trick
teaching inputs. All 18 expected transitions in the separate trick recordings
are recognized: 9 teleports and 9 pushes. This checks recognition, not original
usercmd reconstruction or uninterrupted native execution of complete tricks.

## Learning and evaluation

Both model families trained on an NVIDIA GPU for 30 epochs using Compact and
Balanced. Large/XL profiles remain available; this is not a four-GB VRAM ceiling.
Additional parameters are not treated as evidence of better gameplay.

The main model continued from a previous two-map model, with replay and
distillation of earlier predictions. Original weights, existing active user
stores and installed Models were preserved. The mechanism expert uses its own store.

All matches of a normalized opponent pair remain in one partition. Final q3t2
evaluation covers two validation groups and three test groups. This is limited
independent coverage: thousands of adjacent frames are not thousands of matches.
Separate trick recordings are TRAIN-only. Validation/test event prevalence and
frames are not positive-balanced. Epoch selection uses validation only.

Compact main model: q3t2 test velocity RMSE 135.47 versus the persistence
baseline's 164.62. On unchanged old-map validation: q2duel5 132.97 → 131.28;
ztn2dm3 139.58 → 138.49. Lower is better. These measure observed motion prediction,
not bot strength or tactical quality.

Compact mechanism expert, q3t2 test:

| Metric | Result |
|---|---|
| One-second destination RMSE | 160.59; linear baseline 256.08 |
| Teleport within the next second | Recall 85.0%; precision 67.4% |
| Push within the next second | Recall 60.3%; precision 51.2% |
| Teleport Brier / frequency-prior baseline | 0.0371 / 0.0738 |
| Push Brier / frequency-prior baseline | 0.0174 / 0.0214 |

These forecast observed human behavior, not the success probability of an
executable action. Precision makes the remaining false forecasts explicit.
Balanced also passes preliminary observation-head criteria; a universal advantage
of the larger model is not established. Qualification requires useful recall and
precision and positives in multiple groups, as well as improved Brier/AP.

## Offline package and remaining limits

Actual q3t2 analysis produced 1410 nodes, 7838 ordinary statically qualified
links and 125 control-shot candidates. The package retains 2235 teleport and
461 push witnesses, including observations outside annotated duel epochs.
These broader evidence counts differ intentionally from supervised dataset counts.

The package was compiled and read by its validator. runtime_qualified=false
and server_installable=false; the current mod cannot use it. Mechanism labels,
static geometry and neural forecasts do not prove a complete dynamic route.
Unobserved states remain unknown.

Weapon selection, exact item control and resource-amount prediction still fail
their simple-baseline comparisons; those main-model outputs remain excluded from
the exported prior. The next tactical stage needs feasible action candidates,
known ammo/inventory, confirmed pickups/timers, shot validation and complete
native mechanism execution. q2duel1 is not yet included. This research does not
certify AMD/Intel execution or a new end-user application package.

No game DLL, server, operator config, Mapgen Studio or published archive changed.
The model runs offline. A future official mod will consume bounded validated
data; running a neural model in the server's real-time loop is not planned.

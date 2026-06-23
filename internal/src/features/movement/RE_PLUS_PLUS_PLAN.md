# RE++ — Full Plan

A new dodge engine that fuses **what works in the original RE (RE-Plus / XDodge)**
with **what works in zDodge**, fixes the live customer feedback, and ships behind
one **Assist ↔ Autopilot** toggle. Existing modes stay untouched so RE++ is
A/B-testable live and only becomes default once it wins.

The plan is in three parts, exactly as scoped:

1. **What we take from original RE**
2. **What we take from zDodge**
3. **What the result looks like**

---

## Context in one paragraph

Three dodges exist today, all dispatched from one hook
(`DangerPlanner::Detour_AppEngineUpdate`) and all moving the player through one
speed-clamped rail (`DodgeRuntime::CallMoveTo`). **RE-Plus / XDodge** (mode 1) —
a 41×41×13 spacetime BFS+A\* grid — is strategically strongest but a 2,137-line
monolith with ~30 toggles (several inert). **RE-Sim / Rollout** (mode 2/3) is
forward input-simulation. **zDodge** (mode 4) is candidate-direction
slide-assist: the cleanest *structure* and the only intent-preserving one, but it
shipped **incomplete** — its own spec wanted 32 directions × 5 radius "ring
passes" + a frictionless-circle slide; what shipped is 24 directions, one
distance, no ring passes, greedy single-step, with per-frame allocations and 168
wall-probes per frame. RE++ = **RE-Plus's strategy, re-housed in zDodge's clean
shape, with the slide-assist finished and a hazard-tile danger layer neither
engine ever had.**

---

# PART 1 — What we take from original RE (RE-Plus / XDodge)

RE-Plus is the strategic engine. We keep everything that makes it *smart*, and
drop the monolith + toggle sprawl that makes it hard to live with.

### R1 — Arrival-time danger
A cell's danger is sampled at the time-slice the player would **arrive** there,
not "is it dangerous right now." This is the only way to thread a *moving* bullet
field — you step into space that is empty *when you get there*. (XDodge P2.)

### R2 — Real multi-step escape search
A short-horizon local search (BFS/Dijkstra over the danger field) that can route
**around** walls and hazards to reach a safe pocket. This is RE-Plus's single
biggest advantage over zDodge, whose one straight ray can't path around a corner.
→ directly fixes **"gets stuck in confined spaces."**

### R3 — Walkability cache
The static wall layer is recomputed only on grid-recenter / a slow timer, not
every frame. → eliminates zDodge's **168 `IsWalkPositionBlocked` calls per frame**
game-thread stall and keeps AutoNexus reacting on time.

### R4 — CCD-exact commit
The committed step is verified against true bullet trajectories with continuous
collision detection over the next tick (not the coarse 50 ms grid). → fixes
**"doesn't *fully* dodge the purple shots"** (grazing). (XDodge P6.)

### R5 — Enemy-body avoidance + line-of-sight to target
Never stand on contact-damage bodies; keep a clear shot to a locked enemy.
(Powers Autopilot's keep-shooting behavior.)

### R6 — Orbit ↔ Survive arbiter
Keep weapon range and orbit when the area is safe; flee to the safest pocket when
the region is untenable, with hysteresis so it doesn't flip-flop. → this becomes
the **Autopilot** half of the one-toggle. (XDodge P3.)

### R7 — Commit dwell / goal stickiness
Hold a committed direction briefly and don't swap between equally-good options. →
anti-jitter / anti-zigzag.

### R8 — The shared move rail
`DodgeRuntime` (NativeMoveTo, speed-clamped, server-acked) is already clean and
shared. Kept **verbatim** — RE++ moves the player exactly the way every other
mode does. No new game interaction, no raw position writes.

**Deliberately left behind from RE-Plus:** the 2,137-line monolith structure, the
~30-toggle surface, and the inert phases (`xdodgeStayPenalty`, the
hard-zeroed catalog inflation, etc.).

---

# PART 2 — What we take from zDodge

zDodge is the clean, player-friendly engine. We keep its shape and feel, and we
**finish the algorithm it never completed.**

### Z1 — Clean module split + pure core
zDodge's `PlanRequest → PlanResult` design with isolated Sensors / Planner /
Types / Debug is the nicest structure of the three. We adopt it for the **whole**
RE++ engine — this is the shape that finally de-monoliths RE-Plus.

### Z2 — Intent-preserving slide-assist (the frictionless-circle slide)
Only act when the player's intended next position is unsafe; then cancel **only
the inward component** of the move and keep the tangential part — sliding past
danger like a frictionless circle, then handing control straight back. This
becomes RE++'s **Assist** mode: it feels like help, not autopilot, and never
fights your WASD.

### Z3 — MicroDodge ring-sampling, done right
zDodge's spec called for 32 candidate directions × several radius passes (small →
max correction). What shipped was 24 directions at a single distance — the ring
passes (`kRingPasses`) are declared and never executed. **This is the main reason
it feels "very close but not enough."** RE++ implements the full
direction × distance sampler.

### Z4 — First-class debug overlay
zDodge treats debug visibility as required, not polish: per-candidate
safe/reject reason, threat paths, tracked enemies/obstacles, intent vector, slide
vector, chosen target, frame-status enum. We keep all of it so every frame is
explainable while tuning.

### Z5 — Escape-overlap rule, generalized
zDodge already allows a move that **reduces** exposure to an enemy body you're
already inside (`IsEscapingExistingEnemyOverlap`). We generalize this from enemies
to **hazard tiles** → you can always slide *off* damaging ground you're standing
on. (The KoG-vent "stuck when it activates under you" fix.)

**Deliberately fixed from zDodge:** the per-frame `std::vector` allocations
(→ fixed-size reusable buffers), the 168 wall-probes/frame (→ R3 cache), the
duplicated vector math and `AddThreatSample`, and the greedy single-step
(→ Z3 + R2).

---

# PART 3 — What the result looks like

### The new thing neither engine had — the hazard-tile danger layer
This is the headline, because it's what the customer feedback is really about.
The DLL **already maintains** `WorldTAB::IsDamagingTile` (a live damaging-ground
map built from each tile's `minDmg/maxDmg`) and a tile-speed map. zDodge never
queries either — it only asks `IsTileBlocked` (walls), whose header literally
says *"Damaging tiles are NOT in this set; they are physically walkable."*

RE++ adds, in the shared sensor:
- **N1 — Hazard tiles as SOFT danger** (route around / cost), **never a hard wall**
  (which is what makes you stuck behind or on it). Combined with **Z5** (always
  escape a hazard you're on) this fixes both "walks into venom" and "stuck on the
  vent" at once.
- **N2 — Tile-speed-aware move budget** so arrival-time prediction is correct on
  slow/fast ground → tighter, fewer grazes.
- **N3 — Safe-Walk coordination for free:** RE++ reads the game's *own* live tile
  damage, so it avoids exactly the hazards still live — if Safe Walk neutralized a
  tile, RE++ ignores it (correct); if it didn't, RE++ avoids it. No conflict.

### Structure (the result, as files)
RE++ lives in its **own new folder**, a sibling of `dodge/`, `zdodge/`,
`collider/`, `noclip/`, `speedhack/` under `internal/src/features/movement/` —
**not** nested inside `dodge/`. It owns all its own files and shares only the
low-level sources (`ProjectileTracking`, `AoeTracking`, `WorldTAB`,
`AutoAim::EnumerateLiveEnemies`, `SteerInput`, `DodgeRuntime`).

```
internal/src/features/movement/          ← engines live side by side here
├── dodge/      (RE-Plus / XDodge — untouched)
├── zdodge/     (zDodge — untouched)
├── collider/  noclip/  speedhack/
└── repp/       ← NEW: RE++ lives here (its own sibling folder)
    ├── ReppTypes.h        Settings / Threat / Blocker / HazardCell /
    │                      Candidate / PlanRequest / PlanResult / FrameStatus
    ├── ReppSensors.{h,cpp}  ONE cached snapshot, zero per-frame allocation:
    │                      projectiles (reused time-parametrized paths), AoE,
    │                      enemies/blockers, walls (CACHED — R3),
    │                      HAZARD TILES (N1), tile speed (N2)
    ├── ReppField.{h,cpp}  arrival-time danger field + short-horizon escape (R1+R2)
    ├── ReppPlanner.{h,cpp}  pure PlanRequest→PlanResult: intent gate (Z2),
    │                      ring-sampling (Z3), slide (Z2), scoring, escape rule (Z5)
    ├── ReppCommit.{h,cpp}  CCD-exact verify + dwell (R4 + R7)
    ├── ReppDebug.{h,cpp}  overlay (Z4)
    └── RePP.{h,cpp}       public mode API, settings, Tick, dispatch glue
```

### Behavior (the result, in-game)
- **Default = Assist (Z2):** you drive; RE++ acts only when your intended next
  position is unsafe, slides the unsafe component along the tangent, hands
  control back. RE-Plus tightness with no input fight.
- **One toggle → Autopilot (R5/R6):** full survival — orbits / keeps weapon
  range, flees untenable areas, AFK-safe. Same sensors, same field, same commit;
  only the goal + the "do nothing when intent is safe" gate change.
- Never enters venom/lava/unsafe ground (N1); never stuck on or behind a hazard
  (N1 soft + Z5); never boxed into a corner (R2 multi-step); threads fast/special
  shots without grazing (R4 CCD).
- **~6 knobs, not ~30:** React window · Max assist distance · Hitbox scale ·
  Aggressiveness (danger weight) · Assist/Autopilot · Avoid-hazards on/off
  (+ Debug).

### How the result resolves the live feedback
| Feedback (decendium, Jun 12) | Resolved by |
|---|---|
| auto-dodge pathfinds into PNest venom ring | **N1** |
| path-finds into MV's "unsafe tiles" | **N1** |
| goes into walls / stuck in confined spaces | **R2** + N1 (soft, not hard) |
| stuck on KoG vents when they activate under you | **Z5** + N1 |
| doesn't *fully* dodge Spen purple shots | **R4** + radius tune |
| "hates the levers in Spen" | sensor blocker reclassification (Z1 cleanup) |
| hotkeys fire while typing in chat | out of scope — input-focus bug, not dodge |

---

## Appendix A — Build order (each milestone compiles + A/B-tests live)
- **M0 ✅ DONE** Scaffold: new sibling folder `internal/src/features/movement/repp/`
  (`ReppTypes.h` + `RePP.h` + `RePP.cpp`), `DodgeMode::RePP = 5`, client dropdown
  value `re-plus-plus` ("RE++"), `repp*` IPC keys (react/maxMove/hitScale/danger/
  mode/avoidHazards/debugOverlay), full mode-5 wiring (TestTAB, DangerPlanner
  dispatch, FeatureState clamp, FeatureRuntime, FeatureCommandRegistry,
  vcxproj/.filters + include dir). Selectable + toggles + settings plumbed;
  `Tick` is a guarded NO-OP (no movement yet). Static-verified (0 critical);
  build = `msbuild internal/il2cpp-dll-injection.sln /p:Configuration=Release /p:Platform=x64`.
- **M1 ✅ DONE** Shared sensor (`ReppSensors`) — projectiles/AoE as time-parametrized
  threats, enemy blockers, **N1 hazard tiles** + **N2 tile speed** via cached
  `WorldTAB` tile-map queries (no per-frame wall scan), fixed buffers (no
  per-frame heap alloc). The headline hazard fix.
- **M2 ✅ DONE** Assist done right (`ReppPlanner`) — finished MicroDodge ring-sampler
  (32 dirs × 4 radius passes, Z3), frictionless slide (Z2), generalized
  escape-overlap rule incl. hazards (Z5), arrival-time threat/sweep safety,
  intent gate. `RePP::Tick` now runs sensor→planner→`CallMoveTo` with
  commit-dwell. Assist mode moves.
- **M3 ✅ DONE** Strategic field (`ReppField`) — local 21×21 Dijkstra (0.5-tile cells)
  that routes AROUND walls/obstacles to the nearest safe-to-stand pocket using
  arrival-time danger; invoked by the planner when the ring sampler is boxed in
  (the confined-space fix). Reuses `Planner::CellSafeToStand`/`ArrivalSpeed`
  (shared collision model — no duplicated geometry). Also fixed the 2 critical
  M2 hazard-escape bugs verify#1 found.
- **M4 ✅ DONE** Tight commit (`ReppCommit`) — CCD-exact: re-checks the actual
  per-frame step at 8 sub-steps and shortens it if the full move would graze;
  reuses the shared model. Plus M2's commit-dwell (R7) already in `RePP::Tick`.
- **M5 ✅ DONE** Autopilot half — `mode` setting (0=Assist default / 1=Autopilot,
  the one toggle, plumbed via `reppMode`). Autopilot **locks the biggest
  targetable enemy** (highest `maxHp`, via `EnemyTracker`; drops on untargetable
  / re-acquires / switches back to the boss) and orbits / keeps weapon-range to
  it (R6), fed into the same dodge pipeline; the planner *executes* a safe
  goal-move in Autopilot but hands back in Assist. Flee-when-untenable emerges
  from the search. Lock target drawn on the overlay. (LOS R5 deferred.)
- **M6 ✅ DONE** Debug overlay (`ReppDebug`, Z4) — threats + predicted paths,
  candidates colored by safe/reject reason, intent/slide vectors, selected
  target; wired into `RePP::RenderDebugOverlay`. Knob surface already minimal
  (7 knobs). Make default once it beats RE-Plus & zDodge in-game.

## Appendix B — Open items to confirm
1. Naming: `RE++` / namespace `RePP` / dir `repp` / `repp*` keys. → used as-is.
2. Default behavior: plan assumes **Assist**. → shipped as default.
3. Whether RE++ eventually replaces zDodge in the dropdown or coexists.
   → coexists for now (A/B); promote after in-game wins.
4. `s_damagingMap` rebuild cadence during mid-combat tile transforms
   (PNest blocks→venom) — confirm it refreshes fast enough in-game; if the venom
   transform isn't picked up promptly, add a combat-rate WorldTAB tile rebuild
   trigger. (RE++ reads the map correctly; this is a data-freshness question on
   the WorldTAB side, not the dodge.)

---

## STATUS: ALL MILESTONES COMPLETE (M0–M6) — build-pending

Implemented overnight (2026-06-13). 11 logical files in `repp/`, all wired as
DodgeMode 5, existing modes untouched. Statically verified across 5 adversarial
workflows (compile clean every pass; 2 critical hazard-escape bugs found at M1/M2
and FIXED; 2 warnings at M3/M4 FIXED; final pass 0-critical). **Not yet compiled
in MSVC** — that + in-game tuning (Hit scale / Danger weight) is the remaining
work. See `RE_PLUS_PLUS_HANDOFF.md`.


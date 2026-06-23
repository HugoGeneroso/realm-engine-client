#pragma once

// BootGate — the self-healing boot/discovery loop (AUTO_RESOLVE_PLAN Phase 0 / A0).
//
// Ticked once per frame from dPresent. It runs every resolution step in order —
// wait for the world, resolve offsets by name, audit against the dependency
// ledger + live values, detect a game update, optionally recover, and gate
// feature installs on the result — so a stale offset after a patch can never
// silently drive a broken feature. The state is the single source of truth the
// loading screen / Quest Board (Phase 3 UI) and every feature Install() read.
//
// A0 ships the full loop with name-pass resolution + auditing + gating. The
// structural auto-resolver (A1) and the discovery UI (A2) plug into the seams
// here (RunStructuralRecovery / GetProgress / Current) without reshaping the loop.
namespace BootGate {

    // The boot state machine. Advances strictly forward except a re-audit can be
    // requested (Recheck) after a world reload.
    enum class State {
        WaitingForMetadata, // IL2CPP up but not in a world yet (classes lazy-load)
        Resolving,          // RuntimeOffsets::EnsureAll() name pass running
        Auditing,           // classify every critical anchor OK / stale vs the ledger
        UpdateDetected,     // a critical dep is stale -> consent prompt ("patch your game?")
        Discovery,          // user opted in -> structural recovery (A1) runs
        Ready,              // settled; healthy features may install, degraded stay off
    };

    // Drive the loop one frame. Returns the (possibly advanced) current state.
    State Tick();
    // Current state without advancing (UI / gate reads).
    State Current();
    // Force a re-audit on the next Tick (e.g. after a world/character reload).
    void  RequestRecheck();

    // ── Feature gate ─────────────────────────────────────────────────────────
    // May this feature install its hooks yet? Fail-closed: false until Ready AND
    // every critical anchor the feature needs is healthy. Call at the top of each
    // feature Install(). An unregistered feature name is never gated (returns true).
    bool FeatureAllowed(const char* feature);

    // ── Consent surface (driven by the Phase-3 prompt) ───────────────────────
    bool UpdateAvailable();      // true while in UpdateDetected
    void EnterDiscovery();       // "Enter Discovery Mode"
    void Dismiss();              // "Not now" -> Ready (degraded)
    void SetAutoPatch(bool on);  // skip the prompt on future updates
    bool AutoPatch();

    // ── Progress surface (Phase-3 loading screen / Quest Board read these) ────
    // Critical anchors confirmed healthy / total critical anchors.
    void        GetProgress(int& healthy, int& total);
    // True once the audit has run at least once and a critical dep is stale.
    bool        Degraded();
    // Human-readable current step ("Resolving offsets…", "Ready", …).
    const char* StatusLine();

    // Per-anchor view for diagnostics (DiagBridge / Quest Board). Fills up to
    // maxRows rows; returns the total anchor count.
    struct AnchorView { const char* klass; const char* role; bool critical; bool stale; };
    int GetAnchorReport(AnchorView* out, int maxRows);

} // namespace BootGate

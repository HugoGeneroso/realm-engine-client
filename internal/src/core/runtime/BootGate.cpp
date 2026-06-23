#include "pch-il2cpp.h"
#include "BootGate.h"
#include "RuntimeOffsets.h"
#include "LocalPlayer.h"
#include "GameState.h"
#include "DbgFileLog.h"

#include <cstring>
#include <cstdio>

// Forward-declared to avoid pulling the GUI-heavy WorldTAB.h into core/runtime.
namespace WorldTAB { void* GetSampleTilePtr(); }

namespace BootGate {
namespace {

// ── The critical anchor registry (Tier-A of the completeness ledger) ─────────
// Recovery is per-class, so the gate tracks anchor classes, not the 126 fields.
// The `klass` strings are the CURRENT obfuscated names — used only as a hint and
// for matching the unresolved-class list; staleness is decided structurally
// (class unresolved after settle, or a live sanity check flags its field), never
// by trusting the string. Mirror AUTO_RESOLVE_PLAN.md "dependency registry".
struct Anchor {
    const char* klass;     // obfuscated class name (hint)
    const char* role;      // human label for logs / UI
    bool        critical;  // 🔴 gates an Install(); 🟡/🟢 informational only
};

constexpr Anchor kAnchors[] = {
    { "HBEAKBIHANL", "Projectile instance", true  }, // dodge + damage
    { "LKHPPBEGNOM", "Player stats",        true  }, // HP/MaxHP/Defense (AutoNexus)
    { "HJMBOMEHGDJ", "World manager",       true  }, // enumerate everything
    { "KJMONHENJEN", "Entity base",         true  }, // position/type of every entity
    { "CMFPKCJHKKB", "Tile properties",     true  }, // damaging-tile / safe-walk
    { "BGAIOPJMHLO", "Tile instance",       true  }, // tile reads
    { "FKALGHJIADI", "Player avatar",       false }, // MP / flags
    { "GJJCEFJMNMK", "AoE throwable",       false }, // AoE dodge
    { "FHOHCELBPDO", "AoE visual",          false }, // AoE dodge fallback
    { "COEFCBBIBMC", "Show-effect",         false }, // SFX cues
};
constexpr int kAnchorCount = static_cast<int>(sizeof(kAnchors) / sizeof(kAnchors[0]));

// Feature -> needed anchors (the gate's feature->needs view, from the ledger).
struct FeatureNeeds { const char* feature; const char* needs[4]; int count; };
constexpr FeatureNeeds kFeatures[] = {
    { "ProjectileTracking", { "HBEAKBIHANL", "KJMONHENJEN" },               2 },
    { "AoeTracking",        { "GJJCEFJMNMK", "FHOHCELBPDO" },               2 },
    { "AutoNexus",          { "LKHPPBEGNOM", "HBEAKBIHANL" },               2 },
    { "SafeWalk",           { "CMFPKCJHKKB", "BGAIOPJMHLO", "KJMONHENJEN" }, 3 },
};
constexpr int kFeatureCount = static_cast<int>(sizeof(kFeatures) / sizeof(kFeatures[0]));

// ── State ────────────────────────────────────────────────────────────────────
State s_state = State::WaitingForMetadata;
bool  s_anchorStale[kAnchorCount] = {};   // per-anchor stale verdict at last audit
bool  s_audited      = false;
bool  s_userConsent  = false;             // EnterDiscovery() latched
bool  s_dismissed    = false;
bool  s_autoPatch    = false;
bool  s_liveAudited  = false;             // audit has incorporated live (in-world) values
char  s_status[128]  = "Initializing\xE2\x80\xA6";

void SetStatus(const char* s) { std::snprintf(s_status, sizeof(s_status), "%s", s); }

void EnterState(State next, const char* status) {
    if (next != s_state)
        DBG_FILE_LOG("[BootGate] state -> " << status);
    s_state = next;
    SetStatus(status);
}

// True only when a CHARACTER is genuinely loaded — pointer present AND real stats
// (maxHp > 0). The local-player pointer exists at character-select while maxHp is
// still 0, and the live SanityCheckPlayerStats would wrongly flag that 0 as a
// stale MaxHP offset. Gating the live audit on this avoids that false positive.
bool PlayerLoaded() {
    return LocalPlayer::GetPtr() != nullptr && LocalPlayer::GetMaxHP() > 0;
}

int FindAnchor(const char* klass) {
    for (int i = 0; i < kAnchorCount; ++i)
        if (std::strcmp(kAnchors[i].klass, klass) == 0) return i;
    return -1;
}

bool AnchorStale(const char* klass) {
    const int i = FindAnchor(klass);
    return i >= 0 ? s_anchorStale[i] : false;   // unknown -> don't block
}

bool AnyCriticalStale() {
    for (int i = 0; i < kAnchorCount; ++i)
        if (kAnchors[i].critical && s_anchorStale[i]) return true;
    return false;
}

// Classify every anchor stale/healthy against the ledger. An anchor is stale if
// ANY offset row it owns is unresolved (class given up), field-name-not-found, or
// flagged suspect by a live sanity check — so renamed classes AND renamed/wrong
// fields AND garbage-reading values are all caught, not just missing classes.
void Audit() {
    const bool inWorld = PlayerLoaded();

    // Live self-test (in-world only): flags HP/MaxHP/Defense suspect when a stale
    // offset reads garbage (the over-estimated-damage / stale-defense case). The
    // suspect verdict then surfaces as a stale row below.
    if (inWorld)
        RuntimeOffsets::SanityCheckPlayerStats(
            LocalPlayer::GetHP(), LocalPlayer::GetMaxHP(), LocalPlayer::GetDefense());

    for (int i = 0; i < kAnchorCount; ++i) s_anchorStale[i] = false;

    RuntimeOffsets::OffsetReportRow rows[160];
    const int total = RuntimeOffsets::GetOffsetReport(rows, 160);
    const int n = total < 160 ? total : 160;
    for (int r = 0; r < n; ++r) {
        const RuntimeOffsets::OffsetState st = rows[r].state;
        const bool bad = (st == RuntimeOffsets::OffsetState::FallbackGaveUp)
                      || (st == RuntimeOffsets::OffsetState::FallbackFieldName)
                      || (st == RuntimeOffsets::OffsetState::Suspect);
        if (!bad) continue;
        const int a = FindAnchor(rows[r].className);
        if (a >= 0) s_anchorStale[a] = true;   // Tier-B (real-named) rows don't match -> ignored
    }

    int staleCrit = 0;
    for (int i = 0; i < kAnchorCount; ++i) {
        if (!s_anchorStale[i]) continue;
        if (kAnchors[i].critical) ++staleCrit;
        DBG_FILE_LOG("[BootGate] audit: STALE anchor '" << kAnchors[i].klass
                     << "' (" << kAnchors[i].role << ")"
                     << (kAnchors[i].critical ? " [CRITICAL]" : ""));
    }
    s_audited     = true;
    s_liveAudited = inWorld;
    DBG_FILE_LOG("[BootGate] audit (" << (inWorld ? "live" : "metadata-only")
                 << "): " << staleCrit << " critical anchor(s) stale");
}

// Fold live sanity-check results into the audit once, the first time we get
// in-world (a field can resolve by name yet read garbage). Returns true if it ran.
bool LiveReauditIfNeeded() {
    if (s_liveAudited || !PlayerLoaded()) return false;
    Audit();
    return true;
}

// Structural recovery (A1 + A4): re-derive renamed classes by stable structure.
//  - AutoResolveByStructure: projectile class via its ProjectileProperties* field.
//  - RecoverFromInstance: the player + WorldManager live objects heal the
//    FKALGHJIADI→LKHPPBEGNOM→KJMONHENJEN hierarchy and HJMBOMEHGDJ via their live
//    class (gold-standard rung). No-op when those are already healthy.
// Returns the number of entries/classes healed this pass.
int RunStructuralRecovery() {
    int healed = RuntimeOffsets::AutoResolveByStructure();
    healed += RuntimeOffsets::RecoverFromInstance(LocalPlayer::GetPtr());
    healed += RuntimeOffsets::RecoverFromInstance(GameState::GetWorldMgr());
    healed += RuntimeOffsets::RecoverTileChain(WorldTAB::GetSampleTilePtr());   // BGAIOPJMHLO + CMFPKCJHKKB
    return healed;
}

} // namespace

State Tick() {
    // EnsureAll is idempotent and O(1) once settled, so it's safe to call every
    // frame; it keeps the name pass progressing while lazy classes load.
    RuntimeOffsets::EnsureAll();

    switch (s_state) {
    case State::WaitingForMetadata:
        // Class metadata is available at process start (only instances are lazy),
        // so resolution settles within ~5 s of the first EnsureAll — wait for that
        // single signal rather than a frame count or the stale-only give-up flag.
        SetStatus("Resolving offsets\xE2\x80\xA6");
        if (RuntimeOffsets::AllResolved())
            EnterState(State::Auditing, "Checking for game changes\xE2\x80\xA6");
        break;

    case State::Resolving:
        // Re-entry point after RequestRecheck.
        if (RuntimeOffsets::AllResolved())
            EnterState(State::Auditing, "Checking for game changes\xE2\x80\xA6");
        break;

    case State::Auditing:
        Audit();
        if (!AnyCriticalStale())
            EnterState(State::Ready, "Ready");
        else if (s_autoPatch || s_userConsent)
            EnterState(State::Discovery, "Discovering offsets\xE2\x80\xA6");
        else
            EnterState(State::UpdateDetected, "Game update detected");
        break;

    case State::UpdateDetected:
        LiveReauditIfNeeded();
        if (s_userConsent || s_autoPatch)
            EnterState(State::Discovery, "Discovering offsets\xE2\x80\xA6");
        else if (s_dismissed)
            EnterState(State::Ready, "Ready (degraded)");
        break;

    case State::Discovery: {
        const int healed = RunStructuralRecovery();   // A1
        RuntimeOffsets::EnsureAll();
        Audit();                                       // re-check after recovery
        DBG_FILE_LOG("[BootGate] discovery pass healed " << healed << " anchor(s)");
        EnterState(State::Ready, AnyCriticalStale() ? "Ready (degraded)" : "Ready");
        break;
    }

    case State::Ready:
        // Once in-world, fold live sanity-check results into the audit. If that
        // newly reveals a stale critical dep (e.g. a damage field that resolved by
        // name but reads garbage), surface the update prompt rather than running on it.
        if (LiveReauditIfNeeded() && AnyCriticalStale()
            && !s_autoPatch && !s_dismissed)
            EnterState(State::UpdateDetected, "Game update detected");
        break;
    }
    return s_state;
}

State Current() { return s_state; }

void RequestRecheck() {
    // Re-run the audit; keep any latched auto-patch preference.
    s_userConsent = false;
    s_dismissed   = false;
    s_liveAudited = false;
    EnterState(State::Resolving, "Re-checking offsets\xE2\x80\xA6");
}

bool FeatureAllowed(const char* feature) {
    if (s_state != State::Ready) return false;   // fail-closed during boot
    for (int i = 0; i < kFeatureCount; ++i) {
        if (std::strcmp(kFeatures[i].feature, feature) != 0) continue;
        for (int n = 0; n < kFeatures[i].count; ++n)
            if (AnchorStale(kFeatures[i].needs[n])) return false;  // a needed dep is stale
        return true;
    }
    return true;   // unregistered feature -> not gated
}

bool UpdateAvailable()     { return s_state == State::UpdateDetected; }
void EnterDiscovery()      { s_userConsent = true; }
void Dismiss()             { s_dismissed = true; }
void SetAutoPatch(bool on) { s_autoPatch = on; }
bool AutoPatch()           { return s_autoPatch; }

void GetProgress(int& healthy, int& total) {
    int crit = 0, ok = 0;
    for (int i = 0; i < kAnchorCount; ++i) {
        if (!kAnchors[i].critical) continue;
        ++crit;
        if (s_audited && !s_anchorStale[i]) ++ok;
    }
    healthy = ok;
    total   = crit;
}

bool Degraded() { return s_audited && AnyCriticalStale(); }

const char* StatusLine() { return s_status; }

int GetAnchorReport(AnchorView* out, int maxRows) {
    int n = 0;
    for (int i = 0; i < kAnchorCount && n < maxRows; ++i, ++n) {
        out[n].klass    = kAnchors[i].klass;
        out[n].role     = kAnchors[i].role;
        out[n].critical = kAnchors[i].critical;
        out[n].stale    = s_anchorStale[i];
    }
    return kAnchorCount;
}

} // namespace BootGate

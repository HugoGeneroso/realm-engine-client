#include "pch-il2cpp.h"

#include "EnemyTracker.h"
#include "GameState.h"
#include "RuntimeOffsets.h"
#include "Il2CppResolver.h"
#include "FeatureState.h"
#include "DbgFileLog.h"

#include <Windows.h>
#include <cstring>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

// ── Offset aliases ───────────────────────────────────────────────────────────
static const uint32_t& kOffPosX          = RuntimeOffsets::PosX;
static const uint32_t& kOffPosY          = RuntimeOffsets::PosY;
static const uint32_t& kOffHp            = RuntimeOffsets::HP;
static const uint32_t& kOffMaxHp         = RuntimeOffsets::MaxHP;
static const uint32_t& kOffObjProps      = RuntimeOffsets::ObjProps;
static const uint32_t& kOffOpInvincElem  = RuntimeOffsets::OP_InvincibleElem;
static const uint32_t& kOffObjType       = RuntimeOffsets::ObjType;
static const uint32_t& kOffWmDict        = RuntimeOffsets::WM_AllDict;

// Character mobs: dict is Dictionary<int,LKHPPBEGNOM> — live instances are often
// typed LKHPPBEGNOM at runtime, not PMMFLLAIPGN (subclass). FKALGHJIADI = local player.
static std::atomic<int> s_lastPodCount{ 0 };
static std::atomic<int> s_lastSnapCount{ 0 };
static Il2CppClass*     s_lkhKlass = nullptr;
static Il2CppClass*     s_fkKlass  = nullptr;

static void EnsureKlassCache()
{
    if (!s_lkhKlass) s_lkhKlass = Resolver::GetClass("", "LKHPPBEGNOM");
    if (!s_fkKlass)  s_fkKlass  = Resolver::GetClass("", "FKALGHJIADI");
}

// Player class types (wizard=784, etc.) are < 8192; realm mobs use 50000+.
static constexpr int32_t kRealmMobObjTypeMin = 8192;
// Unshifted HP pair on LKHPPBEGNOM (NCBIICBDGAG/KJNHLADHEMH) when ACTK fields are stale.
static constexpr uint32_t kAltMaxHp = 0x1B8;
static constexpr uint32_t kAltHp    = 0x1BC;

// IL2CPP Dictionary<int,T> layout constants
constexpr uint32_t kOffDictEnt  = 0x18;
constexpr uint32_t kOffDictCnt  = 0x20;
constexpr uint32_t kOffArrMax   = 0x18;
constexpr uint32_t kOffArrData  = 0x20;
constexpr int      kEntryStride = 24;

// ── Object type lists ────────────────────────────────────────────────────────
// Non-enemy entity types to reject outright, and whitelisted types that bypass
// the maxHp==200 decoy heuristic. Quest/fallback tiering lives in TargetSelector.
static constexpr int32_t kIgnoredTypes[]     = { 28491 };
static constexpr int32_t kWhitelistedTypes[] = { 31104 };

static bool IsIgnoredType(int32_t t) {
    for (int32_t v : kIgnoredTypes) if (v == t) return true;
    return false;
}
static bool IsWhitelistedType(int32_t t) {
    for (int32_t v : kWhitelistedTypes) if (v == t) return true;
    return false;
}

static inline bool AddrOk(const void* p) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a > 0x10000 && a < 0x7FFFFFFFFFFFULL;
}

// ── Velocity tracking ────────────────────────────────────────────────────────
struct VelEntry {
    float     x = 0.f, y = 0.f;
    ULONGLONG t = 0;
    float     vx = 0.f, vy = 0.f;
};

static std::unordered_map<int32_t, VelEntry> s_velMap;
static ULONGLONG s_pruneAt = 0;

static constexpr float kServerTickMsMin  = 115.f;
static constexpr float kServerTickMsMax  = 290.f;
static constexpr float kMaxVelTilesPerMs = 0.1f;
static constexpr float kMoVelSmooth      = 0.65f;
static constexpr float kMaxInstTilesPerMs = 0.08f;

static void UpdateVelocity(int32_t id, float ex, float ey, ULONGLONG now, void* entity)
{
    float moVx = 0.f, moVy = 0.f;
    bool  haveMo = false;
    const uint32_t velOff = RuntimeOffsets::MoVelocity;
    if (velOff != 0 && AddrOk(entity)) {
        __try {
            uint8_t* ent = reinterpret_cast<uint8_t*>(entity);
            float rvx = *reinterpret_cast<float*>(ent + velOff);
            float rvy = *reinterpret_cast<float*>(ent + velOff + 4);
            // Only use MoVelocity when it reports actual movement — the field reads
            // 0.0 on enemies when the offset is wrong or the entity is stationary,
            // which would otherwise drive chord-estimated velocity toward 0 via blending.
            if (std::isfinite(rvx) && std::isfinite(rvy) &&
                fabsf(rvx) < kMaxVelTilesPerMs && fabsf(rvy) < kMaxVelTilesPerMs &&
                (fabsf(rvx) > 1e-5f || fabsf(rvy) > 1e-5f)) {
                moVx = rvx; moVy = rvy; haveMo = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    auto it = s_velMap.find(id);
    if (it == s_velMap.end()) {
        s_velMap[id] = { ex, ey, now, haveMo ? moVx : 0.f, haveMo ? moVy : 0.f };
        return;
    }

    VelEntry& e = it->second;
    if (haveMo) {
        e.vx = e.vx * (1.f - kMoVelSmooth) + moVx * kMoVelSmooth;
        e.vy = e.vy * (1.f - kMoVelSmooth) + moVy * kMoVelSmooth;
        e.x = ex; e.y = ey; e.t = now;
        return;
    }

    const float dx = ex - e.x, dy = ey - e.y;
    const float distSq = dx * dx + dy * dy;
    e.x = ex; e.y = ey;
    if (distSq > 1e-14f) {
        // dt spans the true inter-position interval (server tick), not just the poll
        // interval, because e.t is only updated when the position actually changes.
        const float dt  = static_cast<float>(now > e.t ? (now - e.t) : 1ULL);
        const float dtC = (dt < 1.f) ? 1.f : (dt > 500.f ? 500.f : dt);
        float ivx = dx / dtC, ivy = dy / dtC;
        const float mag = sqrtf(ivx * ivx + ivy * ivy);
        if (mag > kMaxInstTilesPerMs && mag > 1e-8f) {
            const float s = kMaxInstTilesPerMs / mag;
            ivx *= s; ivy *= s;
        }
        float blend = 0.4f;
        if (dt >= kServerTickMsMin && dt <= kServerTickMsMax)
            blend = 0.9f;
        if (e.t != 0) {
            e.vx = e.vx * (1.f - blend) + ivx * blend;
            e.vy = e.vy * (1.f - blend) + ivy * blend;
        } else {
            e.vx = ivx; e.vy = ivy;
        }
        e.t = now;
    }
}

// ── World scan helpers ───────────────────────────────────────────────────────
struct DictPod { int32_t id; void* ent; };

static bool SehReadLocalPos(void* local, float* outX, float* outY)
{
    __try {
        uint8_t* lp = reinterpret_cast<uint8_t*>(local);
        *outX = *reinterpret_cast<float*>(lp + kOffPosX);
        *outY = *reinterpret_cast<float*>(lp + kOffPosY);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SehReadEntityKlass(void* entity, uintptr_t* outKlass)
{
    if (!entity || !outKlass) return false;
    __try {
        *outKlass = reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(entity));
        return *outKlass >= 0x10000u && *outKlass <= 0x7FFFFFFFFFFFu;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SehGetKlassName(uintptr_t klassRaw, char* buf, int bufLen)
{
    if (!klassRaw || !buf || bufLen <= 0) return false;
    buf[0] = 0;
    bool ok = false;
    Resolver::Protection::safe_call([&]() {
        const char* n = il2cpp_class_get_name(reinterpret_cast<Il2CppClass*>(klassRaw));
        if (n && n[0]) {
            strncpy_s(buf, static_cast<size_t>(bufLen), n, _TRUNCATE);
            ok = true;
        }
    });
    return ok;
}

static bool SehReadObjType(void* entity, int32_t* out)
{
    if (!entity || !out) return false;
    __try {
        *out = *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(entity) + kOffObjType);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SehKlassNameIs(void* entity, const char* want)
{
    if (!entity || !want || !want[0]) return false;
    uintptr_t k = 0;
    if (!SehReadEntityKlass(entity, &k)) return false;
    char name[64];
    return SehGetKlassName(k, name, sizeof(name)) && strcmp(name, want) == 0;
}

// Realm character mob (not local player, not another human player).
static bool SehIsEnemyCharacter(void* entity)
{
    if (!entity) return false;
    uintptr_t k = 0;
    if (!SehReadEntityKlass(entity, &k)) return false;

    char name[64] = {};
    if (SehGetKlassName(k, name, sizeof(name))) {
        if (strcmp(name, "FKALGHJIADI") == 0) return false;
        if (strcmp(name, "PMMFLLAIPGN") == 0) return true;
        if (strcmp(name, "LKHPPBEGNOM") == 0) {
            int32_t objType = 0;
            SehReadObjType(entity, &objType);
            if (objType > 0 && objType < kRealmMobObjTypeMin) return false;
            return true;
        }
    }

    EnsureKlassCache();
    bool isLkh = false, isFk = false;
    Resolver::Protection::safe_call([&]() {
        auto* ek = reinterpret_cast<Il2CppClass*>(k);
        if (s_lkhKlass) isLkh = il2cpp_class_is_assignable_from(s_lkhKlass, ek) != 0;
        if (s_fkKlass)  isFk  = il2cpp_class_is_assignable_from(s_fkKlass, ek) != 0;
    });
    if (!isLkh || isFk) return false;
    int32_t objType = 0;
    SehReadObjType(entity, &objType);
    if (objType > 0 && objType < kRealmMobObjTypeMin) return false;
    return true;
}

static bool SehKlassIsPlayer(void* entity)
{
    return SehKlassNameIs(entity, "FKALGHJIADI");
}

static bool SehReadHpPair(void* entity, int32_t& hp, int32_t& maxHp)
{
    hp = maxHp = 0;
    __try {
        uint8_t* ent = reinterpret_cast<uint8_t*>(entity);
        hp    = *reinterpret_cast<int32_t*>(ent + kOffHp);
        maxHp = *reinterpret_cast<int32_t*>(ent + kOffMaxHp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    if (hp > 0 && maxHp > 0 && hp <= maxHp) return true;

    __try {
        uint8_t* ent = reinterpret_cast<uint8_t*>(entity);
        const int32_t altHp    = *reinterpret_cast<int32_t*>(ent + kAltHp);
        const int32_t altMaxHp = *reinterpret_cast<int32_t*>(ent + kAltMaxHp);
        if (altHp > 0 && altMaxHp > 0 && altHp <= altMaxHp) {
            hp = altHp;
            maxHp = altMaxHp;
            return true;
        }
        constexpr uint32_t kDumpHp = 0x20C;
        constexpr uint32_t kDumpMaxHp = 0x208;
        const int32_t dumpHp    = *reinterpret_cast<int32_t*>(ent + kDumpHp);
        const int32_t dumpMaxHp = *reinterpret_cast<int32_t*>(ent + kDumpMaxHp);
        if (dumpHp > 0 && dumpMaxHp > 0 && dumpHp <= dumpMaxHp) {
            hp = dumpHp;
            maxHp = dumpMaxHp;
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// Snapshot dict under one SEH — avoids AV from iterating while IL2CPP mutates buckets.
static int SehSnapshotDict(void* wm, DictPod* out, int maxOut)
{
    if (!wm || !out || maxOut <= 0) return 0;
    __try {
        void* allDict = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(wm) + kOffWmDict);
        if (!AddrOk(allDict)) return 0;

        void* entries = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(allDict) + kOffDictEnt);
        int32_t count = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(allDict) + kOffDictCnt);
        if (!AddrOk(entries) || count <= 0) return 0;

        int32_t maxLen = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(entries) + kOffArrMax);
        int32_t limit = (count < maxLen ? count : maxLen);
        if (limit <= 0) return 0;
        if (limit > maxOut) limit = maxOut;
        if (limit > 4096) limit = 4096;

        uint8_t* base = reinterpret_cast<uint8_t*>(entries) + kOffArrData;
        int n = 0;
        for (int32_t i = 0; i < limit; ++i) {
            uint8_t* entry = base + i * kEntryStride;
            if (*reinterpret_cast<int32_t*>(entry) < 0)
                continue;
            void* ent = *reinterpret_cast<void**>(entry + 16);
            if (!AddrOk(ent)) continue;
            out[n].id  = *reinterpret_cast<int32_t*>(entry + 8);
            out[n].ent = ent;
            ++n;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

struct CandidateOut {
    int32_t id, objType, hp, maxHp;
    float   x, y;
    bool    isInvulnerable, hasHealthBar;
    void*   ptr;
};

// Character mob in the world dict (LKHPPBEGNOM / PMMFLLAIPGN, not FKALGHJIADI).
static bool SehReadEnemyMob(int32_t dictKey, void* entity, CandidateOut& out)
{
    if (!SehIsEnemyCharacter(entity)) return false;
    __try {
        uint8_t* ent = reinterpret_cast<uint8_t*>(entity);

        int32_t hp = 0, maxHp = 0;
        if (!SehReadHpPair(entity, hp, maxHp))
            return false;

        const int32_t objType = *reinterpret_cast<int32_t*>(ent + kOffObjType);
        if (!IsWhitelistedType(objType)) {
            if (maxHp == 200) return false;
            if (IsIgnoredType(objType)) return false;
        }

        uint32_t cond0 = 0, cond1 = 0;
        if (RuntimeOffsets::TryReadMapObjectConditions(entity, &cond0, &cond1)
            && (cond0 | cond1)
            && RuntimeOffsets::MapObjectConditionsMakeUntargetable(cond0, cond1))
            return false;

        const float ex2 = *reinterpret_cast<float*>(ent + kOffPosX);
        const float ey2 = *reinterpret_cast<float*>(ent + kOffPosY);
        if (!std::isfinite(ex2) || !std::isfinite(ey2) || (ex2 == 0.f && ey2 == 0.f))
            return false;

        out.id             = dictKey;
        out.objType        = objType;
        out.hp             = hp;
        out.maxHp          = maxHp;
        out.x              = ex2;
        out.y              = ey2;
        out.isInvulnerable = false;
        out.hasHealthBar   = true;
        out.ptr            = entity;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// KJMONHENJEN static/XML enemy (boss walls, etc.) — isEnemy on ObjectProperties.
static bool SehReadStaticEnemy(int32_t dictKey, void* entity, CandidateOut& out)
{
    if (SehIsEnemyCharacter(entity) || SehKlassIsPlayer(entity)) return false;
    __try {
        void* objProps = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(entity) + kOffObjProps);
        if (!AddrOk(objProps)) return false;
        uint8_t* op = reinterpret_cast<uint8_t*>(objProps);
        if (!*reinterpret_cast<uint8_t*>(op + RuntimeOffsets::OP_IsEnemy))
            return false;

        void* invPtr = *reinterpret_cast<void**>(op + kOffOpInvincElem);
        if (invPtr && AddrOk(invPtr)) return false;

        uint8_t* ent = reinterpret_cast<uint8_t*>(entity);
        int32_t hp = 0, maxHp = 0;
        if (!SehReadHpPair(entity, hp, maxHp)) return false;

        const int32_t objType = *reinterpret_cast<int32_t*>(ent + kOffObjType);
        if (!IsWhitelistedType(objType)) {
            if (maxHp == 200) return false;
            if (IsIgnoredType(objType)) return false;
        }

        const float ex2 = *reinterpret_cast<float*>(ent + kOffPosX);
        const float ey2 = *reinterpret_cast<float*>(ent + kOffPosY);
        if (!std::isfinite(ex2) || !std::isfinite(ey2)) return false;

        out.id             = dictKey;
        out.objType        = objType;
        out.hp             = hp;
        out.maxHp          = maxHp;
        out.x              = ex2;
        out.y              = ey2;
        out.isInvulnerable = false;
        out.hasHealthBar   = (*reinterpret_cast<uint8_t*>(op + RuntimeOffsets::OP_NoHealthBar) == 0);
        out.ptr            = entity;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SehReadCandidate(int32_t dictKey, void* entity, void* local, CandidateOut& out)
{
    if (!entity || entity == local) return false;
    if (SehReadEnemyMob(dictKey, entity, out)) return true;
    return SehReadStaticEnemy(dictKey, entity, out);
}

// ── Frame state ──────────────────────────────────────────────────────────────
static SRWLOCK                          s_lock = SRWLOCK_INIT;
static std::vector<EnemyTracker::Entry> s_snapshot;
static std::vector<DictPod>             s_dictScratch;
static std::atomic<int32_t>             s_localPlayerObjectId{ 0 };
static ULONGLONG                        s_lastTickMs = 0;

// Proxy NEWTICK enemy list — authoritative positions when IL2CPP dict scan finds nothing.
static void MergeWireSnapshot(ULONGLONG now)
{
    char buf[4096];
    ULONGLONG wireMs = 0;
    if (!FeatureState::CopyWireEnemySnapshot(buf, static_cast<int>(sizeof(buf)), &wireMs))
        return;
    if (buf[0] == '\0' || now - wireMs > 2500ULL)
        return;

    const int32_t localOid = FeatureState::GetClientObjectId();

    const char* seg = buf;
    while (seg && *seg) {
        int32_t id = 0, objType = 0, hp = 0, maxHp = 0;
        float x = 0.f, y = 0.f;
        const int n = sscanf_s(seg, "%d,%d,%f,%f,%d,%d",
            &id, &objType, &x, &y, &hp, &maxHp);
        if (n >= 6 && id != 0 && id != localOid
            && hp > 0 && maxHp > 0 && hp <= maxHp
            && !(maxHp < 25 && objType < 50000)
            && std::isfinite(x) && std::isfinite(y)) {
            bool found = false;
            for (EnemyTracker::Entry& e : s_snapshot) {
                if (e.id != id) continue;
                e.x = x; e.y = y; e.hp = hp; e.maxHp = maxHp; e.objType = objType;
                found = true;
                break;
            }
            if (!found) {
                EnemyTracker::Entry e{};
                e.id = id;
                e.objType = objType;
                e.x = x;
                e.y = y;
                e.hp = hp;
                e.maxHp = maxHp;
                e.isInvulnerable = false;
                e.hasHealthBar = true;
                e.ptr = nullptr;
                s_snapshot.push_back(e);
            }
        }
        const char* next = strchr(seg, '|');
        if (!next) break;
        seg = next + 1;
    }
}

static void RebuildMemorySnapshotLocked(ULONGLONG now, int& outPodCount)
{
    outPodCount = 0;

    void* local = GameState::GetLocalPtr();
    if (!local) return;

    float px = 0.f, py = 0.f;
    if (!SehReadLocalPos(local, &px, &py)) return;

    void* wm = GameState::GetWorldMgr();
    if (!AddrOk(wm)) return;

    if (s_dictScratch.size() < 4096)
        s_dictScratch.resize(4096);
    outPodCount = SehSnapshotDict(wm, s_dictScratch.data(), static_cast<int>(s_dictScratch.size()));
    if (outPodCount <= 0) return;

    s_snapshot.reserve(static_cast<size_t>(outPodCount / 4));
    for (int i = 0; i < outPodCount; ++i) {
        const DictPod& pod = s_dictScratch[static_cast<size_t>(i)];
        if (pod.ent == local || SehKlassIsPlayer(pod.ent)) {
            s_localPlayerObjectId.store(pod.id, std::memory_order_relaxed);
            continue;
        }

        CandidateOut cand{};
        if (!SehReadCandidate(pod.id, pod.ent, local, cand))
            continue;

        UpdateVelocity(cand.id, cand.x, cand.y, now, cand.ptr);

        EnemyTracker::Entry e{};
        e.id             = cand.id;
        e.objType        = cand.objType;
        e.x              = cand.x;
        e.y              = cand.y;
        e.hp             = cand.hp;
        e.maxHp          = cand.maxHp;
        e.isInvulnerable = cand.isInvulnerable;
        e.hasHealthBar   = cand.hasHealthBar;
        e.ptr            = cand.ptr;

        auto it = s_velMap.find(cand.id);
        if (it != s_velMap.end()) {
            e.vx = it->second.vx;
            e.vy = it->second.vy;
        }
        s_snapshot.push_back(e);
    }
}

static bool SafeRebuildMemorySnapshot(ULONGLONG now, int& outPodCount)
{
    __try {
        RebuildMemorySnapshotLocked(now, outPodCount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void FinishSnapshotLocked(ULONGLONG now, int podCount, int memCount)
{
    if (now >= s_pruneAt) {
        s_pruneAt = now + 5000ULL;
        for (auto it2 = s_velMap.begin(); it2 != s_velMap.end();) {
            if (now - it2->second.t > 8000ULL) it2 = s_velMap.erase(it2);
            else ++it2;
        }
    }

    s_lastPodCount.store(podCount, std::memory_order_relaxed);
    s_lastSnapCount.store(static_cast<int>(s_snapshot.size()), std::memory_order_relaxed);

    static ULONGLONG s_lastDbgMs = 0;
    if (now - s_lastDbgMs > 3000ULL) {
        s_lastDbgMs = now;
        if (podCount > 0 || !s_snapshot.empty())
            DBG_FILE_LOG("[EnemyTracker] pods=" << podCount
                << " mem=" << memCount
                << " wire=" << (static_cast<int>(s_snapshot.size()) - memCount)
                << " total=" << s_snapshot.size());
    }
}

} // namespace

namespace EnemyTracker {

void Tick()
{
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&s_lock);
    if (now - s_lastTickMs < 8ULL) {
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    s_lastTickMs = now;

    s_snapshot.clear();
    int podCount = 0;
    if (!SafeRebuildMemorySnapshot(now, podCount)) {
        static ULONGLONG s_lastMemFailMs = 0;
        if (now - s_lastMemFailMs > 5000ULL) {
            s_lastMemFailMs = now;
            DBG_FILE_LOG("[EnemyTracker] memory scan SEH — wire-only fallback");
        }
    }

    const int memCount = static_cast<int>(s_snapshot.size());
    const int beforeWire = memCount;
    MergeWireSnapshot(now);
    const int afterWire = static_cast<int>(s_snapshot.size());
    if (afterWire > beforeWire) {
        static ULONGLONG s_lastWireLogMs = 0;
        if (now - s_lastWireLogMs > 3000ULL) {
            s_lastWireLogMs = now;
            DBG_FILE_LOG("[EnemyTracker] wire merged +" << (afterWire - beforeWire)
                << " enemies (total=" << afterWire << " mem=" << beforeWire << ")");
        }
    }
    FinishSnapshotLocked(now, podCount, memCount);

    ReleaseSRWLockExclusive(&s_lock);
}

std::vector<Entry> SnapshotCopy()
{
    AcquireSRWLockShared(&s_lock);
    std::vector<Entry> copy = s_snapshot;
    ReleaseSRWLockShared(&s_lock);
    return copy;
}

void Enumerate(Callback cb, void* user)
{
    if (!cb) return;
    AcquireSRWLockShared(&s_lock);
    for (const Entry& e : s_snapshot) cb(e, user);
    ReleaseSRWLockShared(&s_lock);
}

int32_t GetLocalPlayerObjectId()
{
    return s_localPlayerObjectId.load(std::memory_order_relaxed);
}

int GetLastPodCount()
{
    return s_lastPodCount.load(std::memory_order_relaxed);
}

int GetLastSnapshotCount()
{
    return s_lastSnapCount.load(std::memory_order_relaxed);
}

} // namespace EnemyTracker

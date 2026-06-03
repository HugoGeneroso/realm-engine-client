#include "pch-il2cpp.h"
#include "PlayerCollider.h"
#include "DbgFileLog.h"
#include "Il2CppResolver.h"
#include "RuntimeOffsets.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iomanip>

namespace PlayerCollider {
namespace {

constexpr uint32_t kOffCollisionMultiplierFallback = 0x780;
constexpr size_t kMaxObjectPropertiesTargets = 3;
constexpr size_t kMaxEntityCandidates = 2;

void* g_lastPlayer = nullptr;
void* g_lastProperties[kMaxObjectPropertiesTargets]{};
bool g_probePending = true;
bool g_loggedMissingProperties = false;
bool g_loggedAlreadyZero = false;
bool g_loggedCollisionOffset = false;
uint32_t g_collisionMultiplierOffset = kOffCollisionMultiplierFallback;
bool g_collisionMultiplierOffsetFromMetadata = false;
ULONGLONG g_lastWaitingLogMs = 0;
int g_waitingLogCount = 0;

struct EntityCandidate {
    void* ptr = nullptr;
};

bool IsPlausiblePointer(const void* ptr)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    return address > 0x10000ULL && address < 0x7FFFFFFFFFFFULL;
}

void Logf(const char* format, ...)
{
    char line[512] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    std::printf("%s\n", line);
    std::fflush(stdout);
    DBG_FILE_LOG(line);
}

uint32_t FloatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void ConsoleLog(float before, float after, const void* properties, const char* reason)
{
    Logf("[PlayerCollider] collisionRadiusMultiplier %.9g(0x%08X) -> %.9g(0x%08X) props=%p reason=%s",
        before,
        FloatBits(before),
        after,
        FloatBits(after),
        properties,
        reason && reason[0] ? reason : "update");
}

void LogLine(const char* message)
{
    Logf("[PlayerCollider] %s", message ? message : "message");
}

FieldInfo* FindFieldOnHierarchy(Il2CppClass* klass, const char* fieldName)
{
    for (Il2CppClass* current = klass; current; current = il2cpp_class_get_parent(current)) {
        FieldInfo* field = il2cpp_class_get_field_from_name(current, fieldName);
        if (field) return field;
    }
    return nullptr;
}

bool TryGetObjectClass(void* object, Il2CppClass*& outClass)
{
    outClass = nullptr;
    if (!Resolver::Protection::IsValidIl2CppObject(object)) return false;
    return Resolver::Protection::safe_call([&]() {
        outClass = il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(object));
    }) && outClass != nullptr;
}

const char* SafeClassName(Il2CppClass* klass)
{
    const char* name = nullptr;
    if (!klass) return "?";
    Resolver::Protection::safe_call([&]() {
        name = il2cpp_class_get_name(klass);
    });
    return name ? name : "?";
}

bool ClassHierarchyHas(Il2CppClass* klass, const char* expectedName)
{
    if (!klass || !expectedName) return false;
    bool matched = false;
    Resolver::Protection::safe_call([&]() {
        for (Il2CppClass* current = klass; current; current = il2cpp_class_get_parent(current)) {
            const char* name = il2cpp_class_get_name(current);
            if (name && std::strcmp(name, expectedName) == 0) {
                matched = true;
                break;
            }
        }
    });
    return matched;
}

bool ResolveFieldOffset(Il2CppClass* klass, const char* fieldName, uint32_t fallback, uint32_t& outOffset, bool& outFromMetadata)
{
    outOffset = fallback;
    outFromMetadata = false;
    if (!klass || !fieldName) return false;

    FieldInfo* field = nullptr;
    if (!Resolver::Protection::safe_call([&]() {
        field = FindFieldOnHierarchy(klass, fieldName);
    }) || !field) {
        return false;
    }

    return Resolver::Protection::safe_call([&]() {
        outOffset = static_cast<uint32_t>(il2cpp_field_get_offset(field));
        outFromMetadata = true;
    });
}

void* ReadPointerRef(void* basePtr, uint32_t offset)
{
    if (!basePtr || offset == 0) return nullptr;
    void* ptr = nullptr;
    __try {
        ptr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(basePtr) + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ptr = nullptr;
    }
    return IsPlausiblePointer(ptr) ? ptr : nullptr;
}

void* ReadObjectPropertiesRef(void* entityPtr, uint32_t offset)
{
    return ReadPointerRef(entityPtr, offset);
}

bool TryReadMultiplier(void* properties, uint32_t collisionMultiplierOffset, float& outMultiplier)
{
    outMultiplier = 0.0f;
    if (!properties) return false;
    __try {
        outMultiplier = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(properties) + collisionMultiplierOffset);
        return std::isfinite(outMultiplier);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    outMultiplier = 0.0f;
    return false;
}

bool ResolveCollisionMultiplierOffset(void* properties)
{
    Il2CppClass* propertiesClass = nullptr;
    if (!TryGetObjectClass(properties, propertiesClass) || !ClassHierarchyHas(propertiesClass, "ObjectProperties"))
        return false;

    uint32_t offset = kOffCollisionMultiplierFallback;
    bool fromMetadata = false;
    ResolveFieldOffset(propertiesClass, "collisionRadiusMultiplier", kOffCollisionMultiplierFallback, offset, fromMetadata);
    g_collisionMultiplierOffset = offset;
    g_collisionMultiplierOffsetFromMetadata = fromMetadata;
    if (!g_loggedCollisionOffset) {
        Logf("[PlayerCollider] collisionRadiusMultiplier offset=0x%X source=%s propsClass=%s",
            g_collisionMultiplierOffset,
            g_collisionMultiplierOffsetFromMetadata ? "metadata" : "fallback",
            SafeClassName(propertiesClass));
        g_loggedCollisionOffset = true;
    }
    return true;
}

uint32_t ResolveObjectPropertiesOffset(Il2CppClass* entityClass, const ObjectPropertiesTarget& target)
{
    const char* fieldName = nullptr;
    if (std::strcmp(target.label, "base") == 0) fieldName = "OBAKMCCDBJA";
    else if (std::strcmp(target.label, "map-object") == 0) fieldName = "KKENJFFDMPO";
    else if (std::strcmp(target.label, "player-collision") == 0) fieldName = "GGBCADDBAPN";

    uint32_t offset = target.offset;
    bool fromMetadata = false;
    if (fieldName)
        ResolveFieldOffset(entityClass, fieldName, target.offset, offset, fromMetadata);
    return offset;
}

void* ResolveViewDestroyEntity(void* localPlayer)
{
    Il2CppClass* localClass = nullptr;
    uint32_t viewHandlerOffset = RuntimeOffsets::KJ_ViewHandler;
    bool fromMetadata = false;
    if (TryGetObjectClass(localPlayer, localClass))
        ResolveFieldOffset(localClass, "MPGOFIHIDML", RuntimeOffsets::KJ_ViewHandler, viewHandlerOffset, fromMetadata);

    void* viewHandler = ReadPointerRef(localPlayer, viewHandlerOffset);
    Il2CppClass* viewClass = nullptr;
    uint32_t destroyEntityOffset = RuntimeOffsets::VH_DestroyEntity;
    fromMetadata = false;
    if (TryGetObjectClass(viewHandler, viewClass))
        ResolveFieldOffset(viewClass, "destroyEntity", RuntimeOffsets::VH_DestroyEntity, destroyEntityOffset, fromMetadata);

    return ReadPointerRef(viewHandler, destroyEntityOffset);
}

bool ShouldLogWaitingForPlayer()
{
    if (!g_probePending || g_waitingLogCount >= 8) return false;
    const ULONGLONG now = GetTickCount64();
    if (g_waitingLogCount > 0 && now - g_lastWaitingLogMs < 500ULL) return false;
    g_lastWaitingLogMs = now;
    ++g_waitingLogCount;
    return true;
}

bool TargetsChanged(void* const* properties, size_t propertyCount)
{
    for (size_t i = 0; i < kMaxObjectPropertiesTargets; ++i) {
        if (g_lastProperties[i] != (i < propertyCount ? properties[i] : nullptr)) return true;
    }
    return false;
}

void RememberTargets(void* const* properties, size_t propertyCount)
{
    for (size_t i = 0; i < kMaxObjectPropertiesTargets; ++i)
        g_lastProperties[i] = i < propertyCount ? properties[i] : nullptr;
}

void ClearRememberedTargets()
{
    for (void*& properties : g_lastProperties)
        properties = nullptr;
}

bool AddObjectPropertiesTarget(void** properties, size_t& propertyCount, void* candidate)
{
    if (!candidate || propertyCount >= kMaxObjectPropertiesTargets) return false;
    for (size_t i = 0; i < propertyCount; ++i) {
        if (properties[i] == candidate) return false;
    }
    properties[propertyCount++] = candidate;
    return true;
}

size_t CollectPlayerObjectProperties(void* entity, const ObjectPropertiesTarget* targets, size_t targetCount, void** outProperties, size_t outCapacity)
{
    if (!entity || !targets || outCapacity == 0) return 0;

    Il2CppClass* entityClass = nullptr;
    if (!TryGetObjectClass(entity, entityClass) || !ClassHierarchyHas(entityClass, "FKALGHJIADI"))
        return 0;

    size_t propertyCount = 0;
    const size_t count = targetCount < outCapacity ? targetCount : outCapacity;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t offset = ResolveObjectPropertiesOffset(entityClass, targets[i]);
        void* properties = ReadObjectPropertiesRef(entity, offset);
        if (!ResolveCollisionMultiplierOffset(properties)) continue;
        AddObjectPropertiesTarget(outProperties, propertyCount, properties);
    }
    return propertyCount;
}

void LogProbe(const char* reason, const void* player, void* const* properties, size_t propertyCount)
{
    std::printf("[PlayerCollider] probe reason=%s player=%p", reason && reason[0] ? reason : "probe", player);
    DBG_FILE_LOG("[PlayerCollider] probe reason=" << (reason && reason[0] ? reason : "probe") << " player=" << player);
    for (size_t i = 0; i < propertyCount; ++i) {
        float multiplier = -1.0f;
        const bool readable = TryReadMultiplier(properties[i], g_collisionMultiplierOffset, multiplier);
        std::printf(" props[%zu]=%p collOff=0x%X mult=%.9g bits=0x%08X readable=%d",
            i,
            properties[i],
            g_collisionMultiplierOffset,
            multiplier,
            FloatBits(multiplier),
            readable ? 1 : 0);
        DBG_FILE_LOG("[PlayerCollider] probe-target index=" << i
            << " props=" << properties[i]
            << " collisionOffset=0x" << std::hex << g_collisionMultiplierOffset << std::dec
            << " mult=" << multiplier
            << " bits=0x" << std::hex << FloatBits(multiplier) << std::dec
            << " readable=" << readable);
    }
    std::printf("\n");
    std::fflush(stdout);
}

bool ApplyPropertiesMultiplier(void* properties, const char* reason, UpdateLogFn logFn)
{
    if (!properties) return false;

    bool updated = false;
    __try {
        float& multiplier = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(properties) + g_collisionMultiplierOffset);
        updated = ApplyMultiplier(multiplier, properties, reason, logFn);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return updated;
}

} // namespace

bool ApplyEntityMultiplier(void* entityPtr,
    uint32_t primaryObjectPropertiesOffset,
    uint32_t secondaryObjectPropertiesOffset,
    uint32_t collisionMultiplierOffset,
    const char* reason,
    UpdateLogFn logFn)
{
    const ObjectPropertiesTarget targets[] = {
        { "primary", primaryObjectPropertiesOffset },
        { "secondary", secondaryObjectPropertiesOffset },
    };
    return ApplyEntityMultiplierTargets(entityPtr, targets, 2, collisionMultiplierOffset, reason, logFn);
}

bool ApplyEntityMultiplierTargets(void* entityPtr,
    const ObjectPropertiesTarget* targets,
    size_t targetCount,
    uint32_t collisionMultiplierOffset,
    const char* reason,
    UpdateLogFn logFn)
{
    if (!entityPtr || !targets || targetCount == 0) return false;

    bool updated = false;
    void* visited[kMaxObjectPropertiesTargets]{};
    size_t visitedCount = 0;
    const size_t count = targetCount < kMaxObjectPropertiesTargets ? targetCount : kMaxObjectPropertiesTargets;
    for (size_t i = 0; i < count; ++i) {
        void* properties = ReadObjectPropertiesRef(entityPtr, targets[i].offset);
        if (!properties) continue;

        bool duplicate = false;
        for (size_t seen = 0; seen < visitedCount; ++seen) {
            if (visited[seen] == properties) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        visited[visitedCount++] = properties;

        __try {
            float& multiplier = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(properties) + collisionMultiplierOffset);
            updated = ApplyMultiplier(multiplier, properties, reason, logFn) || updated;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    return updated;
}

void Tick(void* player)
{
    if (!player) {
        if (ShouldLogWaitingForPlayer()) LogProbe("waiting-for-local-player", nullptr, nullptr, 0);
        if (g_lastPlayer) ResetScene();
        return;
    }

    const ObjectPropertiesTarget targets[] = {
        { "base", RuntimeOffsets::ObjProps },
        { "map-object", RuntimeOffsets::MoObjectProps },
        { "player-collision", RuntimeOffsets::PlayerCollisionProps },
    };

    EntityCandidate entities[kMaxEntityCandidates] = {
        { player },
        { ResolveViewDestroyEntity(player) },
    };
    if (entities[1].ptr == entities[0].ptr) entities[1].ptr = nullptr;

    void* properties[kMaxObjectPropertiesTargets]{};
    size_t propertyCount = 0;
    for (const EntityCandidate& entity : entities) {
        void* entityProperties[kMaxObjectPropertiesTargets]{};
        const size_t entityPropertyCount = CollectPlayerObjectProperties(entity.ptr, targets, 3, entityProperties, kMaxObjectPropertiesTargets);
        for (size_t i = 0; i < entityPropertyCount; ++i)
            AddObjectPropertiesTarget(properties, propertyCount, entityProperties[i]);
    }

    if (propertyCount == 0) {
        if (g_probePending && !g_loggedMissingProperties) {
            LogProbe("local-player-has-no-valid-player-object-properties", player, nullptr, 0);
            g_loggedMissingProperties = true;
        }
        return;
    }

    const bool changedObject = player != g_lastPlayer || TargetsChanged(properties, propertyCount);
    g_lastPlayer = player;
    RememberTargets(properties, propertyCount);

    const char* reason = changedObject ? "player-or-scene-change" : "value-restored";
    bool updated = false;
    for (size_t i = 0; i < propertyCount; ++i)
        updated = ApplyPropertiesMultiplier(properties[i], reason, ConsoleLog) || updated;

    if (updated) {
        g_probePending = false;
        g_loggedAlreadyZero = false;
        return;
    }

    if ((g_probePending || changedObject) && !g_loggedAlreadyZero) {
        LogProbe("found-player-object-properties-but-no-update-needed", player, properties, propertyCount);
        g_loggedAlreadyZero = true;
        g_probePending = false;
    }
}

void ResetScene()
{
    g_lastPlayer = nullptr;
    ClearRememberedTargets();
    g_probePending = true;
    g_loggedMissingProperties = false;
    g_loggedAlreadyZero = false;
    g_lastWaitingLogMs = 0;
    g_waitingLogCount = 0;
    LogLine("scene reset requested; will re-check local player ObjectProperties");
}

void ResetStateForTest()
{
    g_lastPlayer = nullptr;
    ClearRememberedTargets();
    g_probePending = true;
    g_loggedMissingProperties = false;
    g_loggedAlreadyZero = false;
    g_lastWaitingLogMs = 0;
    g_waitingLogCount = 0;
}

} // namespace PlayerCollider

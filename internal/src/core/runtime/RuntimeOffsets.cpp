#include "pch-il2cpp.h"
#include "RuntimeOffsets.h"
#include "Il2CppResolver.h"
#include "BeebyteName.h"
#include "BootGate.h"
#include "DbgFileLog.h"
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <thread>
#include <atomic>
#include "GameState.h"

// ─────────────────────────────────────────────────────────────────────────────
// All variables are pre-initialised to their hardcoded fallback values.
// EnsureAll() overwrites each one the first time its class appears in IL2CPP
// metadata.  If a class loads but the field name is not found (e.g. a future
// Beebyte rename), the fallback stays in place.
// ─────────────────────────────────────────────────────────────────────────────

namespace RuntimeOffsets {

// ── Offset storage — initialised to fallbacks ─────────────────────────────
uint32_t PosX            = 0x3C;
uint32_t PosY            = 0x40;
uint32_t ObjType         = 0x30;
uint32_t ObjProps        = 0x18;
uint32_t KJ_ViewHandler  = 0x10;   // MPGOFIHIDML — ViewHandler component pointer
uint32_t KJ_SkinWidthObj = 0x28;   // LGDCEJKHGFJ — IPKAMAAPAGA reference
uint32_t ObjId           = 0x34;   // HHPOJBFICAH — objectId Int32
uint32_t KJ_BaseRadius   = 0x44;   // IOKKOCEAJNA — base bullet radius Single
uint32_t KJ_Scale        = 0x74;   // KEDBLBJIKCB — scale float3 first component
uint32_t KJ_Float3Pos    = 0x68;   // DGNPJNFGFPE — Unity.Mathematics.float3 world position (written on teleport/move)

// KJNHLADHEMH = current HP, NCBIICBDGAG = max HP (order in struct; names were once swapped in tooling).
uint32_t HP          = 0x25C;
uint32_t MaxHP       = 0x258;
uint32_t Defense     = 0x210;
uint32_t PlayerIGN   = 0x178;
// COHCKAPOLCA dump 0x248 on LKHPPBEGNOM (not 0x218 — that is HMMHAKPBEDK). +0x50 ACTK => 0x298.
// AV on PMMFLLAIPGN is handled gracefully: AutoAim SEH catches it and returns false (untargetable).
// PMMFLLAIPGN that AV are treated as targetable (correct fallback — assume no immunity).
uint32_t MoConditions = 0x298;
// ECGPFJKCCAN — Vector2 velocity (live 0x2C0 on build 6.11.0.1.0).
uint32_t MoVelocity   = 0x2C0;
// KKENJFFDMPO — LKHPPBEGNOM ObjectProperties alias. Runtime metadata resolves this at 0x1C8.
uint32_t MoObjectProps = 0x1C8;
// GGBCADDBAPN — player collision ObjectProperties used by the C# working implementation.
// Generated/inherited player layout resolves this at 0x2F0; unlike the stat fields above,
// runtime evidence shows this ObjectProperties pointer is not ACTK-shifted.
uint32_t PlayerCollisionProps = 0x2F0;

uint32_t Tex1              = 0x4C4;
uint32_t Tex2              = 0x544;
uint32_t CurMP             = 0x5AC;
uint32_t MaxMP             = 0x5A8;
// DAGEMHFLJLK — groundDamageImmune bool (dump 0x458 / runtime 0x4A8). NOT ability cooldown.
uint32_t GroundDmgImmune   = 0x4A8;
// BINDBHJLPMG — invincible bool (dump 0x459 / runtime 0x4A9). Short-duration hit invulnerability.
uint32_t LocalInvincible   = 0x4A9;
// PPBLNMIMIFP — abilityReady bool (dump 0x515 / runtime 0x565). True when ability can fire.
uint32_t AbilityReady      = 0x575;
// CGCMALPMMJL — bool moving (dump 0x448 / runtime 0x498).
uint32_t Player_Moving     = 0x498;
// BHJFNEAHAOE — float moveDirX (dump 0x478 / runtime 0x4C8).
uint32_t Player_MoveDirX   = 0x4C8;
// GDNEBFDDDKM — float moveDirY (dump 0x47C / runtime 0x4CC).
uint32_t Player_MoveDirY   = 0x4CC;
// BHJFNEAHAOE — float SPD stat (dump 0x478 / runtime 0x478, no ACTK shift).
// PlayerTAB and TestTAB read this without shift for the move-speed formula.
uint32_t Player_Spd        = 0x478;

// ApplicationManager → WorldManager field offset.
// Set by GameState.cpp type-scan (immune to backing-field name obfuscation).
uint32_t AppMgr_WorldMgr   = 0xC0;

uint32_t WM_Local    = 0x48;
uint32_t WM_AllDict  = 0xB0;
uint32_t WM_MapDictA = 0xB8;
uint32_t WM_MapDictB = 0xC0;
uint32_t WM_KjmonList= 0xE8;
uint32_t WM_TileArr  = 0x58;
uint32_t WM_TileList = 0x60;
uint32_t WM_TickId   = 0xD8;   // FIAJOKGHGGK — world tick counter UInt32
uint32_t WM_TickId2  = 0xDC;   // HOMNPDGNOMO — secondary tick UInt32

uint32_t TileX       = 0x38;
uint32_t TileY       = 0x3C;
uint32_t TileType    = 0x40;
uint32_t TileProps   = 0x50;

uint32_t TP_Speed    = 0x50;
uint32_t TP_Sink     = 0x58;
uint32_t TP_NoWalk   = 0x78;
uint32_t TP_MinDmg   = 0xB0;
uint32_t TP_MaxDmg   = 0xB8;
uint32_t TP_Push     = 0xC8;
uint32_t TP_Alpha    = 0xD0;
uint32_t TP_Sinking  = 0xD8;

uint32_t OP_IdStr         = 0x38;
uint32_t OP_NoCover       = 0x98;
// InvincibleElement string pointer — non-null iff XML <Invincible/> is set.
// dump 0x450 + 0x10 IL2CPP object header = 0x460.
uint32_t OP_InvincibleElem= 0x458;
uint32_t OP_NoWallRpt     = 0x210;
uint32_t OP_OccupySq      = 0x6A2;
uint32_t OP_FullOcc       = 0x6D9;
uint32_t OP_EnemyOcc      = 0x6DA;
// isEnemy verified at 0x6D1 against the live client (upstream offset update);
// our il2cpp-types.h dump still shows 0x6C9 — dump is stale for this region.
uint32_t OP_IsEnemy       = 0x6D1;
uint32_t OP_IsStatic      = 0x6DB;
uint32_t OP_BlockProj     = 0x6DC;
// noHealthBar bool — true when the entity type has no visible HP bar. dump 0x6C6 + 0x10 = 0x6D6.
uint32_t OP_NoHealthBar   = 0x6DE;
uint32_t OP_ProtGnd       = 0x6E4;
uint32_t OP_ProtSink      = 0x6E5;
uint32_t OP_Flying        = 0x6EC;
uint32_t OP_ConnectT      = 0x774;
uint32_t OP_Projectiles   = 0x1C0;

uint32_t PP_Lifetime        = 0x160;
uint32_t PP_Speed           = 0x168;
uint32_t PP_IsWavy          = 0x16C;
uint32_t PP_IsBoomerang     = 0x16D;
uint32_t PP_IsParametric    = 0x170;
uint32_t PP_HasCustomHitbox = 0x175;
uint32_t PP_LaserDist       = 0x178;
uint32_t PP_SpeedClamp      = 0x17C;
uint32_t PP_AccelDelay      = 0x180;
uint32_t PP_Acceleration    = 0x184;
uint32_t PP_AccelerationInv = 0x188;
uint32_t PP_IsAccel         = 0x18C;
uint32_t PP_UseAccel        = 0x198;
uint32_t PP_VelocityChangeRate = 0x190;
uint32_t PP_VelocityChangeRateInv = 0x194;
uint32_t PP_Magnitude       = 0x194;
uint32_t PP_Frequency       = 0x198;
uint32_t PP_Amplitude       = 0x19C;
uint32_t PP_HasCustomAmplitude = 0x1A0;
uint32_t PP_CollMult              = 0xC0;
uint32_t PP_TurnRate              = 0xD4;
uint32_t PP_TurnRateDelay         = 0xD8;
uint32_t PP_TurnStopTime          = 0xE8;
uint32_t PP_CircleTurnAngle       = 0xEC;
uint32_t PP_CircleTurnDelay       = 0xF0;
uint32_t PP_TurnAcceleration      = 0xDC;
uint32_t PP_TurnAccelDelay        = 0xE0;
uint32_t PP_TurnClamp             = 0xE4;
uint32_t PP_TurnAccelInv          = 0x1B4;
uint32_t PP_IsTurning             = 0x1B8;
uint32_t PP_IsTurningDelayed      = 0x1BA;

uint32_t Hbeak_ProjRadius         = 0x1D4;  // HHFDCMIIIHF — collision radius T on projectile instance
uint32_t Hbeak_ProjPropsPtr       = 0x118;  // FOMOIBCKIFP — per-shot ProjectileProperties override
uint32_t Hbeak_Angle              = 0x148;  // FFFFKPDHEFP — spawn angle Single
uint32_t Hbeak_InstanceDamage     = 0x174;  // DBNNDLKNECM — per-instance damage Int32
uint32_t PP_CustomHitbox          = 0x148;  // "CustomHitbox" — ProjectileCustomHitbox* reference
uint32_t PP_IsArmorPiercing       = 0x174;  // "IsArmorPiercing"
uint32_t CH_OffsetX               = 0x10;   // "offsetX" — custom hitbox X offset Single
uint32_t CH_OffsetY               = 0x14;   // "offsetY" — custom hitbox Y offset Single
uint32_t VH_SpriteShader          = 0x60;   // "spriteShader" — SpriteShader on ViewHandler
uint32_t VH_DestroyEntity         = 0x88;   // "destroyEntity" — authoritative entity pointer on ViewHandler

// ── LKHPPBEGNOM facing angle (+0x50 ACTK) ────────────────────────────────
// ECHAFMAAKMD — dump 0x1DC + kActk 0x50 = runtime 0x22C
uint32_t Player_FacingAngle  = 0x22C;

// ── GJJCEFJMNMK throwable entity ─────────────────────────────────────────
// BeeByte decoy names ("GuiCanvasSwitcher", "UpdateRadialValue") preserved
// in IL2CPP metadata; il2cpp_field_get_offset returns runtime-ready values
// (all parent ACTK shifts already baked into the dump layout).
uint32_t Gjj_OriginX    = 0x368;  // GuiCanvasSwitcher.x
uint32_t Gjj_OriginY    = 0x36C;  // GuiCanvasSwitcher.y (= OriginX+4)
uint32_t Gjj_DestX      = 0x370;  // IAJJLFBDJGE.x
uint32_t Gjj_DestY      = 0x374;  // IAJJLFBDJGE.y (= DestX+4)
uint32_t Gjj_DurationMs = 0x388;  // EAICINLCCJK

// ── FHOHCELBPDO visual throwable ─────────────────────────────────────────
// Origin is PosX/PosY (inherited from BMO base). No ACTK shift for LKFFPGONEOB.
uint32_t Fhoh_DurationMs = 0x140; // IEJNJENOCFP
uint32_t Fhoh_DestX      = 0x154; // PBHMINMBFOM.x
uint32_t Fhoh_DestY      = 0x158; // PBHMINMBFOM.y (= DestX+4)

// ── COEFCBBIBMC ShowEffect packet ────────────────────────────────────────
// OODFCLBKDJJ base (network packets have no ACTK shift).
uint32_t Sfx_EffectType  = 0x10;  // MIDADCIKEBD
uint32_t Sfx_TargetObjId = 0x14;  // HNOKKCFIJHJ
uint32_t Sfx_Pos1X       = 0x18;  // KMAIENKMNFA.x
uint32_t Sfx_Pos1Y       = 0x1C;  // KMAIENKMNFA.y (= Pos1X+4)
uint32_t Sfx_Pos2X       = 0x20;  // AEPOCACMOHI.x
uint32_t Sfx_Pos2Y       = 0x24;  // AEPOCACMOHI.y (= Pos2X+4)
uint32_t Sfx_Duration    = 0x2C;  // KPKIICOBBIM

// ── CustomExplosionEntrance ───────────────────────────────────────────────
uint32_t Cee_Distance    = 0x38;  // "distance" (XML data class, no ACTK)
uint32_t Cee_Speed       = 0x3C;  // "speed" (XML data class, no ACTK)

// ── FieldInfo pointer cache — initialised to nullptr ─────────────────────
FieldInfo* FI_HP               = nullptr;
FieldInfo* FI_MaxHP            = nullptr;
FieldInfo* FI_Defense          = nullptr;
FieldInfo* FI_CurMP            = nullptr;
FieldInfo* FI_MaxMP            = nullptr;
FieldInfo* FI_AbilityReady     = nullptr;  // PPBLNMIMIFP — bool abilityReady
FieldInfo* FI_LocalInvincible  = nullptr;  // BINDBHJLPMG — bool invincible (short-duration hit immunity)
FieldInfo* FI_ObjType          = nullptr;

static uint32_t s_mapChangesDetected = 0;

// ── Internal helpers ──────────────────────────────────────────────────────

static FieldInfo* FindFieldOnHierarchy(Il2CppClass* klass, const char* name)
{
    for (Il2CppClass* k = klass; k; k = il2cpp_class_get_parent(k)) {
        FieldInfo* f = il2cpp_class_get_field_from_name(k, name);
        if (f) return f;
    }
    return nullptr;
}

static bool IsFieldInfoValid(FieldInfo* fi)
{
    if (!fi) return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(fi);
    if (a < 0x10000 || a > 0x7FFFFFFFFFFFULL) return false;
    if (!fi->type) return false;
    return true;
}

static bool DetectMapChange()
{
    // The local player object is re-created on every realm/Nexus transition,
    // so its pointer changes.  Hooks/FieldInfo captured against the old
    // object must be re-validated.  Plain pointer compare — no SEH needed.
    static void* s_lastLocalPtr = nullptr;
    void* local = GameState::GetLocalPtr();
    if (!local) {
        s_lastLocalPtr = nullptr;
        return false;
    }
    if (s_lastLocalPtr == nullptr) {
        s_lastLocalPtr = local;
        return false;
    }
    if (local != s_lastLocalPtr) {
        s_lastLocalPtr = local;
        s_mapChangesDetected++;
        return true;
    }
    return false;
}

// ── Resolution table ─────────────────────────────────────────────────────
//
// ┌─ UPDATE THIS EACH GAME PATCH ───────────────────────────────────────────┐
// │ BeeByte re-randomizes class/field NAMES (and sometimes offsets) every    │
// │ Exalt build, so name-resolution silently fails and these fallbacks are   │
// │ used stale. To find what broke after a patch:                            │
// │   1. Build + in-game open  Test tab → OFFSET HEALTH.  Stale offsets show  │
// │      yellow (STALE renamed / no-class) or red (SUSPECT = read garbage).   │
// │   2. From a fresh Il2CppInspector dump of the new build, get the new      │
// │      obfuscated class + field name AND the offset for each flagged row.   │
// │   3. Update that row here: the className, the tryNames[] (put the NEW     │
// │      name first; old names can stay as extra candidates), and the         │
// │      `outPtr` variable's fallback initializer above (lines ~20-193).      │
// │ A row resolves automatically once its className+fieldName match metadata; │
// │ the fallback only bites when the NAME is wrong. So fixing the NAME is     │
// │ usually enough — the offset then comes live from il2cpp_field_get_offset. │
// │ CRITICAL rows (verify first): HP/MaxHP/Defense (LKHPPBEGNOM) and          │
// │ Hbeak_InstanceDamage (HBEAKBIHANL) — these feed AutoNexus damage calc.    │
// └─────────────────────────────────────────────────────────────────────────┘
//
// Each Entry:
//   className  — passed to Resolver::FindClassLoose
//   tryNames   — candidate field names tried in order (up to 4)
//   tryCount   — how many names to try
//   actkShift  — added to il2cpp_field_get_offset result (0 or 0x50)
//   outPtr     — pointer to the uint32_t to update
//   done       — set to true once class was found (even if field wasn't)

static constexpr uint32_t kActk = 0x50u;

struct Entry {
    const char* className;
    const char* tryNames[4];
    int         tryCount;
    uint32_t    actkShift;
    uint32_t*   outPtr;
    bool        done;
};

static Entry s_entries[] = {

    // ── KJMONHENJEN (no shift) ────────────────────────────────────────────
    { "KJMONHENJEN", { "CLFEOFKBNEJ" },                              1, 0,     &PosX,           false },
    { "KJMONHENJEN", { "PKEECFNFEIO" },                              1, 0,     &PosY,           false },
    { "KJMONHENJEN", { "HFDNHJFNEKA" },                              1, 0,     &ObjType,        false },
    { "KJMONHENJEN", { "OBAKMCCDBJA" },                              1, 0,     &ObjProps,       false },
    { "KJMONHENJEN", { "MPGOFIHIDML" },                              1, 0,     &KJ_ViewHandler, false },
    { "KJMONHENJEN", { "LGDCEJKHGFJ" },                              1, 0,     &KJ_SkinWidthObj,false },
    { "KJMONHENJEN", { "HHPOJBFICAH" },                              1, 0,     &ObjId,          false },
    { "KJMONHENJEN", { "IOKKOCEAJNA" },                              1, 0,     &KJ_BaseRadius,  false },
    { "KJMONHENJEN", { "KEDBLBJIKCB" },                              1, 0,     &KJ_Scale,       false },
    { "KJMONHENJEN", { "DGNPJNFGFPE" },                              1, 0,     &KJ_Float3Pos,   false },


    // ── LKHPPBEGNOM (+0x50 ACTK for own fields) ───────────────────────────
    { "LKHPPBEGNOM", { "ABCPKBGJPEP", "KJNHLADHEMH" },               2, kActk, &HP,            false },
    { "LKHPPBEGNOM", { "OADOHPKBPJB", "NCBIICBDGAG" },               2, kActk, &MaxHP,         false },
    { "LKHPPBEGNOM", { "HODJPKFINKF" },                              1, kActk, &Defense,       false },
    { "LKHPPBEGNOM", { "DPGEBOCBKEF" },                              1, 0,     &PlayerIGN,     false },
    { "LKHPPBEGNOM", { "COHCKAPOLCA" },                           1, kActk, &MoConditions,  false },
    { "LKHPPBEGNOM", { "ECGPFJKCCAN" },                           1, kActk, &MoVelocity,    false },
    { "LKHPPBEGNOM", { "KKENJFFDMPO" },                           1, 0,     &MoObjectProps, false },
    { "LKHPPBEGNOM", { "GGBCADDBAPN" },                           1, 0,     &PlayerCollisionProps, false },

    // ── FKALGHJIADI (+0x50 ACTK for own fields) ───────────────────────────
    { "FKALGHJIADI", { "HCMECDPHEMC" },                              1, kActk, &Tex1,          false },
    { "FKALGHJIADI", { "HKPOMIBEGPK" },                              1, kActk, &Tex2,          false },
    { "FKALGHJIADI", { "FMHMGKEPIDN" },                              1, kActk, &CurMP,              false },
    { "FKALGHJIADI", { "NEDCKPIIIPN" },                              1, kActk, &MaxMP,              false },
    // DAGEMHFLJLK = groundDamageImmune (dump 0x458 / runtime 0x4A8)
    { "FKALGHJIADI", { "DAGEMHFLJLK" },                              1, kActk, &GroundDmgImmune,    false },
    // BINDBHJLPMG = invincible bool (dump 0x459 / runtime 0x4A9) — per FKALGHJIADI_mapped.txt
    { "FKALGHJIADI", { "BINDBHJLPMG" },                              1, kActk, &LocalInvincible,    false },
    // PPBLNMIMIFP = abilityReady bool (dump 0x515 / runtime 0x565) — the correct ability gate
    { "FKALGHJIADI", { "PPBLNMIMIFP" },                              1, kActk, &AbilityReady,       false },
    // CGCMALPMMJL = bool moving (dump 0x448 / runtime 0x498)
    { "FKALGHJIADI", { "CGCMALPMMJL" },                              1, kActk, &Player_Moving,      false },
    // BHJFNEAHAOE = float moveDirX (dump 0x478 / runtime 0x4C8)
    { "FKALGHJIADI", { "BHJFNEAHAOE" },                              1, kActk, &Player_MoveDirX,    false },
    // GDNEBFDDDKM = float moveDirY (dump 0x47C / runtime 0x4CC)
    { "FKALGHJIADI", { "GDNEBFDDDKM" },                              1, kActk, &Player_MoveDirY,    false },

    // ── HJMBOMEHGDJ WorldManager (no shift) ──────────────────────────────
    { "HJMBOMEHGDJ", { "OCLNLBHDEFK" },                              1, 0,     &WM_Local,      false },
    { "HJMBOMEHGDJ", { "DFALIKKKGLI" },                              1, 0,     &WM_AllDict,    false },
    { "HJMBOMEHGDJ", { "KHIHFNACEKJ" },                              1, 0,     &WM_MapDictA,   false },
    { "HJMBOMEHGDJ", { "CIOIHEOEAEB" },                              1, 0,     &WM_MapDictB,   false },
    { "HJMBOMEHGDJ", { "ONABHKFOJNE" },                              1, 0,     &WM_KjmonList,  false },
    { "HJMBOMEHGDJ", { "NOJEHIAOAJM" },                              1, 0,     &WM_TileArr,    false },
    { "HJMBOMEHGDJ", { "IMAOBDCMPHC" },                              1, 0,     &WM_TileList,   false },
    { "HJMBOMEHGDJ", { "FIAJOKGHGGK" },                              1, 0,     &WM_TickId,     false },
    { "HJMBOMEHGDJ", { "HOMNPDGNOMO" },                              1, 0,     &WM_TickId2,    false },

    // ── BGAIOPJMHLO tile instance (no shift) ─────────────────────────────
    { "BGAIOPJMHLO", { "CLFEOFKBNEJ" },                              1, 0,     &TileX,         false },
    { "BGAIOPJMHLO", { "PKEECFNFEIO" },                              1, 0,     &TileY,         false },
    { "BGAIOPJMHLO", { "JOFEAFJPJEM" },                              1, 0,     &TileType,      false },
    { "BGAIOPJMHLO", { "KEOKJCIJIAD" },                              1, 0,     &TileProps,     false },

    // ── CMFPKCJHKKB XmlTileProperties (no shift) ─────────────────────────
    { "CMFPKCJHKKB", { "MFEJMAABLIL" },                              1, 0,     &TP_Speed,      false },
    { "CMFPKCJHKKB", { "BMGKCKHOIOH" },                              1, 0,     &TP_Sink,       false },
    { "CMFPKCJHKKB", { "LFKLKFIEMAH" },                              1, 0,     &TP_NoWalk,     false },
    { "CMFPKCJHKKB", { "MCMDAGNIGEB" },                              1, 0,     &TP_MinDmg,     false },
    { "CMFPKCJHKKB", { "KHMCMAHEBNG" },                              1, 0,     &TP_MaxDmg,     false },
    { "CMFPKCJHKKB", { "FNCCEGBHNKG" },                              1, 0,     &TP_Push,       false },
    { "CMFPKCJHKKB", { "LCHPDCNHJCA" },                              1, 0,     &TP_Alpha,      false },
    { "CMFPKCJHKKB", { "JKIDGAADOLC" },                              1, 0,     &TP_Sinking,    false },

    // ── ObjectProperties (real names, no shift) ───────────────────────────
    { "ObjectProperties", { "id" },                                  1, 0,     &OP_IdStr,          false },
    { "ObjectProperties", { "NoCoverElement" },                      1, 0,     &OP_NoCover,        false },
    // InvincibleElement — XML <Invincible/> string; non-null = permanently invincible.
    { "ObjectProperties", { "InvincibleElement" },                   1, 0,     &OP_InvincibleElem, false },
    { "ObjectProperties", { "NoWallTextureRepeatElement",
                             "NoWallTextureRepeat" },                2, 0,     &OP_NoWallRpt,      false },
    { "ObjectProperties", { "occupySquare" },                        1, 0,     &OP_OccupySq,       false },
    { "ObjectProperties", { "fullOccupy" },                          1, 0,     &OP_FullOcc,        false },
    { "ObjectProperties", { "enemyOccupySquare" },                   1, 0,     &OP_EnemyOcc,       false },
    { "ObjectProperties", { "isEnemy" },                             1, 0,     &OP_IsEnemy,        false },
    { "ObjectProperties", { "isStatic" },                            1, 0,     &OP_IsStatic,       false },
    { "ObjectProperties", { "blockProjectiles" },                    1, 0,     &OP_BlockProj,      false },
    // noHealthBar — true when entity type has no visible HP bar; must not be targeted.
    { "ObjectProperties", { "noHealthBar" },                         1, 0,     &OP_NoHealthBar,    false },
    { "ObjectProperties", { "protectFromGroundDamage",
                             "ProtectFromGroundDamage" },            2, 0,     &OP_ProtGnd,        false },
    { "ObjectProperties", { "protectFromSink",
                             "ProtectFromSink" },                    2, 0,     &OP_ProtSink,       false },
    { "ObjectProperties", { "flying" },                              1, 0,     &OP_Flying,         false },
    { "ObjectProperties", { "connectType" },                         1, 0,     &OP_ConnectT,       false },
    { "ObjectProperties", { "Projectiles", "projectiles" },          2, 0,     &OP_Projectiles,    false },

    // ── ProjectileProperties (real names, no shift) ───────────────────────
    { "ProjectileProperties", { "Lifetime",   "lifetime" },          2, 0,     &PP_Lifetime,        false },
    { "ProjectileProperties", { "ProjectileSpeed", "Speed" },        2, 0,     &PP_Speed,           false },
    { "ProjectileProperties", { "IsWavy",     "Wavy" },              2, 0,     &PP_IsWavy,          false },
    { "ProjectileProperties", { "IsBoomerang","Boomerang" },         2, 0,     &PP_IsBoomerang,     false },
    { "ProjectileProperties", { "IsParametric","Parametric" },       2, 0,     &PP_IsParametric,    false },
    { "ProjectileProperties", { "HasCustomHitbox","CustomHitbox" },  2, 0,     &PP_HasCustomHitbox, false },
    { "ProjectileProperties", { "LaserDistance","laserDistance" },   2, 0,     &PP_LaserDist,       false },
    { "ProjectileProperties", { "SpeedClampValue", "speedClampValue",
                                 "SpeedClamp", "speedClamp" },        4, 0,     &PP_SpeedClamp,      false },
    { "ProjectileProperties", { "AccelerationDelayValue", "accelerationDelayValue",
                                 "AccelDelay", "accelDelay" },        4, 0,     &PP_AccelDelay,      false },
    { "ProjectileProperties", { "AccelerationValue", "accelerationValue",
                                 "Acceleration", "acceleration" },    4, 0,     &PP_Acceleration,    false },
    { "ProjectileProperties", { "IsAccelerating", "isAccelerating" }, 2, 0, &PP_IsAccel,  false },
    // UseAcceleration is the per-shot enable, NOT an alias for IsAccelerating.
    // Keep separate so cached game-authored projectile paths receive correct props.
    { "ProjectileProperties", { "UseAcceleration", "useAcceleration" }, 2, 0, &PP_UseAccel, false },
    { "ProjectileProperties", { "AccelerationInv", "accelerationInv" },   2, 0,     &PP_AccelerationInv, false },
    { "ProjectileProperties", { "VelocityChangeRate", "velocityChangeRate" }, 2, 0, &PP_VelocityChangeRate, false },
    { "ProjectileProperties", { "VelocityChangeRateInv", "velocityChangeRateInv" }, 2, 0, &PP_VelocityChangeRateInv, false },
    { "ProjectileProperties", { "Magnitude",  "magnitude" },         2, 0,     &PP_Magnitude,       false },
    { "ProjectileProperties", { "Frequency",  "frequency" },         2, 0,     &PP_Frequency,       false },
    { "ProjectileProperties", { "Amplitude",  "amplitude" },         2, 0,     &PP_Amplitude,       false },
    { "ProjectileProperties", { "HasCustomAmplitude","CustomAmplitude","customAmplitude" }, 3, 0, &PP_HasCustomAmplitude, false },
    { "ProjectileProperties", { "CollisionMult","collisionMult",
                                 "ConditionEffectAmount" },          3, 0,     &PP_CollMult,        false },
    { "ProjectileProperties", { "ProjectileTurnRate", "TurnRate","turnRate"},     3, 0, &PP_TurnRate,        false },
    { "ProjectileProperties", { "ProjectileTurnRateDelay","TurnRateDelay" },     2, 0, &PP_TurnRateDelay,   false },
    { "ProjectileProperties", { "ProjectileTurnStopTime", "TurnStopTime" },      2, 0, &PP_TurnStopTime,    false },
    { "ProjectileProperties", { "ProjectileCircleTurnAngle","CircleTurnAngle" }, 2, 0, &PP_CircleTurnAngle, false },
    { "ProjectileProperties", { "ProjectileCircleTurnDelay","CircleTurnDelay" }, 2, 0, &PP_CircleTurnDelay, false },
    { "ProjectileProperties", { "TurnAcceleration","turnAcceleration" },          2, 0, &PP_TurnAcceleration,false },
    { "ProjectileProperties", { "TurnAccelerationDelay","turnAccelerationDelay"},2, 0, &PP_TurnAccelDelay,  false },
    { "ProjectileProperties", { "TurnClamp","turnClamp","ProjectileTurnClamp" }, 3, 0, &PP_TurnClamp,       false },
    { "ProjectileProperties", { "TurnAccelerationInv","turnAccelerationInv" },   2, 0, &PP_TurnAccelInv,    false },
    { "ProjectileProperties", { "IsTurning",  "isTurning","Turning"},            3, 0, &PP_IsTurning,       false },
    { "ProjectileProperties", { "IsTurningDelayed","isTurningDelayed" },         2, 0, &PP_IsTurningDelayed,false },

    // ── HBEAKBIHANL projectile instance (no shift) ───────────────────────────
    { "HBEAKBIHANL", { "HHFDCMIIIHF", "projRadius" },                            2, 0, &Hbeak_ProjRadius,      false },
    { "HBEAKBIHANL", { "FOMOIBCKIFP" },                                           1, 0, &Hbeak_ProjPropsPtr,    false },
    { "HBEAKBIHANL", { "FFFFKPDHEFP" },                                           1, 0, &Hbeak_Angle,           false },
    { "HBEAKBIHANL", { "DBNNDLKNECM" },                                           1, 0, &Hbeak_InstanceDamage,  false },

    // ── ProjectileProperties continued ────────────────────────────────────────
    { "ProjectileProperties", { "CustomHitbox", "customHitbox" },                 2, 0, &PP_CustomHitbox,       false },
    { "ProjectileProperties", { "IsArmorPiercing", "armorPiercing" },             2, 0, &PP_IsArmorPiercing,    false },

    // ── ProjectileCustomHitbox (real names, no shift) ──────────────────────────
    { "ProjectileCustomHitbox", { "offsetX" },                                    1, 0, &CH_OffsetX,            false },
    { "ProjectileCustomHitbox", { "offsetY" },                                    1, 0, &CH_OffsetY,            false },

    // ── ViewHandler (real names, no shift) ─────────────────────────────────────
    { "ViewHandler", { "spriteShader" },                                          1, 0, &VH_SpriteShader,       false },
    { "ViewHandler", { "destroyEntity" },                                        1, 0, &VH_DestroyEntity,      false },

    // ── LKHPPBEGNOM facing angle (+0x50 ACTK) ────────────────────────────────
    // ECHAFMAAKMD (dump 0x1DC + kActk = 0x22C runtime). Written by SendShotPacketDetour.
    { "LKHPPBEGNOM", { "ECHAFMAAKMD" },                                           1, kActk, &Player_FacingAngle, false },

    // ── GJJCEFJMNMK throwable entity (no extra shift — runtime offsets in dump) ──
    // "ICODPOCLEEL" and "IAJJLFBDJGE" are BeeByte field names for origin/dest Vector2.
    // ACTK shift from LKHPPBEGNOM parent is already reflected in the dump layout.
    { "GJJCEFJMNMK", { "ICODPOCLEEL", "GuiCanvasSwitcher" },                      2, 0, &Gjj_OriginX,   false },
    { "GJJCEFJMNMK", { "IAJJLFBDJGE" },                                           1, 0, &Gjj_DestX,     false },
    { "GJJCEFJMNMK", { "EAICINLCCJK" },                                           1, 0, &Gjj_DurationMs,false },

    // ── FHOHCELBPDO visual throwable (LKFFPGONEOB base, no ACTK shift) ─────────
    { "FHOHCELBPDO", { "IEJNJENOCFP" },                                           1, 0, &Fhoh_DurationMs,false },
    { "FHOHCELBPDO", { "PBHMINMBFOM" },                                           1, 0, &Fhoh_DestX,    false },

    // ── COEFCBBIBMC ShowEffect packet (OODFCLBKDJJ base, no ACTK shift) ─────────
    { "COEFCBBIBMC", { "MIDADCIKEBD" },                                           1, 0, &Sfx_EffectType, false },
    { "COEFCBBIBMC", { "HNOKKCFIJHJ" },                                           1, 0, &Sfx_TargetObjId,false },
    { "COEFCBBIBMC", { "KMAIENKMNFA" },                                           1, 0, &Sfx_Pos1X,     false },
    { "COEFCBBIBMC", { "AEPOCACMOHI" },                                           1, 0, &Sfx_Pos2X,     false },
    { "COEFCBBIBMC", { "KPKIICOBBIM" },                                           1, 0, &Sfx_Duration,  false },

    // ── CustomExplosionEntrance (real XML field names, no shift) ─────────────────
    { "CustomExplosionEntrance", { "distance" },                                  1, 0, &Cee_Distance,  false },
    { "CustomExplosionEntrance", { "speed" },                                     1, 0, &Cee_Speed,     false },
};

static constexpr int kEntryCount = static_cast<int>(sizeof(s_entries) / sizeof(s_entries[0]));

// ── FieldInfo resolution table ────────────────────────────────────────────
// Separate from s_entries so we keep the offset table untouched.
// Populated once; used by ReadField<T> for type-correct dynamic reads.

struct FieldInfoEntry {
    const char* className;
    const char* fieldName;
    FieldInfo** out;
    bool        done;
};

static FieldInfoEntry s_fieldInfoEntries[] = {
    { "LKHPPBEGNOM", "ABCPKBGJPEP", &FI_HP,                 false },
    { "LKHPPBEGNOM", "OADOHPKBPJB", &FI_MaxHP,              false },
    { "LKHPPBEGNOM", "HODJPKFINKF", &FI_Defense,            false },
    { "FKALGHJIADI", "FMHMGKEPIDN", &FI_CurMP,              false },
    { "FKALGHJIADI", "NEDCKPIIIPN", &FI_MaxMP,              false },
    // PPBLNMIMIFP = bool abilityReady (dump 0x515 / runtime 0x565)
    { "FKALGHJIADI", "PPBLNMIMIFP", &FI_AbilityReady,       false },
    // BINDBHJLPMG = bool invincible (dump 0x459 / runtime 0x4A9) — short-duration hit immunity
    { "FKALGHJIADI", "BINDBHJLPMG", &FI_LocalInvincible,    false },
    { "KJMONHENJEN", "HFDNHJFNEKA", &FI_ObjType,            false },
};
static constexpr int kFIEntryCount =
    static_cast<int>(sizeof(s_fieldInfoEntries) / sizeof(s_fieldInfoEntries[0]));

// ── EnsureAll ─────────────────────────────────────────────────────────────
//
// Called once per frame.  Iterates the table and attempts to resolve any
// entry whose class is now available in IL2CPP metadata.
// Resolved (or permanently-failed) entries are skipped on future calls.
//
// Perf notes:
//   - s_allDone: skips the entire loop once every entry is settled.
//   - Class-name dedup: entries are grouped by class, so we cache the last
//     FindClassLoose result and reuse it for consecutive same-class entries
//     instead of calling FindClassLoose once per entry.
//   - Rename timeout: if a class is still missing 5 s after first call, we
//     mark its entries done (accepting fallbacks) so we stop scanning metadata
//     every frame for a name that BeeByte has likely renamed.

static bool s_allDone             = false;
static bool s_giveUpFired         = false;
static char      s_unresolvedClassNames[512] = {};
static ULONGLONG s_firstCallTick       = 0;
static int       s_entryIdx            = 0;
static int       s_fiIdx               = 0;
static bool      s_initialPassComplete = false;
static constexpr ULONGLONG kGiveUpMs   = 5000ULL;
static constexpr int     kBudgetPerCall = 8;
static constexpr ULONGLONG kMaxEnsureMs = 4ULL;
static bool s_inEnsureAll             = false;
static bool s_allowLazyClassLookup    = false;
static bool s_mapChangeDetected       = false;

static bool IsLazyInstanceClass(const char* cls)
{
    return cls && (std::strcmp(cls, "HBEAKBIHANL") == 0
                || std::strcmp(cls, "GJJCEFJMNMK") == 0
                || std::strcmp(cls, "FHOHCELBPDO") == 0);
}

bool HasGivenUp() { return s_giveUpFired; }
bool AllResolved() { return s_allDone; }

// ── Structural auto-recovery (Phase 1 / A1) ──────────────────────────────────
static Il2CppClass* s_recoveredProjClass = nullptr;
static bool         s_structScanDone     = false;

Il2CppClass* GetRecoveredProjClass() { return s_recoveredProjClass; }

// The projectile instance class is the (best) class holding a field whose type is
// ProjectileProperties — a relationship BeeByte's per-patch renames cannot change.
// Disambiguate candidates by also having >=2 float fields (pos/angle) and an int
// field (damage), so a class that merely references ProjectileProperties for some
// other reason doesn't win.
static Il2CppClass* ResolveProjectilePropertiesClass()
{
    Il2CppClass* ppClass = Resolver::FindClassLoose("ProjectileProperties");
    if (ppClass) return ppClass;
    for (const auto& kv : Beebyte::GetMap()) {
        if (kv.second != "ProjectileProperties") continue;
        ppClass = Resolver::FindClassLoose(kv.first.c_str());
        if (ppClass) {
            DBG_FILE_LOG("[RuntimeOffsets] ProjectileProperties via Beebyte '"
                << kv.first.c_str() << "'");
            return ppClass;
        }
    }
    return nullptr;
}

static Il2CppClass* ScanForProjectileClass()
{
    Il2CppClass* ppClass = ResolveProjectilePropertiesClass();
    if (!ppClass) return nullptr;
    Il2CppClass* singleClass = Resolver::FindClass("System", "Single");
    Il2CppClass* int32Class  = Resolver::FindClass("System", "Int32");

    struct Ctx {
        Il2CppClass* pp; Il2CppClass* single; Il2CppClass* i32;
        Il2CppClass* best; int bestScore;
    } ctx{ ppClass, singleClass, int32Class, nullptr, -1 };

    il2cpp_class_for_each([](Il2CppClass* klass, void* ud) {
        auto* c = static_cast<Ctx*>(ud);
        void* iter = nullptr;
        bool hasProps = false;
        int  floats = 0, ints = 0;
        for (FieldInfo* f; (f = il2cpp_class_get_fields(klass, &iter)) != nullptr; ) {
            const Il2CppType* ft = il2cpp_field_get_type(f);
            if (!ft) continue;
            Il2CppClass* fc = il2cpp_class_from_type(ft);
            if      (fc == c->pp)     hasProps = true;
            else if (fc == c->single) ++floats;
            else if (fc == c->i32)    ++ints;
        }
        if (!hasProps) return;
        const int score = (floats >= 1 ? 2 : 0) + (ints >= 1 ? 1 : 0);
        if (score > c->bestScore) { c->bestScore = score; c->best = klass; }
    }, &ctx);

    return ctx.best;
}

int AutoResolveByStructure()
{
    if (s_structScanDone) return 0;   // the metadata walk is expensive — run it once

    DBG_FILE_LOG("[RuntimeOffsets] AutoResolveByStructure: metadata scan begin");
    int healed = 0;
    if (Il2CppClass* proj = ScanForProjectileClass()) {
        s_recoveredProjClass = proj;
        ++healed;
        const int fields = HealEntriesFromClassMetadata(proj);
        healed += fields;
        DBG_FILE_LOG("[RuntimeOffsets] AutoResolveByStructure: projectile class recovered via "
            "ProjectileProperties* anchor (name='" << il2cpp_class_get_name(proj) << "'"
            << ", fieldsHealed=" << fields << ")");
    } else {
        DBG_FILE_LOG("[RuntimeOffsets] AutoResolveByStructure: projectile class NOT found "
            "(ProjectileProperties anchor missing or no candidate matched)");
    }
    s_structScanDone = true;
    return healed;
}

// SEH-safe variant of AutoResolveByStructure. The scan walks the live IL2CPP type
// system (il2cpp_class_for_each); a stale game update can make that walk AV. A raw
// exception here used to take down the whole process — the VEH then reports the
// last render-thread step (i.e. "BootGate::Tick"). Catch it, log it, and re-arm
// the retry (KickAsyncStructuralScan re-runs it later) instead of crashing.
static int SafeAutoResolveByStructure()
{
    int healed = 0;
    __try {
        healed = AutoResolveByStructure();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // DbgFileLogWrite (not DBG_FILE_LOG): DBG_FILE_LOG builds an ostringstream,
        // and a local with a destructor triggers C2712 inside __try/__except.
        DbgFileLogWrite("[RuntimeOffsets] AsyncStructuralScan: caught exception during auto-resolve (offset drift?) — re-arming retry instead of crashing");
        s_structScanDone = false;
    }
    return healed;
}

void KickAsyncStructuralScan()
{
    if (s_recoveredProjClass != nullptr) return;

    static std::atomic<bool>         s_workerRunning{ false };
    static std::atomic<ULONGLONG>    s_lastKickMs{ 0 };

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastKick = s_lastKickMs.load(std::memory_order_relaxed);

    // First scan failed (class rename) — retry periodically instead of giving up forever.
    if (s_structScanDone && s_recoveredProjClass == nullptr) {
        if (now - lastKick < 15000ULL) return;
        s_structScanDone = false;
    } else if (s_structScanDone) {
        return;
    }

    // Debounce: BootGate may call every frame while dodge/aim is enabled.
    if (lastKick != 0 && now - lastKick < 2000ULL) return;

    bool expected = false;
    if (!s_workerRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    s_lastKickMs.store(now, std::memory_order_relaxed);
    std::thread([]() {
        DBG_FILE_LOG("[RuntimeOffsets] AsyncStructuralScan: worker begin (in-world deferred)");
        const int healed = SafeAutoResolveByStructure();
        DBG_FILE_LOG("[RuntimeOffsets] AsyncStructuralScan: worker done healed=" << healed);
        if (healed > 0)
            BootGate::RefreshAuditIfReady();
        s_workerRunning.store(false, std::memory_order_release);
    }).detach();
}

const char* GetUnresolvedClassNames()  { return s_unresolvedClassNames; }

// ── Offset health status (parallel to s_entries) ─────────────────────────────
static OffsetState s_entryState[kEntryCount];     // OffsetState::Pending (0) by default
static uint32_t    s_entryFallback[kEntryCount];  // snapshot of each initial fallback
static bool        s_fallbackSnapped = false;

// Lazy instance classes are skipped during login; accept pre-validated fallbacks
// for BootGate gating so combat features can arm before the class lazy-loads.
static void FinalizeLazyFallbackEntries()
{
    for (int i = 0; i < kEntryCount; ++i) {
        Entry& e = s_entries[i];
        if (e.done) continue;
        if (!IsLazyInstanceClass(e.className)) continue;
        s_entryState[i] = OffsetState::ResolvedMatch;
        e.done = true;
    }
}

int GetOffsetReport(OffsetReportRow* out, int maxRows)
{
    if (out) {
        for (int i = 0; i < kEntryCount && i < maxRows; ++i) {
            OffsetReportRow& r = out[i];
            r.className = s_entries[i].className;
            r.fieldName = s_entries[i].tryCount ? s_entries[i].tryNames[0] : "?";
            r.fallback  = s_fallbackSnapped ? s_entryFallback[i] : *s_entries[i].outPtr;
            r.value     = *s_entries[i].outPtr;
            r.state     = s_entryState[i];
        }
    }
    return kEntryCount;
}

void GetOffsetSummary(int& resolved, int& usingFallback, int& suspect, int& pending)
{
    resolved = usingFallback = suspect = pending = 0;
    for (int i = 0; i < kEntryCount; ++i) {
        switch (s_entryState[i]) {
            case OffsetState::ResolvedMatch:
            case OffsetState::ResolvedShifted:   ++resolved;      break;
            case OffsetState::FallbackFieldName:
            case OffsetState::FallbackGaveUp:    ++usingFallback; break;
            case OffsetState::Suspect:           ++suspect;       break;
            default:                             ++pending;       break;
        }
    }
}

void MarkSuspect(const uint32_t* offsetVar)
{
    for (int i = 0; i < kEntryCount; ++i)
        if (s_entries[i].outPtr == offsetVar) { s_entryState[i] = OffsetState::Suspect; return; }
}

// Force the Beebyte name table to be built now, at a clean point during init
// (before dPresent touches the heap), rather than lazily the first time a
// HBEAKBIHANL recovery walk iterates GetMap() on the render thread.
void Warmup()
{
    (void)Beebyte::GetMap().size();
}

// ── A4: recover renamed classes from a LIVE OBJECT the cheat already holds ────
static Il2CppClass* LookupEntryClass(const char* className)
{
    if (!className) return nullptr;
    // Lazy instance classes (projectile/AoE) are resolved via MaybeRetry only —
    // FindClassLoose + Beebyte scan on the login screen has hung Unity (~3s freeze).
    if (IsLazyInstanceClass(className) && !s_allowLazyClassLookup) return nullptr;
    if (Il2CppClass* k = Resolver::FindClassLoose(className)) return k;
    if (std::strcmp(className, "HBEAKBIHANL") == 0) {
        if (s_recoveredProjClass) return s_recoveredProjClass;
        for (const auto& kv : Beebyte::GetMap()) {
            if (kv.second != "Projectile") continue;
            if (Il2CppClass* k = Resolver::FindClassLoose(kv.first.c_str())) return k;
        }
    }
    return nullptr;
}

static bool EntryTargetsClass(const Entry& e, Il2CppClass* cls)
{
    if (!cls) return false;
    const char* clsName = il2cpp_class_get_name(cls);
    if (clsName && e.className && std::strcmp(e.className, clsName) == 0) return true;
    if (cls != s_recoveredProjClass) return false;
    return e.outPtr == &Hbeak_ProjRadius || e.outPtr == &Hbeak_ProjPropsPtr
        || e.outPtr == &Hbeak_Angle || e.outPtr == &Hbeak_InstanceDamage;
}

static int HealEntryFieldsFromClass(Il2CppClass* cls, bool onlyBroken)
{
    if (!cls) return 0;
    const char* clsName = il2cpp_class_get_name(cls);
    int healed = 0;
    for (int i = 0; i < kEntryCount; ++i) {
        if (onlyBroken) {
            const OffsetState st = s_entryState[i];
            if (st != OffsetState::FallbackGaveUp && st != OffsetState::FallbackFieldName)
                continue;
        }
        Entry& e = s_entries[i];
        if (!EntryTargetsClass(e, cls)) continue;

        for (int t = 0; t < e.tryCount; ++t) {
            FieldInfo* f = FindFieldOnHierarchy(cls, e.tryNames[t]);
            if (!f) continue;
            const uint32_t resolved = static_cast<uint32_t>(il2cpp_field_get_offset(f)) + e.actkShift;
            const uint32_t committed = s_entryFallback[i];
            *e.outPtr = resolved;
            s_entryState[i] = (resolved == committed) ? OffsetState::ResolvedMatch
                                                      : OffsetState::ResolvedShifted;
            e.done = true;
            ++healed;
            DBG_FILE_LOG("[RuntimeOffsets] HealFromClass '" << (clsName ? clsName : "?")
                << "': " << e.className << "::" << e.tryNames[t] << " -> 0x"
                << std::hex << resolved << std::dec);
            break;
        }
    }
    return healed;
}

// Retry ONLY FallbackGaveUp rows once their class lazy-loads — never resets s_allDone
// or re-walks the full 119-entry table (that burst on the login screen crashed Unity).
// Returns true if a retry was actually attempted (passed the internal gates), so the
// caller can latch the one-shot gate without marking it done when BootGate/LazyOffset
// wasn't ready yet.
static bool MaybeRetryLazyGaveUpEntries()
{
    // s_allDone fires during login while BootGate is still Auditing; the first
    // retry here called FindClassLoose for HBEAKBIHANL and hung Unity (~3.6s).
    if (!BootGate::LazyOffsetLookupAllowed()) return false;

    static ULONGLONG s_lastRetryMs = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastRetryMs < 1000ULL) return false;

    bool anyRetry = false;
    for (int i = 0; i < kEntryCount; ++i) {
        if (s_entryState[i] == OffsetState::FallbackGaveUp) { anyRetry = true; break; }
        if (!s_entries[i].done && IsLazyInstanceClass(s_entries[i].className)) {
            anyRetry = true;
            break;
        }
    }
    if (!anyRetry) return false;
    s_lastRetryMs = now;
    s_allowLazyClassLookup = true;
    const ULONGLONG deadline = now + kMaxEnsureMs;

    const char*  lastClassName = nullptr;
    Il2CppClass* lastClass     = nullptr;
    int healed = 0;
    for (int i = 0; i < kEntryCount; ++i) {
        if (GetTickCount64() >= deadline) break;
        const bool pendingLazy = !s_entries[i].done
            && IsLazyInstanceClass(s_entries[i].className);
        if (s_entryState[i] != OffsetState::FallbackGaveUp && !pendingLazy) continue;
        Entry& e = s_entries[i];

        Il2CppClass* klass;
        if (e.className == lastClassName) {
            klass = lastClass;
        } else {
            klass = LookupEntryClass(e.className);
            lastClassName = e.className;
            lastClass     = klass;
        }
        if (!klass) continue;

        FieldInfo* found = nullptr;
        const char* foundName = nullptr;
        for (int t = 0; t < e.tryCount && !found; ++t) {
            found = FindFieldOnHierarchy(klass, e.tryNames[t]);
            if (found) foundName = e.tryNames[t];
        }
        if (!found) continue;

        const uint32_t resolved = static_cast<uint32_t>(il2cpp_field_get_offset(found)) + e.actkShift;
        const uint32_t committed = s_entryFallback[i];
        *e.outPtr = resolved;
        s_entryState[i] = (resolved == committed) ? OffsetState::ResolvedMatch
                                                  : OffsetState::ResolvedShifted;
        e.done = true;
        ++healed;
        DBG_FILE_LOG("[RuntimeOffsets] lazy retry '" << (lastClassName ? lastClassName : "?")
            << "': " << e.className << "::" << foundName << " -> 0x"
            << std::hex << resolved << std::dec);
    }
    if (s_recoveredProjClass && GetTickCount64() < deadline)
        healed += HealEntryFieldsFromClass(s_recoveredProjClass, true);
    if (healed > 0) {
        DBG_FILE_LOG("[RuntimeOffsets] lazy retry healed " << healed << " entr(ies)");
        BootGate::RefreshAuditIfReady();
    }
    s_allowLazyClassLookup = false;
    return true;
}

int HealEntriesFromClassMetadata(Il2CppClass* klass)
{
    return HealEntryFieldsFromClass(klass, true);
}

// `il2cpp_object_get_class(instance)` is the gold-standard rung — it can't be
// wrong. FindFieldOnHierarchy walks the instance's whole parent chain, so one
// player instance covers the FKALGHJIADI→LKHPPBEGNOM→KJMONHENJEN hierarchy, and
// the WorldManager instance covers HJMBOMEHGDJ. We re-resolve ONLY entries that
// are currently broken (class never resolved → FallbackGaveUp); healthy offsets
// are never touched. Heals the class-rename case (field names still stable);
// field-name renames within a recovered class still need value/param matching.
int RecoverFromInstance(void* instance)
{
    if (!instance) return 0;
    Il2CppClass* cls = il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(instance));
    if (!cls) return 0;
    const char* clsName = il2cpp_class_get_name(cls);

    const int healed = HealEntryFieldsFromClass(cls, true);
    if (healed > 0)
        DBG_FILE_LOG("[RuntimeOffsets] RecoverFromInstance via live class '"
            << (clsName ? clsName : "?") << "' healed " << healed << " entr(ies)");
    return healed;
}

// SEH-guarded raw pointer read — its own function so the __try contains no C++
// object unwinding (avoids C2712).
static void* SafeReadPtr(void* base, uint32_t off)
{
    if (!base) return nullptr;
    __try { return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// A4 tile chain: a live tile instance heals BGAIOPJMHLO (incl. its TileProps field
// offset), then we read the now-correct TileProps pointer and heal CMFPKCJHKKB
// (XmlTileProperties) from it. One call converges both — no frame dependency.
int RecoverTileChain(void* tileInstance)
{
    if (!tileInstance) return 0;
    int healed = RecoverFromInstance(tileInstance);     // BGAIOPJMHLO (+ TileProps offset)
    void* props = SafeReadPtr(tileInstance, TileProps); // now-healed offset
    if (props) healed += RecoverFromInstance(props);    // CMFPKCJHKKB
    return healed;
}

// Conservative bounds — only values a CORRECT offset can never produce, so a
// legitimate edge state (0 def, huge-HP boss pet, etc.) is not false-flagged.
void SanityCheckPlayerStats(int32_t hp, int32_t maxHp, int32_t defense)
{
    // Player not loaded yet (char-select / between worlds): all-zero is "not
    // populated", not a stale offset — a stale offset reads WILD values, not clean
    // zeros. Skip so we don't falsely flag MaxHP at char-select.
    if (hp == 0 && maxHp == 0 && defense == 0) return;
    if (maxHp <= 0 || maxHp > 1000000) MarkSuspect(&MaxHP);
    if (hp < -1000 || (maxHp > 0 && maxHp <= 1000000 && hp > maxHp * 5)) MarkSuspect(&HP);
    if (defense < 0 || defense > 2000) MarkSuspect(&Defense);
}

void SanityCheckProjDamage(int32_t sampledDamage)
{
    if (sampledDamage < 0 || sampledDamage > 1000000) MarkSuspect(&Hbeak_InstanceDamage);
}

void EnsureAll()
{
    if (s_inEnsureAll) return;
    s_inEnsureAll = true;
    struct EnsureGuard { ~EnsureGuard() { s_inEnsureAll = false; } } guard;

    if (s_allDone) {
        if (DetectMapChange()) {
            DbgFileLogWrite("[RuntimeOffsets] Map change detected in EnsureAll, re-validating FieldInfo pointers");
            if (FI_HP && !IsFieldInfoValid(FI_HP)) {
                DbgFileLogWrite("[RuntimeOffsets] FI_HP invalid after map change, clearing");
                FI_HP = nullptr;
            }
            if (FI_MaxHP && !IsFieldInfoValid(FI_MaxHP)) {
                DbgFileLogWrite("[RuntimeOffsets] FI_MaxHP invalid after map change, clearing");
                FI_MaxHP = nullptr;
            }
            if (FI_Defense && !IsFieldInfoValid(FI_Defense)) {
                DbgFileLogWrite("[RuntimeOffsets] FI_Defense invalid after map change, clearing");
                FI_Defense = nullptr;
            }
            if (FI_LocalInvincible && !IsFieldInfoValid(FI_LocalInvincible)) {
                DbgFileLogWrite("[RuntimeOffsets] FI_LocalInvincible invalid after map change, clearing");
                FI_LocalInvincible = nullptr;
            }
            if (FI_ObjType && !IsFieldInfoValid(FI_ObjType)) {
                DbgFileLogWrite("[RuntimeOffsets] FI_ObjType invalid after map change, clearing");
                FI_ObjType = nullptr;
            }
        }
        // Lazy retry is a ONE-SHOT recovery of FallbackGaveUp / pending lazy-instance
        // rows once their class becomes available — it must NOT run every frame.
        // Re-armed exactly once after s_recoveredProjClass is set by the async
        // structural scan, so projectile/AoE offsets can still heal from the
        // recovered class. MaybeRetryLazyGaveUpEntries() returns false (no-op) while
        // BootGate/LazyOffset isn't ready, so we don't latch "done" until it actually
        // attempted a retry (which only happens once a live character is in-world).
        static bool s_lazyRetryDone       = false;
        static bool s_retryAfterRecover   = false;
        const bool projRecoveredNow       = (s_recoveredProjClass != nullptr);
        if (!s_lazyRetryDone || (projRecoveredNow && !s_retryAfterRecover)) {
            if (MaybeRetryLazyGaveUpEntries())
                s_lazyRetryDone = true;
            if (projRecoveredNow)
                s_retryAfterRecover = true;
        }
        Gjj_OriginY = Gjj_OriginX + 4;
        Gjj_DestY   = Gjj_DestX   + 4;
        Fhoh_DestY  = Fhoh_DestX  + 4;
        Sfx_Pos1Y   = Sfx_Pos1X   + 4;
        Sfx_Pos2Y   = Sfx_Pos2X   + 4;
        return;
    }

    if (!s_fallbackSnapped) {
        s_fallbackSnapped = true;
        for (int i = 0; i < kEntryCount; ++i) s_entryFallback[i] = *s_entries[i].outPtr;
    }

    const ULONGLONG now = GetTickCount64();
    if (s_firstCallTick == 0) s_firstCallTick = now;
    const bool giveUp = (now - s_firstCallTick) >= kGiveUpMs;

    // First time give-up fires: collect unique unresolved class names before marking done.
    if (giveUp && !s_giveUpFired) {
        s_giveUpFired = true;
        const char* lastCls = nullptr;
        for (int i = 0; i < kEntryCount; ++i) {
            if (s_entries[i].done) continue;
            const char* cls = s_entries[i].className;
            if (lastCls && strcmp(cls, lastCls) == 0) continue;
            lastCls = cls;
            if (s_unresolvedClassNames[0] != '\0')
                strncat_s(s_unresolvedClassNames, sizeof(s_unresolvedClassNames), ",", _TRUNCATE);
            strncat_s(s_unresolvedClassNames, sizeof(s_unresolvedClassNames), cls, _TRUNCATE);
        }
        if (s_unresolvedClassNames[0] != '\0')
            DBG_FILE_LOG("[RuntimeOffsets] Unresolved (BeeByte renamed): " << s_unresolvedClassNames);
    }

    // Give-up window: settle every remaining row in one pass (no per-frame budget).
    if (giveUp) {
        const char*  lastClassName = nullptr;
        Il2CppClass* lastClass     = nullptr;
        for (int i = 0; i < kEntryCount; ++i) {
            Entry& e = s_entries[i];
            if (e.done) continue;
            if (IsLazyInstanceClass(e.className)) {
                s_entryState[i] = OffsetState::ResolvedMatch;
                e.done = true;
                continue;
            }
            DBG_FILE_LOG("[RuntimeOffsets] " << e.className << "::"
                << (e.tryCount ? e.tryNames[0] : "?")
                << " GIVE UP after timeout — keeping fallback 0x"
                << std::hex << *e.outPtr << std::dec);
            s_entryState[i] = OffsetState::FallbackGaveUp;
            e.done = true;
        }
        for (int i = 0; i < kFIEntryCount; ++i)
            s_fieldInfoEntries[i].done = true;
        s_entryIdx = kEntryCount;
        s_fiIdx = kFIEntryCount;
        s_initialPassComplete = true;
        s_allDone = true;
        Gjj_OriginY = Gjj_OriginX + 4;
        Gjj_DestY   = Gjj_DestX   + 4;
        Fhoh_DestY  = Fhoh_DestX  + 4;
        Sfx_Pos1Y   = Sfx_Pos1X   + 4;
        Sfx_Pos2Y   = Sfx_Pos2X   + 4;
        return;
    }

    // Budgeted pass — spread IL2CPP metadata walks across frames so the first
    // Present tick during login does not resolve 80+ fields synchronously.
    int budget = kBudgetPerCall;
    const ULONGLONG deadline = GetTickCount64() + kMaxEnsureMs;
    const char*  lastClassName = nullptr;
    Il2CppClass* lastClass     = nullptr;

    while (s_entryIdx < kEntryCount && budget > 0) {
        if (GetTickCount64() >= deadline) break;
        const int i = s_entryIdx++;
        Entry& e = s_entries[i];
        if (e.done) continue;
        // Never walk IL2CPP for lazy instance classes on the render thread during
        // login — MaybeRetryLazyGaveUpEntries() picks them up once in-world.
        if (IsLazyInstanceClass(e.className)) continue;

        Il2CppClass* klass;
        if (e.className == lastClassName) {
            klass = lastClass;
        } else {
            klass = LookupEntryClass(e.className);
            lastClassName = e.className;
            lastClass     = klass;
        }

        if (!klass) continue;

        FieldInfo* found = nullptr;
        const char* foundName = nullptr;
        for (int t = 0; t < e.tryCount && !found; ++t) {
            found = FindFieldOnHierarchy(klass, e.tryNames[t]);
            if (found) foundName = e.tryNames[t];
        }

        const uint32_t fallback = *e.outPtr;
        if (found) {
            const uint32_t resolved = static_cast<uint32_t>(il2cpp_field_get_offset(found)) + e.actkShift;
            if (resolved != fallback) {
                DBG_FILE_LOG("[RuntimeOffsets] " << e.className << "::" << foundName
                    << " resolved -> 0x" << std::hex << resolved
                    << " (fallback was 0x" << fallback << ", SHIFTED)" << std::dec);
            }
            *e.outPtr = resolved;
            s_entryState[i] = (resolved == fallback) ? OffsetState::ResolvedMatch
                                                     : OffsetState::ResolvedShifted;
        } else {
            DBG_FILE_LOG("[RuntimeOffsets] " << e.className << "::"
                << (e.tryCount ? e.tryNames[0] : "?")
                << " FIELD NAME NOT FOUND — using fallback 0x" << std::hex << fallback << std::dec);
            s_entryState[i] = OffsetState::FallbackFieldName;
        }

        e.done = true;
        --budget;
    }

    if (s_entryIdx >= kEntryCount) {
        lastClassName = nullptr;
        lastClass     = nullptr;
        while (s_fiIdx < kFIEntryCount && budget > 0) {
            if (GetTickCount64() >= deadline) break;
            const int i = s_fiIdx++;
            FieldInfoEntry& fe = s_fieldInfoEntries[i];
            if (fe.done) continue;

            Il2CppClass* klass;
            if (fe.className == lastClassName) {
                klass = lastClass;
            } else {
                klass = Resolver::FindClassLoose(fe.className);
                lastClassName = fe.className;
                lastClass     = klass;
            }

            if (!klass) continue;

            FieldInfo* f = FindFieldOnHierarchy(klass, fe.fieldName);
            if (f) *fe.out = f;
            fe.done = true;
            --budget;
        }
    }

    if (s_entryIdx >= kEntryCount && s_fiIdx >= kFIEntryCount)
        s_initialPassComplete = true;

    if (s_initialPassComplete) {
        bool anyPending = false;
        for (int i = 0; i < kEntryCount; ++i) {
            if (s_entries[i].done) continue;
            if (!IsLazyInstanceClass(s_entries[i].className)) anyPending = true;
        }
        for (int i = 0; i < kFIEntryCount; ++i) {
            if (!s_fieldInfoEntries[i].done) anyPending = true;
        }
        if (!anyPending) {
            s_allDone = true;
        } else {
            bool onlyLazy = true;
            for (int i = 0; i < kEntryCount; ++i) {
                if (s_entries[i].done) continue;
                if (!IsLazyInstanceClass(s_entries[i].className)) { onlyLazy = false; break; }
            }
            if (onlyLazy) {
                for (int fi = 0; fi < kFIEntryCount; ++fi) {
                    if (!s_fieldInfoEntries[fi].done) { onlyLazy = false; break; }
                }
            }
            if (onlyLazy) {
                static bool s_lazyFinalized = false;
                if (!s_lazyFinalized) {
                    s_lazyFinalized = true;
                    FinalizeLazyFallbackEntries();
                    BootGate::RefreshAuditIfReady();
                }
                s_allDone = true;
            }
        }
    }

    // ── Vector2 .y derivation pass ────────────────────────────────────────
    // Unity Vector2 lays out {float x, float y} contiguously.
    // il2cpp_field_get_offset gives us x; y is always x+4.
    // We re-derive every call so the Y is always consistent with the resolved X,
    // even before X has been resolved (fallback X + 4 == fallback Y).
    Gjj_OriginY = Gjj_OriginX + 4;
    Gjj_DestY   = Gjj_DestX   + 4;
    Fhoh_DestY  = Fhoh_DestX  + 4;
    Sfx_Pos1Y   = Sfx_Pos1X   + 4;
    Sfx_Pos2Y   = Sfx_Pos2X   + 4;
}

// ── MapObject status conditions (COHCKAPOLCA UInt32[] — offset_map.md) ─────

bool MapObjectConditionsMakeUntargetable(uint32_t word0, uint32_t word1)
{
    // Confirmed from Flash client source: condition_ (COHCKAPOLCA UInt32[2]) is shared by ALL
    // GameObjects — players AND enemies receive CONDITION_STAT / NEW_CON_STAT from the server.
    const uint64_t full = GetFullConditions(word0, word1);
    return HasCondition(full, ConditionEffects::Stasis)       // bit 21 — frozen + immune
        || HasCondition(full, ConditionEffects::Invincible)   // bit 23 — temporary hit immunity
        || HasCondition(full, ConditionEffects::Invulnerable);// bit 24 — permanent immunity
}

bool TryReadMapObjectConditions(void* mapObjectPtr, uint32_t* outWord0, uint32_t* outWord1)
{
    if (outWord0) *outWord0 = 0;
    if (outWord1) *outWord1 = 0;
    if (!mapObjectPtr || !outWord0 || !outWord1)
        return false;
    const uint32_t off = MoConditions;
    if (off == 0)
        return false;

    __try {
        uint8_t* ent = reinterpret_cast<uint8_t*>(mapObjectPtr);
        void* arr = *reinterpret_cast<void**>(ent + off);
        if (!arr)
            return true;
        int32_t maxLen = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(arr) + 0x18);
        // COHCKAPOLCA is always exactly UInt32[2]. Reject anything else as garbage/wrong class.
        if (maxLen != 2)
            return true;
        auto* data = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(arr) + 0x20);
        *outWord0 = data[0];
        *outWord1 = data[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outWord0 = *outWord1 = 0;
        return false;
    }
}

void FormatMapObjectConditionMask(uint32_t word0, uint32_t word1, char* buf, size_t bufSize)
{
    if (!buf || bufSize == 0)
        return;
    buf[0] = '\0';
    if ((word0 | word1) == 0)
        return;

    static const struct { ConditionEffects effect; const char* name; } kEffects[] = {
        { ConditionEffects::Dead,             "Dead"             },
        { ConditionEffects::Quiet,            "Quiet"            },
        { ConditionEffects::Weak,             "Weak"             },
        { ConditionEffects::Slowed,           "Slowed"           },
        { ConditionEffects::Sick,             "Sick"             },
        { ConditionEffects::Dazed,            "Dazed"            },
        { ConditionEffects::Stunned,          "Stunned"          },
        { ConditionEffects::Blind,            "Blind"            },
        { ConditionEffects::Hallucinating,    "Hallucinating"    },
        { ConditionEffects::Drunk,            "Drunk"            },
        { ConditionEffects::Confused,         "Confused"         },
        { ConditionEffects::StunImmune,       "StunImmune"       },
        { ConditionEffects::Invisible,        "Invisible"        },
        { ConditionEffects::Paralyzed,        "Paralyzed"        },
        { ConditionEffects::Speedy,           "Speedy"           },
        { ConditionEffects::Bleeding,         "Bleeding"         },
        { ConditionEffects::ArmorBreakImmune, "ArmorBreakImmune" },
        { ConditionEffects::Healing,          "Healing"          },
        { ConditionEffects::Damaging,         "Damaging"         },
        { ConditionEffects::Berserk,          "Berserk"          },
        { ConditionEffects::Paused,           "Paused"           },
        { ConditionEffects::Stasis,           "Stasis"           },
        { ConditionEffects::StasisImmune,     "StasisImmune"     },
        { ConditionEffects::Invincible,       "Invincible"       },
        { ConditionEffects::Invulnerable,     "Invulnerable"     },
        { ConditionEffects::Armored,          "Armored"          },
        { ConditionEffects::ArmorBroken,      "ArmorBroken"      },
        { ConditionEffects::Hexed,            "Hexed"            },
        { ConditionEffects::NinjaSpeedy,      "NinjaSpeedy"      },
        { ConditionEffects::Unstable,         "Unstable"         },
        { ConditionEffects::Darkness,         "Darkness"         },
    };

    const uint64_t full = GetFullConditions(word0, word1);

    auto append = [&](const char* s) {
        if (!s || !s[0]) return;
        strncat_s(buf, bufSize, s, _TRUNCATE);
    };

    for (const auto& e : kEffects) {
        if (!HasCondition(full, e.effect)) continue;
        append(e.name);
        append(" ");
    }
}

} // namespace RuntimeOffsets

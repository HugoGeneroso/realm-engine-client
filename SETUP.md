# Setup — regenerating stripped build artifacts

To keep the source small enough to distribute, two categories of large,
**regenerable** files are not committed (they're in `.gitignore`):

| Not committed | ~Size | How to regenerate |
|---|---|---|
| `internal/src/game/generated/il2cpp-*.h` | ~94 MB | Dump from your game (step 1) |
| `client/data/objects.xml`, `client/data/tiles.xml`, `internal/data/objects.xml` | ~85 MB | `npm run download-game-xml` (step 2) |

Both regenerate per RotMG build — the game's IL2CPP offsets and obfuscated
names change on every update, so you re-run these after each patch.

You need the **RotMG Exalt** game installed (you need it to inject anyway).

---

## 1. Dump the il2cpp headers

The C++ DLL (`internal/`) `#include`s generated headers that describe the
game's classes, field offsets, and function addresses. Regenerate them with
**Il2CppInspectorPro** (the tool that produced the originals):

1. Get Il2CppInspectorPro: https://github.com/Jadis0x/Il2CppInspectorPro
2. Point it at your install's two files:
   - `…/RotMG Exalt/RotMG Exalt_Data/il2cpp_data/Metadata/global-metadata.dat`
   - `…/RotMG Exalt/GameAssembly.dll`
3. Generate the **C++ scaffold / application headers**.
4. Copy the resulting `il2cpp-*.h` files into:
   ```
   internal/src/game/generated/
   ```
   (`il2cpp-types.h`, `il2cpp-functions.h`, `il2cpp-types-ptr.h`,
   `il2cpp-api-functions.h`, `il2cpp-api-functions-ptr.h`,
   `il2cpp-metadata-version.h`)

Re-dump whenever the game updates.

---

## 2. Download the game XML

The client reads item/projectile/tile data from `objects.xml` + `tiles.xml`.
Fetch them (tries your local game install → official CDN → public mirror):

```bash
cd client

# → client/data/  (read by the client app at startup)
npm run download-game-xml -- --dir ./data

# → %USERPROFILE%/Documents/Realmengine/data/  (read by the DLL skin feature)
npm run download-game-xml
```

Add `-- --force` to re-fetch after a game update.

> The app still boots without these (it logs a warning and those features are
> just incomplete), so this step is recommended, not mandatory to launch.

---

## 3. Build

Once steps 1–2 are done, build as normal — see the README / `client/package.json`
scripts (`npm run build:native`, `npm run build:prod`, `npm run dist`, …).

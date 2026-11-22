# Patch packs

A patch pack is a JSON file of built-in patches for one module of one build of one game. Packs are
in `dist/patch-packs/`. Each patch in a pack is a built-in add-on like those compiled into
`src/core/file_sys/patch_manager.cpp`: Add-Ons lists it, `[DisabledAddOns]` stores it, and
`PatchManager::PatchNSO` applies it after the same checks. A patch for any game can be added by
a pull request with no C++. Ocarina of Time's three Text Speed add-ons are the first pack:
`0400486024DEC000-text-speed.json`.

## Where packs come from

- The build copies `dist/patch-packs/` to `patch-packs/` next to every program that links `core`:
  `guarana`, `guarana-cli` and `tests` (the `patch_packs` target in `src/core/CMakeLists.txt`).
- A macOS bundle keeps them in `Contents/Resources/patch-packs`.
- The release workflow ships the folder. The Windows zip is the `bin` folder, which holds it. The
  AppImage and the Linux tarball copy it to `usr/bin/patch-packs`, and the macOS bundle carries it.
  The Windows release and test builds fail when it holds no pack.
- The build also compiles every pack into `core` (`FileSys::EmbeddedPatchPacks`, written by
  `CMakeModules/EmbedPatchPacks.cmake`). A program with no folder beside it keeps the packs:
  Android, `guarana-cli` inside a macOS bundle, or a program moved without its folder.
- `FileSys::ShippedPatchPackDirectory` finds the folder: next to the executable on Windows and
  Linux (`/proc/self/exe`). On macOS it looks in the bundle's `Contents/Resources`, then in
  `../Resources` and next to the executable (`_NSGetExecutablePath`), for `guarana-cli` in
  `Contents/MacOS`.
- `LoadPatchPacks` reads every `*.json` file there once, on first use. A file there replaces the
  compiled-in copy of the same file name, so the shipped packs stay editable. When that file
  cannot be used, the log says why and the compiled-in copy is used instead. The packs come in
  file name order, whichever copy each is read from. A missing folder is not a problem.
- The log says how many built-in patches came from packs and where they were read
  (`Patch packs: N built-in patches; P packs from <folder>, C compiled in`). It names every pack
  that was left out and why: a problem in the file, or a name that was already taken.

The built-ins compiled in come first, then the packs' patches, each pack in its own order. Order
matters in two places:

- In a group, the first patch that applies wins.
- A patch must come before the one it `replaces`.

## The format

```json
{
  "document_type": "guarana_patch_pack",
  "schema_version": 1,
  "game": "The Legend of Zelda: Ocarina of Time",
  "title_id": "0400486024DEC000",
  "module": "main",
  "build_id": "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348",
  "offset_basis": "module",
  "comment": ["For reviewers: what the patches do, where they come from."],
  "patches": [
    {
      "name": "Text Speed 2x (built-in)",
      "description": "Dialogue, the fairy's hints and signs appear 2 times faster. ...",
      "level": "experimental",
      "group": "Text Speed",
      "off_by_default": true,
      "records": [
        {
          "offset": "0xAFE1B4",
          "bytes": "5323C514",
          "original_sha256": "6B699EB58B07460F4CC4C59EDB1AE2E9E418AE130BFF703F732D9A600E98C9F3",
          "comment": "b main+0x3C46F00"
        },
        { "offset": "0x3C46F00", "bytes": "9000001C 0008301E 69DE4079 ABDC3A17 00000040", "cave": true }
      ]
    }
  ]
}
```

### The pack

| Field | Required | What it holds |
|---|---|---|
| `document_type` | yes | `"guarana_patch_pack"` |
| `schema_version` | yes | `1` |
| `game` | yes | The game's name, for reviewers, at most 128 bytes |
| `title_id` | yes | The program id: 16 hex digits |
| `module` | yes | `main`, `rtld`, `sdk` or `subsdk0` to `subsdk9` |
| `build_id` | yes | The module's build id: 2 to 64 hex digits. Case and trailing zeros do not matter, so the 40-digit and the 64-digit forms are the same build. |
| `offset_basis` | yes | `"module"`: offsets count from the module's start, as `main+0x...` in the log. An IPS offset includes the 0x100-byte NSO header; subtract it. |
| `comment` | no | A string or a list of strings. The program does not read it. |
| `patches` | yes | 1 to 64 patches |

### A patch

| Field | Required | What it holds |
|---|---|---|
| `name` | yes | Printable ASCII, at most 128 bytes, ending with ` (built-in)`, not starting with `+`. It is the add-on's `[DisabledAddOns]` key: never rename a patch that has shipped. No two built-ins of one game share a name. |
| `description` | yes | What the patch does, for a player, in 40 to 1000 bytes, ending with a full stop. Add-Ons shows it. |
| `level` | no | `standard` (default), `recommended` (leave it on) or `experimental`. A patch that is off by default is never `recommended`. |
| `group` | no | Patches that change the same thing. Of those checked, the first that applies wins. |
| `replaces` | no | The name of a patch later in this pack that this one contains. It is skipped once this one applies. |
| `off_by_default` | no | `true`: off until checked in Add-Ons, then stored as `+<name>` |
| `state` | no | `{ "offset": "0x...", "size": "0x..." }`: 1 to 0x10000 bytes of the module's BSS that the patch's routines keep their state in |
| `records` | yes | 1 to 256 runs of bytes the patch writes |
| `comment` | no | As for the pack |

### A record

| Field | Required | What it holds |
|---|---|---|
| `offset` | yes | `0x` and 1 to 8 hex digits, from the module's start |
| `bytes` | yes | 1 to 4096 bytes in hex, two digits a byte, with spaces only between bytes |
| `cave` | no | `true`: the bytes go into the executable padding after the module's text |
| `original_sha256` | unless `cave` | The SHA-256 of the game's bytes at `offset`, before any mod |
| `original_size` | no | The number of bytes the hash covers, `0x` and hex digits. It defaults to the size of `bytes` and may be larger, to guard instructions the patch relies on, never smaller. |
| `comment` | no | As for the pack |

A record carries the hash of the game's bytes, not the bytes themselves, so a pack holds no game
code. A hash of a single 4-byte instruction can be found again by trying every value, so it does
not keep the instruction secret. The point is that no game code is in the file.

## What is checked

When the game's module loads, `PatchNSO` applies a pack patch only when every one of these holds,
as for a built-in compiled in:

- The title, the module and the build id match.
- Every record lies inside the module.
- Every guarded record's `original_size` bytes in the game's own module hash to `original_sha256`.
- A cave lies in the executable padding: after the text, below the next page boundary and the
  read-only segment, and all zero.
- The state block lies in the BSS and is all zero.
- No user mod (IPS, IPSwitch) changed a byte the patch writes, or its state block. User mods
  always win.

Any failure skips the whole patch. Add-Ons shows the reason (`BuiltinPatchResult`), and its
`replaces` patch can still apply.

`ParsePatchPack` rejects the whole file on any problem, before anything applies:

- a field the format does not have, so a typo such as `off_by_defualt` cannot flip a default;
- a key twice in one object;
- a missing or malformed field;
- a size over its limit, the file over 1 MiB, or a record past 512 MiB into the module;
- a write over the game's code that checks nothing, or checks fewer bytes than it writes;
- a cave with a hash;
- two patches of one name;
- a `replaces` that does not name a later patch;
- a patch that is off by default and `recommended`.

A pack whose patch takes the name of another built-in of the same game is left out at load.

CI runs the same checks on the shipped packs in the unit tests (`src/tests/core/builtin_patches.cpp`):

- **Shipped patch packs pass the pack checks:** every file reads without an error, and none is left
  out. File names are `<program id>-<what it does>.json`, and every patch has a description.
- **Built-in patches that can be applied together never touch the same bytes:**
  `FindBuiltinPatchOverlaps` runs on the compiled-in built-ins and the packs together, through
  `BuiltinPatchRanges`. A guarded record's range is the larger of what it writes and what it checks.
- **The shipped patch packs are compiled in as well:** with no folder, or no path to look in, the
  compiled-in copies read without an error and equal the folder's packs, record for record.
- **A pack in the folder replaces the compiled-in copy of its name:** an edited copy wins, a copy
  that does not read gives way to the compiled-in one and says why, and packs of other names join
  in file name order.
- **Patch packs are read strictly** has one case per rule above.
- **A patch pack gives a game built-ins without any C++** runs a made-up game's pack through
  `HasNSOPatch`, `GetPatches`, `PatchNSO` and `StoredDisabledAddons`.

A pull request that only adds a pack still runs the build and these tests: `build-check` skips only
documentation.

## Adding a pack

1. Write the patch as you would an IPSwitch or IPS mod, and test it in the game.
2. Take the build id from the log (`Patching NSO for name=main, build_id=...`), or from the file
   name of an IPS mod for that build.
3. For each record that writes over the game's code, hash the bytes it replaces from your own
   dump of the module, and keep the dump on your machine:

   ```sh
   python -c "import hashlib,sys; print(hashlib.sha256(bytes.fromhex(sys.argv[1])).hexdigest().upper())" 69DE4079
   ```

4. Name the file `<program id>-<what it does>.json`. Give every patch a description for players.
5. Open a pull request with a `changelog.d` fragment. CI builds it and runs the checks above. If two
   packs or a pack and a compiled-in built-in write the same bytes, the overlap test names both.

## Lichii #1358

zortos293/Lichii#1358 ("Nextendo: signed NPLN patch catalogue and ALPN") was an open draft when
this format was designed. It merged upstream on 2026-11-16 as 6cffb2390d and reaches Guarana
through the upstream merge.

It fetches a signed manifest per title and build from Nextendo's service. Each patch is an IPS32
blob plus "guards", and the NSO loader applies it after mods while online play is on. It is a
separate mechanism with its own hook in `loader/nso.cpp`. Packs do not copy its code, which the
upstream merge brings as it is.

Taken from it:

- the field names and forms: `title_id` as 16 hex digits, `build_id`, `module`, records as an
  offset with hex bytes, `document_type` with `schema_version`;
- strict reading with size and count caps: 1 MiB per document, at most 256 writes a patch, nothing
  past 512 MiB;
- rejecting a patch with no guard, and two entries with one identity;
- an explicit offset basis.

Not taken:

- **The signature, validity window, revision and rollback rules.** Packs ship with the program and
  are reviewed in pull requests. A remote catalogue needs them; see "Next steps".
- **The IPS32 envelope next to the guards.** A pack's records are the writes.
- **`original_hex`, the game's bytes in the clear.** A pack carries their SHA-256 and size.
- **Offsets that include the NSO header.** #1358 uses `nso_header_included`; packs use module
  offsets, as the built-ins, the logs and the overlap test do. `offset_basis` says which.
- **`main` only.** A pack may patch any executable module.
- **Accepting a guard whose bytes are already patched.** #1358 takes the original or the patched
  bytes. A built-in takes only the original:
  - patched bytes in the game's own module are unexpected code;
  - bytes a user mod wrote are a mod conflict, as before.

## What this does not cover

- **Android** has no folder next to the program. It uses the compiled-in copies, so its packs
  change only with a new build. No pull request check builds Android.
- **macOS and the release packaging** are not built by a pull request's CI:
  - build-check compiles Linux GCC and Windows clang-cl only;
  - the macOS lookups (`GetBundleDirectory`, `_NSGetExecutablePath`) and the bundle copy were
    written to match the code around them. If they miss the folder, the compiled-in copies load;
  - the AppImage and Windows zip steps run in `release-binaries.yml`;
  - a run of that workflow on a branch builds every platform without publishing.
- **Compiled-in built-ins stay where they are.** The DLSS render scales follow a setting, and Max
  Resolution feeds the render scale switch: a pack has no field for either. FPS Unlock, Dynamic FPS
  and Texture Pool could move as they are; that is a migration of its own.

## Next steps

- **A remote catalogue:** download signed packs per title and build, with the signature, validity
  and rollback rules of #1358, into a cache folder that `LoadPatchPacks` reads after the shipped
  one.
- **A user's own packs:** a `patch-packs` folder in the user directory, loaded after the shipped
  ones, with the same checks. A name that is already taken leaves the pack out.
- **Moving the other built-ins:** FPS Unlock, Dynamic FPS and Texture Pool.

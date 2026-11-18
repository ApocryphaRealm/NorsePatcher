# Changelog

## 0.1.0 - 2026-11-17 (tested in game: TEST-RESULT-0.1.0.txt)
### Added (later, 2026-11-17)
- Config files follow NorseManifest: files of switched-off packs are skipped, the rest are read in pack load order, loose files last; the framework ships its own manifest (package/NorsePacks).
### Added
- Patch files in `patches\*.ini`: `[object path]` sections, `Field = value` lines in Unreal's text form, `*=` / `+=` for
  numbers. Data tables take `Row.Field` (`*` = every row); a class patches its default object and every live instance.
- Applied at every map load; console `np.reload` (re-read and apply), `np.apply`, `np.dump <object path>`.
- `Dump=` lines in NorsePatcher.ini: every field and value of an object written to `dumps\` - how field names are found.
- File-name order decides a shared field; the log names the file that wins. Missing objects, rows and fields warn with
  file and line.
### Fixed (during the first test)
- Callbacks are registered in `on_unreal_init`; registered in the constructor they were dropped silently.
- Dumps print default values (empty arrays, False, 0) instead of `<unreadable>`.

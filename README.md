# NorsePatcher

Runtime data patching for NORSE: Oath of Blood - a UE4SS v4.0.0 C++ mod. Plain-text patch files change data-table
rows, data assets and class defaults while the game runs. The game's paks are never edited: remove a patch file and the
original values are back at the next start.

It is the first of the framework layer in the Norse overhaul plan
("NorsePatcher (SkyPatcher-like)"). Prior art checked 2026-11-17: Norse has no data-patching mod on Nexus; the closest
model is PalSchema for Palworld (a UE4SS framework that edits DataTables and Blueprints from JSON, field-level, with live
reload). NorsePatcher takes its field-level edits and live reload, and SkyPatcher's plain-text, one-line-per-change form.

## Patch files

`ue4ss\Mods\NorsePatcher\patches\*.ini`, read in file-name order; a later file wins a field it shares with an earlier
one, and the log names the winner. Full author guide: `package\patches\README.txt`. Worked examples: `docs\examples\`.

```ini
[/Game/Norse/Core/Data/CombatUnits/EnemyTypes/Outcasts/Outcast_Nithing.Outcast_Nithing]
Stats = (Health=(base=40))          ; a struct: only the fields named change
CombatRatingScale *= 2              ; numbers: *= and +=

[/Game/Norse/Core/Data/Tables/DT_Stats.DT_Stats]
EStat::Health.Icon = /Game/Norse/UI/Assets/Icons/Attributes/T_Atribute_Armor_01.T_Atribute_Armor_01   ; Row.Field, * = every row

[/Game/Norse/Characters/MetaHumans/BP_Metahuman_Unit.BP_Metahuman_Unit_C]
SomeField = 1                       ; a class: its default object and every live instance
```

Values are written in Unreal's own text form (`FProperty::ImportText_Direct`), so every property type works the way
Unreal itself reads it. Applied at every map load (the game reloads some assets between maps - seen in the first test)
and by the console commands `np.reload`, `np.apply`, `np.dump <object path>`.

## How it works

- Objects are found by path, or loaded (`KismetSystemLibrary.LoadAsset_Blocking`), as in NorseRealtimeNative.
- Fields are found by walking the struct's own property list (`UStruct::GetChildProperties`, `FField::GetNext`) up the
  inheritance chain; Blueprint struct fields match by the designer's name (the `_<n>_<GUID>` suffix is ignored).
- Data-table rows come from `UDataTable::RowMap`, read by hand and checked against the engine's own
  `GetDataTableRowNames` before any row is touched; a mismatch leaves the table alone.
- No function taking an `FName` by value is called (this mod cannot promise UE4SS's FName size).
- A parse error is caught by a stand-in `FOutputDevice` (UE4SS exports no GWarn/GLog) and an SEH guard.

## Build

`xmake f -p windows -a x64 -m release --toolchain=msvc -y`, then `xmake`. Links against an import library made from the
installed UE4SS.dll (`tools\make-importlib.ps1`; `build\ue4ss\` is copied from NorseRealtimeNative). Output
`build\windows\x64\release\main.dll` goes to `ue4ss\Mods\NorsePatcher\dlls\`, with `enabled.txt`, `NorsePatcher.ini`
and `patches\` beside it.

Log: `NorsePatcher.log` beside `NorsePatcher.ini` (or `sLogDir`): one line per value set, before -> after, with the
file and line that set it.

## Licence

GPL-3.0-or-later (see LICENSE).

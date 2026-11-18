NorsePatcher patch files
========================

Every *.ini file in this folder is read, in file-name order. A later file wins a field it shares with an earlier one,
and the log says which file won. Nothing in the game's own files is changed: remove a patch file and the original
values are back at the next start.

A section names the object to change, by its full object path. The lines under it set fields:

    [/Game/Norse/Core/Data/Tables/DT_Stats.DT_Stats]
    ; a data table: Row.Field = value     (* as the row = every row)
    Strength.MaxValue = 30

    [/Game/Norse/Core/Data/CombatUnits/EnemyTypes/Basic/Basic_Warrior.Basic_Warrior]
    ; any other asset: Field = value
    SomeNumber = 12

    [/Game/Norse/Characters/MetaHumans/BP_Metahuman_Unit.BP_Metahuman_Unit_C]
    ; a class: its default values and every unit of that class already spawned
    SomeFlag = true

    SomeNumber *= 2      multiply the current number
    SomeNumber += 5      add to the current number

Values are written the way Unreal writes them itself: 12, 0.5, true, EnumValue, a struct as (X=1,Y=2) (fields not
named keep their values), an array as (1,2,3), an asset as /Game/Path/Asset.Asset. Comments start with ; or #.

Finding field names: in-game console, np.dump <object path> writes every field and its current value to
dumps\<name>.txt beside NorsePatcher.ini (or add Dump= lines to NorsePatcher.ini). Blueprint struct fields are listed
by the name the designer gave them; that is the name to use here.

Console: np.reload (re-read these files and apply), np.apply (apply again), np.dump <object path>.
The log is NorsePatcher.log beside NorsePatcher.ini: one line per value set, before -> after.

-- NorsePatcher - UE4SS v4.0.0 C++ mod for NORSE: Oath of Blood (runtime data patching).
-- Build:  powershell -File tools\make-importlib.ps1 -Ue4ssDll <path to the UE4SS.dll the game loads>   (once per UE4SS build)
--         xmake f -m release -y && xmake build
-- Output: build\windows\x64\release\main.dll + main.pdb

set_project("NorsePatcher")
local version = "0.1.0"
set_version(version)
set_xmakever("2.8.0")

add_rules("mode.release")
set_defaultmode("release")
set_languages("c++20")
-- UE4SS release builds use the dynamic release CRT; a C++ mod must match it (shared std::wstring/std::vector/std::function).
set_runtimes("MD")

target("main")
    set_kind("shared")
    add_files("src/*.cpp")
    add_includedirs("include")
    add_defines("NP_VERSION=\"" .. version .. "\"")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE")
    add_cxflags("/EHsc", "/W4", "/permissive-", "/utf-8", {force = true})
    add_linkdirs("build/ue4ss")
    add_links("UE4SS", "user32")
    set_symbols("debug")
    set_optimize("faster")
    -- rule 45: no build-machine paths in the binary (TABLE form, gate row xmake-d1trimfile-reaches-cl-intact)
    add_cxflags({"/d1trimfile:$(projectdir)"}, {force = true, expand = false})
    add_shflags("/PDBALTPATH:%_PDB%", {force = true})
    add_shflags("/DEBUG", "/OPT:REF", "/OPT:ICF", {force = true})
    before_build(function (target)
        if not os.isfile(path.join(os.projectdir(), "build", "ue4ss", "UE4SS.lib")) then
            raise("build\\ue4ss\\UE4SS.lib missing - run tools\\make-importlib.ps1 -Ue4ssDll <UE4SS.dll> first")
        end
    end)

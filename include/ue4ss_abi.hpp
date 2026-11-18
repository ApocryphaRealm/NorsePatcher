#pragma once
// ue4ss_abi.hpp - the minimal slice of the UE4SS v4.0.0 C++ API NorsePatcher uses (copied from NorseRealtimeNative 0.5.27, extended), declared to match the EXPORTED
// symbols of UE4SS.dll exactly (MSVC name mangling), so the mod links against an import library generated from the
// installed UE4SS.dll (tools\make-importlib.ps1) instead of the full RE-UE4SS source tree.
//
// Why: building RE-UE4SS's own headers needs its private UEPseudo submodule (Epic-account-gated GitHub access).
// Every declaration below mirrors a public header of RE-UE4SS v4.0.0 (commit 30afe5ff, the installed build):
//   - RC::CppUserModBase: UE4SS/include/Mod/CppUserModBase.hpp (layout AND virtual order copied verbatim - MSVC
//     orders overloaded virtuals by declaration, so the declarations must stay in this exact order).
//   - RC::Unreal::*: signatures read from the UE4SS.dll export table (dumpbin /exports).
// A wrong signature here fails at LINK time (unresolved external), never silently at runtime - that is the point of
// linking against the generated import library rather than resolving by GetProcAddress.
//
// Rule: never add a data member, a virtual, or reorder anything in CppUserModBase without re-reading the header of the
// exact UE4SS version installed.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define NRN_UE4SS_IMPORT __declspec(dllimport)

namespace RC
{
    using StringType = std::wstring;
    using StringViewType = std::wstring_view;

    namespace GUI
    {
        class GUITab;
    }
    namespace LuaMadeSimple
    {
        class Lua;
    }

    class CppUserModBase
    {
      protected:
        std::vector<std::shared_ptr<GUI::GUITab>> GUITabs{};

      public:
        StringType ModName{};
        StringType ModVersion{};
        StringType ModDescription{};
        StringType ModAuthors{};
        StringType ModIntendedSDKVersion{};

      public:
        NRN_UE4SS_IMPORT CppUserModBase();
        NRN_UE4SS_IMPORT virtual ~CppUserModBase();

      public:
        virtual auto on_update() -> void {}
        virtual auto on_unreal_init() -> void {}
        virtual auto on_ui_init() -> void {}
        virtual auto on_program_start() -> void {}
        virtual auto on_lua_start(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
        virtual auto on_lua_start(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
        virtual auto on_lua_stop(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
        virtual auto on_lua_stop(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) -> void {}
        virtual auto on_dll_load(StringViewType) -> void {}
        virtual auto render_tab() -> void {}
        virtual auto on_lua_start(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void {}
        virtual auto on_lua_start(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void {}
        virtual auto on_lua_stop(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void {}
        virtual auto on_lua_stop(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) -> void {}
        virtual auto on_cpp_mods_loaded() -> void {}
    };

    namespace Unreal
    {
        class UClass;
        class UFunction;
        class UEngine;
        class AGameModeBase;
        class AActor;
        class FOutputDevice;
        class FProperty;

        // RC::Unreal::EFindName / FName (0.4.0): only the exported constructor
        // ??0FName@Unreal@RC@@QEAA@PEB_WW4EFindName@12@PEAX@Z is used. The class is declared with a large tail so a
        // caller's storage is never smaller than UE4SS's own FName; the engine value is the first 8 bytes
        // (ComparisonIndex, Number).
        enum EFindName
        {
            FNAME_Find,
            FNAME_Add
        };
        class FName
        {
          public:
            NRN_UE4SS_IMPORT FName(const wchar_t* StrName, EFindName FindType = FNAME_Add, void* FunctionAddressOverride = nullptr);
            // ?ToString@FName@Unreal@RC@@QEAA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@XZ (NorsePatcher)
            NRN_UE4SS_IMPORT std::wstring ToString();
            uint32_t ComparisonIndex;
            uint32_t Number;
            uint8_t Reserve[48];
        };

        class UObjectBase
        {
          public:
            NRN_UE4SS_IMPORT UClass*& GetClassPrivate();
        };

        class UObject : public UObjectBase
        {
          public:
            NRN_UE4SS_IMPORT UFunction* GetFunctionByNameInChain(const wchar_t* ShortFunctionName);
            NRN_UE4SS_IMPORT void ProcessEvent(UFunction* Function, void* Parms);
            NRN_UE4SS_IMPORT void* GetValuePtrByPropertyNameInChain(const wchar_t* PropertyName);
            NRN_UE4SS_IMPORT std::wstring GetFullName(UObject* StopOuter = nullptr) const;
            // Keep an object alive whatever refers to it. An asset loaded at start-up can sit outside garbage-collection
            // tracking, so its references do not keep a newly loaded object alive (NorseSound 0.1.0: a sound swapped
            // into an audio event was collected at the next map change and the event's field read as freed memory).
            NRN_UE4SS_IMPORT void SetRootSet(); // ?SetRootSet@UObject@Unreal@RC@@QEAAXXZ
            NRN_UE4SS_IMPORT bool IsRootSet();  // ?IsRootSet@UObject@Unreal@RC@@QEAA_NXZ
        };

        class UFunction
        {
          public:
            NRN_UE4SS_IMPORT uint16_t& GetParmsSize();
        };

        // ---- NorsePatcher 0.1.0: the reflection slice (property walk, text import/export). Each member is an export of
        // UE4SS.dll v4.0.0 (build\ue4ss\UE4SS.def); the mangled name is in the comment. Nothing here takes an FName by
        // value: this header cannot promise UE4SS's FName size, so by-value FName functions are never declared.
        class FString
        {
          public:
            NRN_UE4SS_IMPORT FString();  // ??0FString@Unreal@RC@@QEAA@XZ
            NRN_UE4SS_IMPORT ~FString(); // ??1FString@Unreal@RC@@QEAA@XZ
            FString(const FString&) = delete;
            FString& operator=(const FString&) = delete;
            const wchar_t* c_str() const { return (Data && Num > 0) ? Data : L""; }
            wchar_t* Data = nullptr; // TArray<TCHAR>: allocation, Num (with the terminator), Max
            int32_t Num = 0;
            int32_t Max = 0;
            uint8_t Reserve[16]{}; // headroom in case UE4SS's FString is larger than the engine's 16 bytes
        };

        class FField
        {
          public:
            // ?GetName@FField@Unreal@RC@@QEBA?AV?$basic_string@_WU?$char_traits@_W@std@@V?$allocator@_W@2@@std@@XZ
            NRN_UE4SS_IMPORT std::wstring GetName() const;

          private:
            // ?GetNext@FField@Unreal@RC@@AEBAAEAPEBV123@XZ - private in UE4SS, reached through FieldWalk
            NRN_UE4SS_IMPORT const FField*& GetNext() const;
            friend struct FieldWalk;
        };

        class FProperty : public FField
        {
          public:
            NRN_UE4SS_IMPORT const int32_t& GetOffset_Internal() const; // ?GetOffset_Internal@FProperty@Unreal@RC@@QEBAAEBHXZ
            NRN_UE4SS_IMPORT int32_t GetSize() const;                   // ?GetSize@FProperty@Unreal@RC@@QEBAHXZ
            // ?GetCPPType@FProperty@Unreal@RC@@QEBA?AVFString@23@PEAV423@I@Z
            NRN_UE4SS_IMPORT FString GetCPPType(FString* ExtendedTypeText, uint32_t CPPExportFlags) const;
            // ?ImportText_Direct@FProperty@Unreal@RC@@QEBAPEB_WPEB_WPEAXPEAVUObject@23@HPEAVFOutputDevice@23@@Z
            NRN_UE4SS_IMPORT const wchar_t* ImportText_Direct(const wchar_t* Buffer, void* PropertyPtr, UObject* OwnerObject, int32_t PortFlags,
                                                              FOutputDevice* ErrorText) const;
            // ?ExportText_Direct@FProperty@Unreal@RC@@QEAA_NAEAVFString@23@PEBX1PEAVUObject@23@H2@Z
            NRN_UE4SS_IMPORT bool ExportText_Direct(FString& ValueStr, const void* Data, const void* Delta, UObject* Parent, int32_t PortFlags,
                                                    UObject* ExportRootScope);
        };

        struct FieldWalk
        {
            static const FField* Next(const FField* f) { return f ? f->GetNext() : nullptr; }
        };

        class UStruct : public UObject
        {
          public:
            NRN_UE4SS_IMPORT FField*& GetChildProperties();         // ?GetChildProperties@UStruct@Unreal@RC@@QEAAAEAPEAVFField@23@XZ
            NRN_UE4SS_IMPORT UStruct* GetInheritanceSuper() const;  // ?GetInheritanceSuper@UStruct@Unreal@RC@@QEBAPEAV123@XZ
        };

        class UClass : public UStruct
        {
        };

        // NorseLoot (2026-11-17): reaching lists nested in structs and other lists
        template <typename T>
        class TObjectPtr
        {
          public:
            T* Ptr;
        };
        class UScriptStruct : public UStruct
        {
        };
        class FArrayProperty : public FProperty
        {
          public:
            NRN_UE4SS_IMPORT FProperty*& GetInner(); // ?GetInner@FArrayProperty@Unreal@RC@@QEAAAEAPEAVFProperty@23@XZ
        };
        class FStructProperty : public FProperty
        {
          public:
            // ?GetStruct@FStructProperty@Unreal@RC@@QEAAAEAV?$TObjectPtr@VUScriptStruct@Unreal@RC@@@23@XZ
            NRN_UE4SS_IMPORT TObjectPtr<UScriptStruct>& GetStruct();
        };

        // UE4SS's FText wrapper: a properly constructed text value to pass to a UFunction (NMMF 0.1.0: importing
        // INVTEXT(...) into an uninitialised parameter buffer failed every time - TextBlock.SetText never ran).
        // The storage is generous: UE4SS keeps the engine FText inside (MaxStorageSize), we never read it ourselves.
        class FText
        {
          public:
            NRN_UE4SS_IMPORT FText(const wchar_t* Str);           // ??0FText@Unreal@RC@@QEAA@PEB_W@Z
            NRN_UE4SS_IMPORT ~FText();                            // ??1FText@Unreal@RC@@QEAA@XZ
            NRN_UE4SS_IMPORT void CopyBorrowedTo(void* Dest) const; // ?CopyBorrowedTo@FText@Unreal@RC@@QEBAXPEAX@Z
            NRN_UE4SS_IMPORT static int StaticSize();             // ?StaticSize@FText@Unreal@RC@@SAHXZ
          private:
            alignas(16) uint8_t m_storage[256]{};
        };

        // An object's slot in the global object array (NorseHUD 0.1.1: a unit destroyed between refreshes)
        struct FUObjectItem
        {
            NRN_UE4SS_IMPORT bool IsPendingKill() const; // ?IsPendingKill@FUObjectItem@Unreal@RC@@QEBA_NXZ
            NRN_UE4SS_IMPORT bool IsUnreachable() const; // ?IsUnreachable@FUObjectItem@Unreal@RC@@QEBA_NXZ
        };
        class FUObjectArray
        {
          public:
            NRN_UE4SS_IMPORT static FUObjectItem* IndexToObject(int32_t Index); // ?IndexToObject@FUObjectArray@Unreal@RC@@SAPEAUFUObjectItem@23@H@Z
        };

        class FMemory
        {
          public:
            NRN_UE4SS_IMPORT static void Free(void* Original); // ?Free@FMemory@Unreal@RC@@SAXPEAX@Z
        };

        namespace UObjectGlobals
        {
            NRN_UE4SS_IMPORT UObject* FindFirstOf(const wchar_t* ClassName);
            NRN_UE4SS_IMPORT void FindAllOf(const wchar_t* ClassName, std::vector<UObject*>& OutStorage);
            NRN_UE4SS_IMPORT UObject* StaticFindObject_InternalSlow(UClass* ObjectClass, UObject* InObjectPackage, const wchar_t* OrigInName, bool bExactClass);
        } // namespace UObjectGlobals

        namespace Hook
        {
            NRN_UE4SS_IMPORT void RegisterEngineTickPreCallback(std::function<void(UEngine*, float)>);
            NRN_UE4SS_IMPORT void RegisterProcessConsoleExecCallback(std::function<bool(UObject*, const wchar_t*, FOutputDevice&, UObject*)>);
            NRN_UE4SS_IMPORT void RegisterInitGameStatePostCallback(std::function<void(AGameModeBase*)>);
            // NorseHUD: runs after every ProcessEvent, also for a Blueprint event nothing implements (ReceiveDrawHUD)
            NRN_UE4SS_IMPORT void RegisterProcessEventPostCallback(std::function<void(UObject*, UFunction*, void*)>);
            // NorseEncounters: every actor's BeginPlay, streamed sub-levels included (a spawn point before it spawns)
            NRN_UE4SS_IMPORT void RegisterBeginPlayPreCallback(std::function<void(AActor*)>);
            NRN_UE4SS_IMPORT void RegisterBeginPlayPostCallback(std::function<void(AActor*)>);
        } // namespace Hook
    } // namespace Unreal
} // namespace RC

#pragma once
// NorseReflect.hpp - the shared reflection layer of the Norse frameworks (NorsePatcher, NorseDistributor).
// Moved out of NorsePatcher 0.1.0's main.cpp unchanged, 2026-11-17. Text helpers, object lookup and loading, the
// property walk, Unreal text import/export with guards, data-table rows (checked against the engine's row names), and
// np.dump-style dumps. Included by exactly ONE source file per mod, so it lives in an unnamed namespace there.
#include <Windows.h>

#include "Log.hpp"
#include "Ue.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace norse
{
namespace
{
    namespace fs = std::filesystem;
    using namespace nrn;
    using namespace nrn::ue;
    using RC::Unreal::FField;
    using RC::Unreal::FieldWalk;
    using RC::Unreal::FName;
    using RC::Unreal::FProperty;
    using RC::Unreal::FString;
    using RC::Unreal::UStruct;

    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    std::wstring Trim(const std::wstring& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && iswspace(s[a])) ++a;
        while (b > a && iswspace(s[b - 1])) --b;
        return s.substr(a, b - a);
    }

    std::wstring Lower(std::wstring s)
    {
        for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    }

    // Reads a UTF-8 text file (BOM skipped) into lines.
    std::vector<std::wstring> ReadLines(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (all.size() >= 3 && static_cast<unsigned char>(all[0]) == 0xEF && static_cast<unsigned char>(all[1]) == 0xBB &&
            static_cast<unsigned char>(all[2]) == 0xBF)
            all.erase(0, 3);
        std::vector<std::wstring> out;
        std::wstringstream ss(Widen(all));
        std::wstring line;
        while (std::getline(ss, line))
        {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            out.push_back(line);
        }
        return out;
    }

    // A ';' or '#' that follows whitespace and sits outside double quotes starts a comment.
    std::wstring StripComment(const std::wstring& s)
    {
        bool q = false;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == L'"') q = !q;
            if (!q && (s[i] == L';' || s[i] == L'#') && (i == 0 || iswspace(s[i - 1]))) return s.substr(0, i);
        }
        return s;
    }

    std::wstring ShortName(const std::wstring& path)
    {
        size_t d = path.find_last_of(L"./:"); // ":" too: a sub-object (Asset.Asset:TraderProfile_0) - NorseLoot, 2026-11-17
        return d == std::wstring::npos ? path : path.substr(d + 1);
    }

    // ------------------------------------------------------------------ reflection
    UObject* FindObj(const wchar_t* path)
    {
        try
        {
            return RC::Unreal::UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, path, false);
        }
        catch (...)
        {
            return nullptr;
        }
    }

    // Loads an asset by object path (already loaded: found directly). KismetSystemLibrary MakeSoftObjectPath ->
    // Conv_SoftObjPathToSoftObjRef -> LoadAsset_Blocking, as in NorseRealtimeNative 0.5.x.
    UObject* LoadByPath(const wchar_t* path);

    // Load an asset a rule puts into the game's data and mark it root set. NOT enough on its own: NorseModelSwap 0.1.0
    // (2026-11-18) saw rooted assets freed at a map change anyway. Keep IndexOf(result) and check Alive() before
    // each use; load it again when it was freed.
    UObject* LoadKept(const wchar_t* path)
    {
        UObject* o = LoadByPath(path);
        if (o)
        {
            try
            {
                if (!o->IsRootSet()) o->SetRootSet();
            }
            catch (...)
            {
            }
        }
        return o;
    }

    UObject* LoadByPath(const wchar_t* path)
    {
        if (UObject* o = FindObj(path)) return o;
        static UObject* ksl = nullptr;
        if (!ksl) ksl = FindObj(L"/Script/Engine.Default__KismetSystemLibrary");
        if (!ksl) return nullptr;
        struct FStr
        {
            const wchar_t* data;
            int32_t num, max;
        } str{path, static_cast<int32_t>(std::wcslen(path) + 1), static_cast<int32_t>(std::wcslen(path) + 1)};
        uint16_t ps1 = Reflect::Get().ParmsSizeOf(ksl, L"MakeSoftObjectPath");
        uint16_t ps2 = Reflect::Get().ParmsSizeOf(ksl, L"Conv_SoftObjPathToSoftObjRef");
        uint16_t ps3 = Reflect::Get().ParmsSizeOf(ksl, L"LoadAsset_Blocking");
        if (!ps1 || !ps2 || !ps3 || ps1 > 200 || ps2 > 200 || ps3 > 200) return nullptr;
        Buf a;
        std::memcpy(a.b, &str, sizeof(str));
        if (!Reflect::Get().Call(ksl, L"MakeSoftObjectPath", a.b, ps1, sizeof(a.b))) return nullptr;
        size_t pathSize = ps1 - 0x10;
        Buf b;
        std::memcpy(b.b, a.b + 0x10, pathSize);
        if (!Reflect::Get().Call(ksl, L"Conv_SoftObjPathToSoftObjRef", b.b, ps2, sizeof(b.b))) return nullptr;
        size_t retOff = (pathSize + 7) & ~size_t(7);
        size_t softSize = ps2 - retOff;
        Buf c;
        std::memcpy(c.b, b.b + retOff, softSize);
        if (!Reflect::Get().Call(ksl, L"LoadAsset_Blocking", c.b, ps3, sizeof(c.b))) return nullptr;
        UObject* out = nullptr;
        std::memcpy(&out, c.b + (ps3 - 8), 8);
        return out;
    }

    // A Blueprint struct's field is stored as Name_<n>_<32 hex GUID>; authors write Name.
    std::wstring AuthoredName(const std::wstring& n)
    {
        if (n.size() < 36) return n;
        size_t g = n.size() - 32;
        if (n[g - 1] != L'_') return n;
        for (size_t i = g; i < n.size(); ++i)
            if (!iswxdigit(n[i])) return n;
        size_t u = n.rfind(L'_', g - 2);
        if (u == std::wstring::npos || u + 1 >= g - 1) return n;
        for (size_t i = u + 1; i < g - 1; ++i)
            if (!iswdigit(n[i])) return n;
        return n.substr(0, u);
    }

    template <typename F>
    void ForEachProp(UStruct* s, F&& fn)
    {
        for (UStruct* c = s; c; c = c->GetInheritanceSuper())
            for (const FField* f = c->GetChildProperties(); f; f = FieldWalk::Next(f))
                fn(const_cast<FProperty*>(static_cast<const FProperty*>(f)));
    }

    FProperty* FindProp(UStruct* s, const std::wstring& name)
    {
        FProperty* hit = nullptr;
        ForEachProp(s, [&](FProperty* p) {
            if (hit) return;
            std::wstring n = p->GetName();
            if (_wcsicmp(n.c_str(), name.c_str()) == 0 || _wcsicmp(AuthoredName(n).c_str(), name.c_str()) == 0) hit = p;
        });
        return hit;
    }

    // ImportText_Direct reports a parse error through an FOutputDevice. UE4SS exports no global one (GWarn/GLog), so
    // this stand-in has a vtable of no-ops (x64: the caller cleans up, so one empty function serves every slot); the
    // failure itself still shows as a null return.
    void* NopFn()
    {
        return nullptr;
    }
    struct FakeOutputDevice
    {
        void** vtbl;
        bool bSuppressEventTag = false;
        bool bAutoEmitLineTerminator = true;
        uint8_t pad[62]{};
    };
    void* g_nopVtbl[48];
    FakeOutputDevice g_errOut;

    __declspec(noinline) const wchar_t* SehImport(FProperty* p, const wchar_t* text, void* ptr, UObject* owner, bool* faulted)
    {
        __try
        {
            return p->ImportText_Direct(text, ptr, owner, 0, reinterpret_cast<RC::Unreal::FOutputDevice*>(&g_errOut));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *faulted = true;
            return nullptr;
        }
    }

    __declspec(noinline) bool SehExport(FProperty* p, FString* out, const void* ptr, UObject* owner)
    {
        __try
        {
            // Delta = the value itself: Unreal exports only when Data == Delta or the value differs from Delta, and a null
            // Delta compares against the type's default - first run, every empty array / false / null read "<unreadable>".
            return p->ExportText_Direct(*out, ptr, ptr, owner, 0, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::wstring Export(FProperty* p, const void* ptr, UObject* owner)
    {
        FString s;
        if (!SehExport(p, &s, ptr, owner)) return L"<unreadable>";
        return s.c_str();
    }

    std::wstring CppType(FProperty* p)
    {
        try
        {
            FString t = p->GetCPPType(nullptr, 0);
            return t.c_str();
        }
        catch (...)
        {
            return L"?";
        }
    }

    // ------------------------------------------------------------------ data tables
    struct Row
    {
        std::wstring name;
        uint8_t* data = nullptr;
    };

    // The engine's own list of row names (UDataTableFunctionLibrary::GetDataTableRowNames), as 8-byte FName values.
    bool RowNames(UObject* table, std::vector<uint64_t>& out)
    {
        static UObject* lib = nullptr;
        if (!lib) lib = FindObj(L"/Script/Engine.Default__DataTableFunctionLibrary");
        if (!lib) return false;
        Buf p;
        std::memcpy(p.b, &table, 8);
        if (!Reflect::Get().Call(lib, L"GetDataTableRowNames", p.b, 24, sizeof(p.b))) return false;
        uint64_t* data = nullptr;
        int32_t num = 0;
        std::memcpy(&data, p.b + 8, 8);
        std::memcpy(&num, p.b + 16, 4);
        if (num < 0 || num > (1 << 20) || (num && !data)) return false;
        out.assign(data, data + num);
        if (data) RC::Unreal::FMemory::Free(data);
        return true;
    }

    std::wstring NameText(uint64_t v)
    {
        FName n(L"None", RC::Unreal::FNAME_Find);
        std::memcpy(&n, &v, 8);
        try
        {
            return n.ToString();
        }
        catch (...)
        {
            return L"?";
        }
    }

    // UDataTable::RowMap (TMap<FName, uint8*>) sits right after the reflected RowStruct pointer. Its sparse-array layout is
    // read by hand, so it is checked against the engine's own row-name list before any row is touched.
    bool Rows(UObject* table, UStruct*& rowStruct, std::vector<Row>& out, std::string& why)
    {
        out.clear();
        auto** rs = Reflect::Get().Prop<UStruct*>(table, L"RowStruct");
        if (!rs || !*rs)
        {
            why = "no RowStruct";
            return false;
        }
        rowStruct = *rs;
        std::vector<uint64_t> names;
        if (!RowNames(table, names))
        {
            why = "GetDataTableRowNames failed";
            return false;
        }
        struct SparseHead
        {
            uint8_t* data;
            int32_t num, max;
            uint32_t inl[4];
            uint32_t* secondary;
            int32_t numBits, maxBits;
            int32_t firstFree, numFree;
        };
        auto* h = reinterpret_cast<SparseHead*>(reinterpret_cast<uint8_t*>(rs) + 8);
        if (h->num < 0 || h->num > (1 << 20) || h->num > h->max || (h->num && !h->data) || h->numFree < 0 || h->numFree > h->num)
        {
            why = "the row map does not look like a TMap (num " + std::to_string(h->num) + ")";
            return false;
        }
        const uint32_t* bits = h->secondary ? h->secondary : h->inl;
        std::set<uint64_t> want(names.begin(), names.end());
        for (int32_t i = 0; i < h->num; ++i)
        {
            if (h->numFree && !(bits[i >> 5] & (1u << (i & 31)))) continue;
            uint8_t* el = h->data + static_cast<size_t>(i) * 24; // TSetElement<TPair<FName, uint8*>>: key 8, value 8, hash 8
            uint64_t key = 0;
            std::memcpy(&key, el, 8);
            uint8_t* row = nullptr;
            std::memcpy(&row, el + 8, 8);
            if (!want.count(key) || !row)
            {
                why = "the row map does not match the engine's row names - layout differs; table left alone";
                out.clear();
                return false;
            }
            out.push_back({NameText(key), row});
        }
        if (out.size() != names.size())
        {
            why = "row count " + std::to_string(out.size()) + " vs the engine's " + std::to_string(names.size()) + "; table left alone";
            out.clear();
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------ apply
    enum class Kind
    {
        Table,
        Class,
        Object
    };

    Kind KindOf(UObject* obj)
    {
        std::wstring full = obj->GetFullName(nullptr);
        std::wstring cls = full.substr(0, full.find(L' '));
        if (cls == L"DataTable" || cls == L"CompositeDataTable") return Kind::Table;
        if (cls.size() >= 5 && cls.compare(cls.size() - 5, 5, L"Class") == 0) return Kind::Class;
        return Kind::Object;
    }

    bool IsInteger(const std::wstring& t)
    {
        return t.find(L"int") != std::wstring::npos;
    }

    // ------------------------------------------------------------------ dump
    void Dump(const std::wstring& path, const fs::path& outDir)
    {
        UObject* obj = LoadByPath(path.c_str());
        if (!obj)
        {
            NRN_WARN("np.dump: %s not found or not loadable", Narrow(path).c_str());
            return;
        }
        std::error_code ec;
        fs::create_directories(outDir / "dumps", ec);
        fs::path file = outDir / "dumps" / (ShortName(path) + L".txt");
        std::ofstream o(file, std::ios::binary);
        o << "# NorsePatcher dump of " << Narrow(obj->GetFullName(nullptr)) << "\n";
        auto field = [&](FProperty* p, uint8_t* base, UObject* owner, const char* indent) {
            std::wstring n = p->GetName();
            o << indent << Narrow(AuthoredName(n)) << " = " << Narrow(Export(p, base + p->GetOffset_Internal(), owner)) << "    ; "
              << Narrow(CppType(p)) << (AuthoredName(n) != n ? "  (stored as " + Narrow(n) + ")" : "") << "\n";
        };
        size_t count = 0;
        switch (KindOf(obj))
        {
        case Kind::Table:
        {
            UStruct* rs = nullptr;
            std::vector<Row> rows;
            std::string why;
            if (!Rows(obj, rs, rows, why))
            {
                o << "# rows unreadable: " << why << "\n";
                NRN_ERROR("np.dump %s: %s", Narrow(path).c_str(), why.c_str());
                return;
            }
            o << "# data table, row struct " << Narrow(rs->GetFullName(nullptr)) << ", " << rows.size() << " rows. Patch as <Row>.<Field> = value\n";
            for (const Row& r : rows)
            {
                o << "\n" << Narrow(r.name) << "\n";
                ForEachProp(rs, [&](FProperty* p) {
                    field(p, r.data, obj, "    ");
                    ++count;
                });
            }
            break;
        }
        case Kind::Class:
        {
            std::wstring pkg = path.substr(0, path.rfind(L'.'));
            UObject* cdo = FindObj((pkg + L".Default__" + ShortName(path)).c_str());
            if (!cdo)
            {
                o << "# class default object not found\n";
                break;
            }
            o << "# class default object " << Narrow(cdo->GetFullName(nullptr)) << "\n";
            ForEachProp(reinterpret_cast<UStruct*>(obj), [&](FProperty* p) {
                field(p, reinterpret_cast<uint8_t*>(cdo), cdo, "");
                ++count;
            });
            break;
        }
        case Kind::Object:
            ForEachProp(reinterpret_cast<UStruct*>(Reflect::Get().ClassOf(obj)), [&](FProperty* p) {
                field(p, reinterpret_cast<uint8_t*>(obj), obj, "");
                ++count;
            });
            break;
        }
        NRN_INFO("np.dump: %s -> %s (%zu fields)", Narrow(path).c_str(), Narrow(file.wstring()).c_str(), count);
    }

    // ------------------------------------------------------------------ nested lists (NorseLoot, 2026-11-17)
    // A path into an object: Field, Field.Sub, List[2], List[*] (every element). ResolveArrays returns every TArray the
    // path ends on: e.g. "Tiers[*].ItemPools[*].Items" on a TraderProfile -> each pool's item list. A step whose type is
    // not what the path needs ends that branch with a log line; nothing is written here.
    struct ArrayAt
    {
        FProperty* prop = nullptr; // the TArray property
        uint8_t* addr = nullptr;   // its value
        std::string label;         // the path with the indexes taken
    };

    bool IsStructType(const std::wstring& cpp)
    {
        return cpp.size() > 1 && cpp[0] == L'F' && cpp != L"FString" && cpp != L"FName" && cpp != L"FText";
    }

    void ResolveSteps(UStruct* s, uint8_t* base, const std::vector<std::wstring>& steps, size_t i, const std::string& label,
                      std::vector<ArrayAt>& out)
    {
        std::wstring step = steps[i], name = step, sel;
        size_t lb = step.find(L'[');
        if (lb != std::wstring::npos && step.back() == L']')
        {
            name = step.substr(0, lb);
            sel = step.substr(lb + 1, step.size() - lb - 2);
        }
        FProperty* p = FindProp(s, name);
        if (!p)
        {
            NRN_WARN("path: no field '%s' at %s", Narrow(name).c_str(), label.empty() ? "the object" : label.c_str());
            return;
        }
        uint8_t* addr = base + p->GetOffset_Internal();
        std::wstring cpp = CppType(p);
        std::string here = label + (label.empty() ? "" : ".") + Narrow(name);
        bool last = i + 1 == steps.size();
        if (cpp.rfind(L"TArray", 0) == 0)
        {
            if (last && sel.empty())
            {
                out.push_back({p, addr, here});
                return;
            }
            if (sel.empty())
            {
                NRN_WARN("path: %s is a list - write %s[*] or %s[n] to go inside it", here.c_str(), Narrow(name).c_str(), Narrow(name).c_str());
                return;
            }
            auto* inner = static_cast<RC::Unreal::FArrayProperty*>(p)->GetInner();
            if (!inner || !IsStructType(CppType(inner)))
            {
                NRN_WARN("path: %s holds no structs - it can only be the last step", here.c_str());
                return;
            }
            UStruct* es = static_cast<RC::Unreal::FStructProperty*>(inner)->GetStruct().Ptr;
            size_t esize = static_cast<size_t>(inner->GetSize());
            struct FScriptArray
            {
                uint8_t* data;
                int32_t num, max;
            };
            auto* arr = reinterpret_cast<FScriptArray*>(addr);
            if (!es || !esize || arr->num < 0 || arr->num > 100000 || (arr->num && !arr->data)) return;
            for (int32_t k = 0; k < arr->num; ++k)
            {
                if (sel != L"*" && _wtoi(sel.c_str()) != k) continue;
                if (last) continue; // List[n] as the last step names an element, not a list - not supported
                ResolveSteps(es, arr->data + static_cast<size_t>(k) * esize, steps, i + 1, here + "[" + std::to_string(k) + "]", out);
            }
            return;
        }
        if (last)
        {
            NRN_WARN("path: %s is a %s, not a list", here.c_str(), Narrow(cpp).c_str());
            return;
        }
        if (!IsStructType(cpp))
        {
            NRN_WARN("path: %s is a %s - a path can only go into structs and lists", here.c_str(), Narrow(cpp).c_str());
            return;
        }
        UStruct* ss = static_cast<RC::Unreal::FStructProperty*>(p)->GetStruct().Ptr;
        if (ss) ResolveSteps(ss, addr, steps, i + 1, here, out);
    }

    std::vector<ArrayAt> ResolveArrays(UObject* obj, const std::wstring& path)
    {
        std::vector<ArrayAt> out;
        auto* cls = reinterpret_cast<UStruct*>(Reflect::Get().ClassOf(obj));
        std::vector<std::wstring> steps;
        std::wstringstream ss(path);
        std::wstring part;
        while (std::getline(ss, part, L'.'))
            if (!Trim(part).empty()) steps.push_back(Trim(part));
        if (cls && !steps.empty()) ResolveSteps(cls, reinterpret_cast<uint8_t*>(obj), steps, 0, "", out);
        return out;
    }

    // Top-level elements of an exported list "(a,b,(c,d))" (moved here from NorseDistributor for NorseLoot).
    std::vector<std::wstring> ListElements(const std::wstring& arr)
    {
        std::vector<std::wstring> out;
        std::wstring s = Trim(arr);
        if (s.size() < 2 || s.front() != L'(' || s.back() != L')') return out;
        s = s.substr(1, s.size() - 2);
        int depth = 0;
        bool q = false;
        std::wstring cur;
        for (wchar_t c : s)
        {
            if (c == L'"') q = !q;
            if (!q && c == L'(') ++depth;
            if (!q && c == L')') --depth;
            if (!q && depth == 0 && c == L',')
            {
                out.push_back(Trim(cur));
                cur.clear();
                continue;
            }
            cur += c;
        }
        if (!Trim(cur).empty()) out.push_back(Trim(cur));
        return out;
    }
} // namespace
} // namespace norse

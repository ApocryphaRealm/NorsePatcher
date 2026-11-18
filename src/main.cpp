// NorsePatcher - runtime data patching for NORSE: Oath of Blood (UE4SS v4.0.0 C++ mod).
//
// SkyPatcher-like (and in the spirit of PalSchema for Palworld): plain-text patch files change data-table rows, data
// assets and class defaults at runtime. Nothing in the game's paks is edited, so removing a patch file restores the
// original values at the next start. Values are written by Unreal's own text import (FProperty::ImportText_Direct),
// so every property type takes the form Unreal itself uses: 12, 0.5, true, EnumValue, (X=1,Y=2), ("a","b"), a path.
//
// Patch files: <this mod>\patches\*.ini, read in file-name order; a later file wins a field it shares with an earlier
// one (and the log says so). Any mod can add its own file there.
//   [/Game/Norse/Core/Data/Tables/DT_Stats.DT_Stats]    a data table: <Row>.<Field> = value   (Row * = every row)
//   Strength.MaxValue = 30
//   [/Game/Some/DA_Thing.DA_Thing]                      any other object: <Field> = value
//   [/Game/Units/BP_Unit.BP_Unit_C]                     a class: its default object and every live instance
//   field *= 2      field += 5                          numeric fields: multiply / add to the current value
// Applied at every map load (InitGameState), and by the console commands np.reload (re-read the files, then apply),
// np.apply, np.dump <object path> (writes every field and value to dumps\<name>.txt - how field names are found).
#include <Windows.h>

#include "Log.hpp"
#include "Ue.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace nrn;
using namespace nrn::ue;
using RC::Unreal::FField;
using RC::Unreal::FieldWalk;
using RC::Unreal::FName;
using RC::Unreal::FProperty;
using RC::Unreal::FString;
using RC::Unreal::UStruct;

namespace
{
    // ------------------------------------------------------------------ config
    struct Config
    {
        int LogLevel = 2;
        bool ApplyOnMapLoad = true;
        std::wstring LogDir; // empty = the mod folder
        std::vector<std::wstring> Dumps;
    } g_cfg;

    fs::path g_mod, g_out;

    fs::path ModDir()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModDir), &self);
        wchar_t buf[MAX_PATH * 2]{};
        GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
        return fs::path(buf).parent_path().parent_path(); // ...\Mods\NorsePatcher\dlls\main.dll -> mod folder
    }

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

    void LoadConfig(const fs::path& ini)
    {
        g_cfg = Config{};
        for (const auto& raw : ReadLines(ini))
        {
            std::wstring l = Trim(StripComment(raw));
            if (l.empty() || l[0] == L'[') continue;
            size_t eq = l.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring k = Trim(l.substr(0, eq)), v = Trim(l.substr(eq + 1));
            if (k == L"uLogLevel") g_cfg.LogLevel = _wtoi(v.c_str());
            else if (k == L"bApplyOnMapLoad") g_cfg.ApplyOnMapLoad = _wtoi(v.c_str()) != 0;
            else if (k == L"sLogDir") g_cfg.LogDir = v;
            else if (k == L"Dump" && !v.empty()) g_cfg.Dumps.push_back(v);
        }
    }

    // ------------------------------------------------------------------ patch files
    struct Edit
    {
        std::wstring key;   // as written: Field, or Row.Field for a data table
        wchar_t op = L'=';  // '=', '*', '+'
        std::wstring value; // Unreal text form
        std::string where;  // file:line
    };
    struct Target
    {
        std::wstring path;
        std::vector<Edit> edits;
    };
    std::vector<Target> g_targets;

    void LoadPatches()
    {
        g_targets.clear();
        fs::path dir = g_mod / "patches";
        std::vector<fs::path> files;
        std::error_code ec;
        if (fs::is_directory(dir, ec))
            for (const auto& f : fs::directory_iterator(dir, ec))
                if (f.is_regular_file() && Lower(f.path().extension().wstring()) == L".ini") files.push_back(f.path());
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return Lower(a.filename().wstring()) < Lower(b.filename().wstring());
        });
        std::map<std::wstring, std::string> owner; // path|key -> the file that set it first
        size_t edits = 0;
        for (const auto& f : files)
        {
            Target* cur = nullptr;
            int n = 0;
            for (const auto& raw : ReadLines(f))
            {
                ++n;
                std::wstring l = Trim(StripComment(raw));
                if (l.empty()) continue;
                std::string where = Narrow(f.filename().wstring()) + ":" + std::to_string(n);
                if (l.front() == L'[' && l.back() == L']')
                {
                    g_targets.push_back({Trim(l.substr(1, l.size() - 2)), {}});
                    cur = &g_targets.back();
                    continue;
                }
                size_t eq = l.find(L'=');
                if (!cur || eq == std::wstring::npos || eq == 0)
                {
                    NRN_WARN("%s: not a [object path] or field = value line - skipped: %s", where.c_str(), Narrow(l).c_str());
                    continue;
                }
                Edit e;
                e.where = where;
                size_t keyEnd = eq;
                if (l[eq - 1] == L'*' || l[eq - 1] == L'+')
                {
                    e.op = l[eq - 1];
                    keyEnd = eq - 1;
                }
                e.key = Trim(l.substr(0, keyEnd));
                e.value = Trim(l.substr(eq + 1));
                if (e.key.empty())
                {
                    NRN_WARN("%s: no field name - skipped", where.c_str());
                    continue;
                }
                std::wstring k = Lower(cur->path + L"|" + e.key);
                if (auto it = owner.find(k); it == owner.end()) owner[k] = where;
                else if (it->second.substr(0, it->second.find(':')) != where.substr(0, where.find(':')))
                    NRN_INFO("%s %s: %s comes later and wins over %s", Narrow(cur->path).c_str(), Narrow(e.key).c_str(), where.c_str(),
                             it->second.c_str());
                cur->edits.push_back(std::move(e));
                ++edits;
            }
        }
        NRN_INFO("patches: %zu file(s) in %s, %zu object section(s), %zu edit(s)", files.size(), Narrow(dir.wstring()).c_str(),
                 g_targets.size(), edits);
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

    struct Stats
    {
        int ok = 0, failed = 0;
    };

    void ApplyOne(FProperty* p, uint8_t* ptr, UObject* owner, const Edit& e, const std::string& label, Stats& st)
    {
        std::wstring before = Export(p, ptr, owner);
        std::wstring text = e.value;
        if (e.op != L'=')
        {
            wchar_t* end = nullptr;
            double cur = wcstod(before.c_str(), &end), arg = wcstod(e.value.c_str(), nullptr);
            if (!end || *end)
            {
                NRN_WARN("%s: '%c=' needs a number, the field holds '%s' - skipped (%s)", label.c_str(), static_cast<char>(e.op),
                         Narrow(before).c_str(), e.where.c_str());
                ++st.failed;
                return;
            }
            double v = e.op == L'*' ? cur * arg : cur + arg;
            wchar_t buf[64];
            if (IsInteger(CppType(p))) swprintf(buf, 64, L"%lld", static_cast<long long>(v < 0 ? v - 0.5 : v + 0.5));
            else swprintf(buf, 64, L"%.9g", v);
            text = buf;
        }
        bool faulted = false;
        const wchar_t* r = SehImport(p, text.c_str(), ptr, owner, &faulted);
        std::wstring after = Export(p, ptr, owner);
        if (!r || faulted)
        {
            NRN_WARN("%s: Unreal did not accept '%s'%s (%s) - value now %s", label.c_str(), Narrow(text).c_str(),
                     faulted ? " (fault caught)" : "", e.where.c_str(), Narrow(after).c_str());
            ++st.failed;
            return;
        }
        NRN_INFO("%s: %s -> %s (%s)", label.c_str(), Narrow(before).c_str(), Narrow(after).c_str(), e.where.c_str());
        ++st.ok;
    }

    std::wstring ShortName(const std::wstring& path)
    {
        size_t d = path.find_last_of(L"./");
        return d == std::wstring::npos ? path : path.substr(d + 1);
    }

    void ApplyTarget(const Target& t, Stats& st)
    {
        UObject* obj = LoadByPath(t.path.c_str());
        if (!obj)
        {
            NRN_WARN("[%s] not found or not loadable - its %zu edit(s) skipped", Narrow(t.path).c_str(), t.edits.size());
            st.failed += static_cast<int>(t.edits.size());
            return;
        }
        std::string sn = Narrow(ShortName(t.path));
        switch (KindOf(obj))
        {
        case Kind::Table:
        {
            UStruct* rs = nullptr;
            std::vector<Row> rows;
            std::string why;
            if (!Rows(obj, rs, rows, why))
            {
                NRN_ERROR("[%s] data table rows unreadable: %s", Narrow(t.path).c_str(), why.c_str());
                st.failed += static_cast<int>(t.edits.size());
                return;
            }
            for (const Edit& e : t.edits)
            {
                size_t dot = e.key.rfind(L'.');
                if (dot == std::wstring::npos)
                {
                    NRN_WARN("%s: a data table field is written Row.Field (Row * = every row): '%s' (%s)", sn.c_str(), Narrow(e.key).c_str(),
                             e.where.c_str());
                    ++st.failed;
                    continue;
                }
                std::wstring row = e.key.substr(0, dot), field = e.key.substr(dot + 1);
                FProperty* p = FindProp(rs, field);
                if (!p)
                {
                    NRN_WARN("%s: the row struct has no field '%s' (np.dump %s lists them) (%s)", sn.c_str(), Narrow(field).c_str(),
                             Narrow(t.path).c_str(), e.where.c_str());
                    ++st.failed;
                    continue;
                }
                int hits = 0;
                for (const Row& r : rows)
                    if (row == L"*" || _wcsicmp(r.name.c_str(), row.c_str()) == 0)
                    {
                        ++hits;
                        ApplyOne(p, r.data + p->GetOffset_Internal(), obj, e, sn + " " + Narrow(r.name) + "." + Narrow(field), st);
                    }
                if (!hits)
                {
                    NRN_WARN("%s: no row '%s' (%zu rows) (%s)", sn.c_str(), Narrow(row).c_str(), rows.size(), e.where.c_str());
                    ++st.failed;
                }
            }
            break;
        }
        case Kind::Class:
        {
            auto* cls = reinterpret_cast<UStruct*>(obj);
            std::wstring pkg = t.path.substr(0, t.path.rfind(L'.'));
            std::wstring name = ShortName(t.path);
            UObject* cdo = FindObj((pkg + L".Default__" + name).c_str());
            std::vector<UObject*> live;
            try
            {
                RC::Unreal::UObjectGlobals::FindAllOf(name.c_str(), live);
            }
            catch (...)
            {
            }
            live.erase(std::remove(live.begin(), live.end(), cdo), live.end());
            for (const Edit& e : t.edits)
            {
                FProperty* p = FindProp(cls, e.key);
                if (!p)
                {
                    NRN_WARN("%s: the class has no field '%s' (%s)", sn.c_str(), Narrow(e.key).c_str(), e.where.c_str());
                    ++st.failed;
                    continue;
                }
                if (cdo) ApplyOne(p, reinterpret_cast<uint8_t*>(cdo) + p->GetOffset_Internal(), cdo, e, sn + " default." + Narrow(e.key), st);
                for (UObject* o : live)
                    ApplyOne(p, reinterpret_cast<uint8_t*>(o) + p->GetOffset_Internal(), o, e, Narrow(ShortName(o->GetFullName(nullptr))) + "." + Narrow(e.key), st);
            }
            NRN_INFO("%s: class default %s, %zu live instance(s)", sn.c_str(), cdo ? "patched" : "NOT FOUND", live.size());
            break;
        }
        case Kind::Object:
        {
            auto* cls = reinterpret_cast<UStruct*>(Reflect::Get().ClassOf(obj));
            for (const Edit& e : t.edits)
            {
                FProperty* p = cls ? FindProp(cls, e.key) : nullptr;
                if (!p)
                {
                    NRN_WARN("%s: no field '%s' (%s)", sn.c_str(), Narrow(e.key).c_str(), e.where.c_str());
                    ++st.failed;
                    continue;
                }
                ApplyOne(p, reinterpret_cast<uint8_t*>(obj) + p->GetOffset_Internal(), obj, e, sn + "." + Narrow(e.key), st);
            }
            break;
        }
        }
    }

    void ApplyAll(const char* why)
    {
        Stats st;
        for (const Target& t : g_targets)
        {
            try
            {
                ApplyTarget(t, st);
            }
            catch (...)
            {
                NRN_ERROR("[%s] threw - skipped", Narrow(t.path).c_str());
            }
        }
        NRN_INFO("applied (%s): %d value(s) set, %d not", why, st.ok, st.failed);
    }

    // ------------------------------------------------------------------ dump
    void Dump(const std::wstring& path)
    {
        UObject* obj = LoadByPath(path.c_str());
        if (!obj)
        {
            NRN_WARN("np.dump: %s not found or not loadable", Narrow(path).c_str());
            return;
        }
        std::error_code ec;
        fs::create_directories(g_out / "dumps", ec);
        fs::path file = g_out / "dumps" / (ShortName(path) + L".txt");
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

    // ------------------------------------------------------------------ commands
    bool RunCommand(const std::wstring& c)
    {
        std::wstring cmd = Trim(c), arg;
        if (size_t sp = cmd.find(L' '); sp != std::wstring::npos)
        {
            arg = Trim(cmd.substr(sp + 1));
            cmd = cmd.substr(0, sp);
        }
        cmd = Lower(cmd);
        NRN_INFO("command: %s %s", Narrow(cmd).c_str(), Narrow(arg).c_str());
        if (cmd == L"np.reload")
        {
            LoadPatches();
            ApplyAll("np.reload");
        }
        else if (cmd == L"np.apply") ApplyAll("np.apply");
        else if (cmd == L"np.dump" && !arg.empty()) Dump(arg);
        else NRN_WARN("unknown command - np.reload | np.apply | np.dump <object path>");
        return true;
    }
} // namespace

class NorsePatcher : public RC::CppUserModBase
{
  public:
    NorsePatcher()
    {
        ModName = L"NorsePatcher";
        ModVersion = L"" NP_VERSION;
        ModDescription = L"Runtime data patching: patch files change data-table rows, data assets and class defaults without touching the paks.";
        ModAuthors = L"ApocryphaRealm";

        for (auto& s : g_nopVtbl) s = reinterpret_cast<void*>(&NopFn);
        g_errOut.vtbl = g_nopVtbl;

        g_mod = ModDir();
        LoadConfig(g_mod / "NorsePatcher.ini");
        g_out = g_cfg.LogDir.empty() ? g_mod : fs::path(g_cfg.LogDir);
        Log::Get().Open(g_out / "NorsePatcher.log", g_cfg.LogLevel);
        Log::Get().Write(Level::Error, "NorsePatcher v%s - log level %d (2 = info: every value set is logged)", NP_VERSION, g_cfg.LogLevel);
        LoadPatches();
    }

    // 0.1.0 first run: callbacks registered in the constructor were dropped silently - UE4SS's hooks exist only from
    // on_unreal_init (NorseRealtimeNative registers there too).
    auto on_unreal_init() -> void override
    {
        NRN_INFO("on_unreal_init: registering InitGameState and console callbacks");
        try
        {
            RC::Unreal::Hook::RegisterInitGameStatePostCallback([](RC::Unreal::AGameModeBase*) {
                try
                {
                    for (const auto& d : g_cfg.Dumps) Dump(d);
                    if (g_cfg.ApplyOnMapLoad) ApplyAll("map load");
                }
                catch (...)
                {
                    NRN_ERROR("map-load apply threw");
                }
            });
            RC::Unreal::Hook::RegisterProcessConsoleExecCallback(
                    [](UObject*, const wchar_t* cmd, RC::Unreal::FOutputDevice&, UObject*) -> bool {
                        if (!cmd || _wcsnicmp(cmd, L"np.", 3) != 0) return false;
                        // the same command reaches ProcessConsoleExec on several objects: run it once
                        static ULONGLONG last = 0;
                        static std::wstring lastCmd;
                        ULONGLONG now = GetTickCount64();
                        if (lastCmd == cmd && now - last < 250) return true;
                        last = now;
                        lastCmd = cmd;
                        try
                        {
                            return RunCommand(cmd);
                        }
                        catch (...)
                        {
                            NRN_ERROR("command threw");
                            return true;
                        }
                    });
            NRN_INFO("callbacks registered (apply at map load: %d, %zu dump(s) configured)", g_cfg.ApplyOnMapLoad, g_cfg.Dumps.size());
        }
        catch (const std::exception& e)
        {
            NRN_ERROR("callback registration failed: %s", e.what());
        }
    }
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new NorsePatcher();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}

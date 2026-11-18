#pragma once
// NorseFilters.hpp - the shared rule filters of the Norse frameworks (2026-11-17). Moved out of NorseDistributor 0.1.0
// unchanged, plus two additions for NorseTagger: a class filter (the object's class or any parent class) and Tag:<name>
// in the field filters, answered by NorseTagger through its exports (NT_HasTag). Include after NorseReflect.hpp; like it,
// included by one source file per mod.
//
// One rule's filters, every part optional:   names | folders | field filters | chance
//   names    the object's name; * wildcards; -name excludes; comma-separated
//   folders  text in the object's path; -text excludes
//   fields   Field:text - that field's value (as Unreal writes it) contains text; Tag:Name - NorseTagger gave it that
//            tag; a leading - excludes
//   chance   0-100, a fixed roll per object and rule (the same every start)
#include <functional>

namespace norse
{
namespace
{
    std::vector<std::wstring> SplitList(const std::wstring& s, wchar_t sep)
    {
        std::vector<std::wstring> out;
        std::wstring cur;
        for (wchar_t c : s)
            if (c == sep)
            {
                out.push_back(Trim(cur));
                cur.clear();
            }
            else cur += c;
        out.push_back(Trim(cur));
        return out;
    }

    struct Filter
    {
        std::vector<std::wstring> inc, exc; // lower-case
        bool Empty() const { return inc.empty() && exc.empty(); }
    };
    struct FieldFilter
    {
        std::wstring field, text; // text lower-case (a Tag: filter keeps the tag name as written)
        bool exclude = false;
        bool tag = false;
    };
    struct Filters
    {
        Filter classes, names, folders;
        std::vector<FieldFilter> fields;
        int chance = 100;
        bool UsesTags() const
        {
            return std::any_of(fields.begin(), fields.end(), [](const FieldFilter& f) { return f.tag; });
        }
    };

    Filter ParseFilter(const std::wstring& s)
    {
        Filter f;
        for (auto& t : SplitList(s, L','))
        {
            if (t.empty() || _wcsicmp(t.c_str(), L"NONE") == 0) continue;
            if (t[0] == L'-') f.exc.push_back(Lower(Trim(t.substr(1))));
            else f.inc.push_back(Lower(t));
        }
        return f;
    }

    std::vector<FieldFilter> ParseFields(const std::wstring& s, const std::string& where)
    {
        std::vector<FieldFilter> out;
        for (auto& t : SplitList(s, L','))
        {
            if (t.empty() || _wcsicmp(t.c_str(), L"NONE") == 0) continue;
            FieldFilter ff;
            if (t[0] == L'-')
            {
                ff.exclude = true;
                t = Trim(t.substr(1));
            }
            size_t c = t.find(L':');
            if (c == std::wstring::npos)
            {
                NRN_WARN("%s: field filter '%s' is not Field:text or Tag:Name - ignored", where.c_str(), Narrow(t).c_str());
                continue;
            }
            ff.field = Trim(t.substr(0, c));
            ff.tag = _wcsicmp(ff.field.c_str(), L"Tag") == 0;
            ff.text = ff.tag ? Trim(t.substr(c + 1)) : Lower(Trim(t.substr(c + 1)));
            out.push_back(ff);
        }
        return out;
    }

    int ParseChance(const std::wstring& s)
    {
        return (s.empty() || _wcsicmp(s.c_str(), L"NONE") == 0) ? 100 : std::clamp(_wtoi(s.c_str()), 0, 100);
    }

    bool Glob(const wchar_t* p, const wchar_t* s)
    {
        if (!*p) return !*s;
        if (*p == L'*') return Glob(p + 1, s) || (*s && Glob(p, s + 1));
        return *s && towlower(*p) == towlower(*s) && Glob(p + 1, s + 1);
    }

    uint32_t Fnv(const std::wstring& s)
    {
        uint32_t h = 2166136261u;
        for (wchar_t c : s)
        {
            h ^= static_cast<uint32_t>(towlower(c));
            h *= 16777619u;
        }
        return h;
    }

    // ---- NorseTagger, found beside this mod (...\ue4ss\Mods\NorseTagger\dlls\main.dll)
    using NT_HasTagFn = int (*)(void* obj, const char* tag);
    using NT_ApplyFn = void (*)();
    struct TaggerApi
    {
        NT_HasTagFn HasTag = nullptr;
        NT_ApplyFn Apply = nullptr;
    };
    const TaggerApi* Tagger()
    {
        static TaggerApi t;
        if (t.HasTag) return &t;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Tagger), &self);
        wchar_t buf[MAX_PATH * 2]{};
        GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
        fs::path dll = fs::path(buf).parent_path().parent_path().parent_path() / L"NorseTagger" / L"dlls" / L"main.dll";
        if (HMODULE m = GetModuleHandleW(dll.c_str()))
        {
            t.HasTag = reinterpret_cast<NT_HasTagFn>(GetProcAddress(m, "NT_HasTag"));
            t.Apply = reinterpret_cast<NT_ApplyFn>(GetProcAddress(m, "NT_Apply"));
        }
        if (!t.HasTag || !t.Apply) t = {};
        return t.HasTag ? &t : nullptr;
    }

    // The class and its parents, as Unreal names them without the U/A prefix: ItemAsset_HeavyAxe, ItemAsset_Weapon_TwoHanded, ...
    std::vector<std::wstring> ClassChain(UStruct* cls)
    {
        std::vector<std::wstring> out;
        for (UStruct* c = cls; c; c = c->GetInheritanceSuper())
        {
            std::wstring full = c->GetFullName(nullptr);
            out.push_back(Lower(ShortName(full.substr(full.find(L' ') + 1))));
        }
        return out;
    }

    // Why an object does not pass, or empty when it does. rollKey makes the chance roll per object and rule.
    std::string Rejects(const Filters& f, UObject* obj, UStruct* cls, const std::wstring& name, const std::wstring& path,
                        const std::wstring& rollKey)
    {
        if (!f.classes.Empty())
        {
            auto chain = ClassChain(cls);
            auto in = [&](const std::wstring& c) { return std::find(chain.begin(), chain.end(), c) != chain.end(); };
            if (!f.classes.inc.empty() && std::none_of(f.classes.inc.begin(), f.classes.inc.end(), in)) return "class";
            if (std::any_of(f.classes.exc.begin(), f.classes.exc.end(), in)) return "class excluded";
        }
        if (!f.names.inc.empty() && std::none_of(f.names.inc.begin(), f.names.inc.end(), [&](auto& p) { return Glob(p.c_str(), name.c_str()); }))
            return "name";
        if (std::any_of(f.names.exc.begin(), f.names.exc.end(), [&](auto& p) { return Glob(p.c_str(), name.c_str()); })) return "name excluded";
        std::wstring lp = Lower(path);
        if (!f.folders.inc.empty() && std::none_of(f.folders.inc.begin(), f.folders.inc.end(), [&](auto& t) { return lp.find(t) != std::wstring::npos; }))
            return "folder";
        if (std::any_of(f.folders.exc.begin(), f.folders.exc.end(), [&](auto& t) { return lp.find(t) != std::wstring::npos; }))
            return "folder excluded";
        bool anyInc = false, incHit = false;
        for (const auto& ff : f.fields)
        {
            bool has = false;
            if (ff.tag)
            {
                const TaggerApi* t = Tagger();
                has = t && t->HasTag(obj, Narrow(ff.text).c_str()) != 0;
            }
            else
            {
                FProperty* p = FindProp(cls, ff.field);
                std::wstring v = p ? Lower(Export(p, reinterpret_cast<uint8_t*>(obj) + p->GetOffset_Internal(), obj)) : L"";
                has = v.find(ff.text) != std::wstring::npos;
            }
            if (ff.exclude && has) return ff.tag ? "tag excluded" : "field excluded";
            if (!ff.exclude)
            {
                anyInc = true;
                incHit |= has;
            }
        }
        if (anyInc && !incHit) return "field";
        if (f.chance < 100 && static_cast<int>(Fnv(rollKey) % 100) >= f.chance) return "chance";
        return {};
    }
} // namespace
} // namespace norse

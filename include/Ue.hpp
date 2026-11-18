#pragma once
// Ue.hpp - reflection-only access to the game: UFunction calls through ProcessEvent with checked parameter sizes,
// and property pointers by name. No raw native addresses (NorseAddressLibrary is not needed for v0.1).
#include "Log.hpp"
#include "ue4ss_abi.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace nrn::ue
{
    using RC::Unreal::UClass;
    using RC::Unreal::UFunction;
    using RC::Unreal::UObject;

    struct FVec
    {
        double X{}, Y{}, Z{};
    };
    struct FRot
    {
        double Pitch{}, Yaw{}, Roll{};
    };

    inline std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // SEH guard around the one call that executes game code. No C++ objects with destructors live in this frame,
    // so __try is legal here. On a fault we log, and the caller disables the feature rather than crash the game.
    __declspec(noinline) inline bool GuardedProcessEvent(UObject* obj, UFunction* fn, void* parms)
    {
        __try
        {
            obj->ProcessEvent(fn, parms);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Warn once per (function name) so a per-frame miss does not flood the log (rule 14).
    inline void WarnOnce(const std::string& key, const char* fmt, const char* a, int b = 0, int c = 0)
    {
        static std::set<std::string> seen;
        if (seen.insert(key).second) Log::Get().Write(Level::Warn, fmt, a, b, c);
    }

    class Reflect
    {
      public:
        static Reflect& Get()
        {
            static Reflect s;
            return s;
        }

        void ClearCaches()
        {
            m_fn.clear();
        }

        UClass* ClassOf(UObject* obj)
        {
            if (!obj) return nullptr;
            try
            {
                return obj->GetClassPrivate();
            }
            catch (...)
            {
                return nullptr;
            }
        }

        UFunction* Find(UObject* obj, const wchar_t* name)
        {
            if (!obj || !name) return nullptr;
            UClass* cls = ClassOf(obj);
            auto key = std::make_pair(cls, std::wstring(name));
            if (auto it = m_fn.find(key); it != m_fn.end()) return it->second;
            UFunction* fn = nullptr;
            try
            {
                fn = obj->GetFunctionByNameInChain(name);
            }
            catch (...)
            {
                fn = nullptr;
            }
            m_fn[key] = fn;
            if (!fn)
                WarnOnce("nofn:" + Narrow(name), "UFunction '%s' not found on the object's class chain - that call is skipped", Narrow(name).c_str());
            else
                NRN_DEBUG("resolved UFunction %s -> %p", Narrow(name).c_str(), static_cast<void*>(fn));
            return fn;
        }

        // Calls obj.name(parms). minParms is the byte size of the parameter block as laid out by UHT for UE 5.6 (the end
        // of the last parameter); the reflected ParmsSize must be within [minParms, minParms + 8] or the call is refused,
        // which catches a wrong hand-written layout before it can write past the buffer.
        bool Call(UObject* obj, const wchar_t* name, void* parms, uint16_t minParms, uint16_t bufSize)
        {
            if (!obj)
            {
                WarnOnce("nullobj:" + Narrow(name), "call %s on a null object - skipped", Narrow(name).c_str());
                return false;
            }
            UFunction* fn = Find(obj, name);
            if (!fn) return false;
            uint16_t ps = 0;
            try
            {
                ps = fn->GetParmsSize();
            }
            catch (...)
            {
                return false;
            }
            if (ps < minParms || ps > minParms + 8 || ps > bufSize)
            {
                WarnOnce("parms:" + Narrow(name), "UFunction %s ParmsSize %d does not match the expected layout (%d) - call refused",
                         Narrow(name).c_str(), ps, minParms);
                return false;
            }
            if (!GuardedProcessEvent(obj, fn, parms))
            {
                NRN_ERROR("ProcessEvent faulted calling %s on %p", Narrow(name).c_str(), static_cast<void*>(obj));
                return false;
            }
            return true;
        }

        // Reflected parameter-block size of obj.name, 0 when the function is missing.
        uint16_t ParmsSizeOf(UObject* obj, const wchar_t* name)
        {
            UFunction* fn = Find(obj, name);
            if (!fn) return 0;
            try
            {
                return fn->GetParmsSize();
            }
            catch (...)
            {
                return 0;
            }
        }

        template <typename T>
        T* Prop(UObject* obj, const wchar_t* name)
        {
            if (!obj) return nullptr;
            void* p = nullptr;
            try
            {
                p = obj->GetValuePtrByPropertyNameInChain(name);
            }
            catch (...)
            {
                p = nullptr;
            }
            if (!p) WarnOnce("noprop:" + Narrow(name), "property '%s' not found - its read/write is skipped", Narrow(name).c_str());
            return static_cast<T*>(p);
        }

      private:
        std::map<std::pair<UClass*, std::wstring>, UFunction*> m_fn;
    };

    // ---- typed wrappers (UE 5.6 parameter layouts; LWC doubles) ----
    // A UObject pointer kept across frames can outlive its object (destroyed, then freed by the garbage collector).
    // Keep the object's InternalIndex (UObjectBase +0xC) beside it and ask the global object array before each use:
    // the same object still in that slot, not marked for deletion, not unreachable. (NorseHUD 0.1.1, 2026-11-17)
    inline int32_t IndexOf(UObject* o)
    {
        int32_t i = -1;
        if (o) std::memcpy(&i, reinterpret_cast<uint8_t*>(o) + 0xC, sizeof(i));
        return i;
    }
    inline bool Alive(UObject* o, int32_t index)
    {
        if (!o || index < 0) return false;
        RC::Unreal::FUObjectItem* it = nullptr;
        try
        {
            it = RC::Unreal::FUObjectArray::IndexToObject(index);
        }
        catch (...)
        {
            return false;
        }
        if (!it) return false;
        UObject* cur = nullptr;
        std::memcpy(&cur, it, sizeof(cur)); // FUObjectItem::Object is its first member
        return cur == o && !it->IsPendingKill() && !it->IsUnreachable();
    }

    struct Buf
    {
        alignas(16) uint8_t b[256]{};
    };

    inline UObject* GetObjectRet(UObject* obj, const wchar_t* fn)
    {
        Buf p;
        if (!Reflect::Get().Call(obj, fn, p.b, 8, sizeof(p.b))) return nullptr;
        UObject* r = nullptr;
        std::memcpy(&r, p.b, 8);
        return r;
    }

    inline bool GetVec(UObject* obj, const wchar_t* fn, FVec& out)
    {
        Buf p;
        if (!Reflect::Get().Call(obj, fn, p.b, 24, sizeof(p.b))) return false;
        std::memcpy(&out, p.b, 24);
        return true;
    }

    inline bool GetRot(UObject* obj, const wchar_t* fn, FRot& out)
    {
        Buf p;
        if (!Reflect::Get().Call(obj, fn, p.b, 24, sizeof(p.b))) return false;
        std::memcpy(&out, p.b, 24);
        return true;
    }

    inline bool GetBool(UObject* obj, const wchar_t* fn, bool& out)
    {
        Buf p;
        if (!Reflect::Get().Call(obj, fn, p.b, 1, sizeof(p.b))) return false;
        out = p.b[0] != 0;
        return true;
    }

    inline bool GetByte(UObject* obj, const wchar_t* fn, uint8_t& out)
    {
        Buf p;
        if (!Reflect::Get().Call(obj, fn, p.b, 1, sizeof(p.b))) return false;
        out = p.b[0];
        return true;
    }

    inline bool GetFloat(UObject* obj, const wchar_t* fn, float& out)
    {
        Buf p;
        if (!Reflect::Get().Call(obj, fn, p.b, 4, sizeof(p.b))) return false;
        std::memcpy(&out, p.b, 4);
        return true;
    }

    inline bool CallObjArg(UObject* obj, const wchar_t* fn, UObject* arg)
    {
        Buf p;
        std::memcpy(p.b, &arg, 8);
        return Reflect::Get().Call(obj, fn, p.b, 8, sizeof(p.b));
    }

    inline bool CallNoArgs(UObject* obj, const wchar_t* fn)
    {
        Buf p;
        return Reflect::Get().Call(obj, fn, p.b, 0, sizeof(p.b));
    }

    // APawn::AddMovementInput(FVector WorldDirection, float ScaleValue, bool bForce)
    inline bool AddMovementInput(UObject* pawn, const FVec& dir, float scale, bool force)
    {
        Buf p;
        std::memcpy(p.b, &dir, 24);
        std::memcpy(p.b + 24, &scale, 4);
        p.b[28] = force ? 1 : 0;
        return Reflect::Get().Call(pawn, L"AddMovementInput", p.b, 29, sizeof(p.b));
    }

    // AActor::K2_SetActorRotation(FRotator NewRotation, bool bTeleportPhysics) -> bool
    inline bool SetActorRotation(UObject* actor, const FRot& r)
    {
        Buf p;
        std::memcpy(p.b, &r, 24);
        p.b[24] = 0;
        return Reflect::Get().Call(actor, L"K2_SetActorRotation", p.b, 26, sizeof(p.b));
    }

    // AController::SetControlRotation(const FRotator&)
    inline bool SetControlRotation(UObject* ctrl, const FRot& r)
    {
        Buf p;
        std::memcpy(p.b, &r, 24);
        return Reflect::Get().Call(ctrl, L"SetControlRotation", p.b, 24, sizeof(p.b));
    }

    // APlayerController::SetViewTargetWithBlend(AActor*, float BlendTime, EViewTargetBlendFunction, float BlendExp, bool bLockOutgoing)
    inline bool SetViewTargetWithBlend(UObject* pc, UObject* target, float blendTime)
    {
        Buf p;
        std::memcpy(p.b, &target, 8);
        std::memcpy(p.b + 8, &blendTime, 4);
        p.b[12] = 0; // VTBlend_Linear
        float exp = 0.f;
        std::memcpy(p.b + 16, &exp, 4);
        p.b[20] = 0;
        return Reflect::Get().Call(pc, L"SetViewTargetWithBlend", p.b, 21, sizeof(p.b));
    }

    // UTeamIDLibrary::GetTeamIDSafe(const UObject*) -> uint8 (static: called on the class default object)
    inline bool GetTeamIDSafe(UObject* lib, UObject* obj, uint8_t& out)
    {
        Buf p;
        std::memcpy(p.b, &obj, 8);
        if (!Reflect::Get().Call(lib, L"GetTeamIDSafe", p.b, 9, sizeof(p.b))) return false;
        out = p.b[8];
        return true;
    }

    // UNavigationSystemV1::K2_ProjectPointToNavigation(UObject* WorldContextObject, const FVector& Point,
    //     FVector& ProjectedLocation, ANavigationData* NavData, TSubclassOf<UNavigationQueryFilter> FilterClass,
    //     const FVector QueryExtent) -> bool   (static: called on Default__NavigationSystemV1)
    // Layout: ctx 0, Point 8, Projected 32, NavData 56, FilterClass 64, QueryExtent 72, ReturnValue 96 (end 97).
    // Returns false when the call itself could not be made; onNav carries the answer.
    inline bool ProjectPointToNavigation(UObject* navLib, UObject* worldCtx, const FVec& point, const FVec& extent, bool& onNav)
    {
        Buf p;
        std::memcpy(p.b, &worldCtx, 8);
        std::memcpy(p.b + 8, &point, 24);
        std::memcpy(p.b + 72, &extent, 24);
        if (!Reflect::Get().Call(navLib, L"K2_ProjectPointToNavigation", p.b, 97, sizeof(p.b))) return false;
        onNav = p.b[96] != 0;
        return true;
    }

    // ---- follow camera (0.2.0) ----
    // FTransform (UE 5.6, doubles): FQuat Rotation @0 (x,y,z,w), FVector Translation @32 (+pad), FVector Scale3D @64
    // (+pad); size 96, alignment 16.
    inline void WriteTransform(uint8_t* at, const FVec& loc)
    {
        double t[12] = {0, 0, 0, 1, loc.X, loc.Y, loc.Z, 0, 1, 1, 1, 0};
        std::memcpy(at, t, sizeof(t));
    }

    // UGameplayStatics::BeginDeferredActorSpawnFromClass(const UObject* WorldContext, TSubclassOf<AActor> ActorClass,
    //     const FTransform& SpawnTransform, ESpawnActorCollisionHandlingMethod, AActor* Owner, ESpawnActorScaleMethod)
    //     -> AActor*.  Layout: ctx 0, class 8, transform 16..112, collision 112, owner 120, scale 128, return 136 (end 144).
    // Then UGameplayStatics::FinishSpawningActor(AActor*, const FTransform&, ESpawnActorScaleMethod) -> AActor*.
    //     Layout: actor 0, transform 16..112, scale 112, return 120 (end 128).
    // Without the ESpawnActorScaleMethod parameter (older engines) the blocks are 136 / 120 with the return at 128 / 112;
    // the reflected ParmsSize picks the layout.
    inline UObject* SpawnActorOfClass(UObject* gameplayStatics, UObject* worldCtx, UObject* cls, const FVec& loc)
    {
        Buf p;
        std::memcpy(p.b, &worldCtx, 8);
        std::memcpy(p.b + 8, &cls, 8);
        WriteTransform(p.b + 16, loc);
        p.b[112] = 1; // AlwaysSpawn
        uint16_t bps = Reflect::Get().ParmsSizeOf(gameplayStatics, L"BeginDeferredActorSpawnFromClass");
        bool withScale = bps >= 144;
        if (withScale) p.b[128] = 1; // MultiplyWithRoot (the default)
        if (!Reflect::Get().Call(gameplayStatics, L"BeginDeferredActorSpawnFromClass", p.b, withScale ? 144 : 136, sizeof(p.b))) return nullptr;
        UObject* actor = nullptr;
        std::memcpy(&actor, p.b + (withScale ? 136 : 128), 8);
        if (!actor) return nullptr;
        Buf f;
        std::memcpy(f.b, &actor, 8);
        WriteTransform(f.b + 16, loc);
        uint16_t fps = Reflect::Get().ParmsSizeOf(gameplayStatics, L"FinishSpawningActor");
        bool fScale = fps >= 128;
        if (fScale) f.b[112] = 1;
        if (!Reflect::Get().Call(gameplayStatics, L"FinishSpawningActor", f.b, fScale ? 128 : 120, sizeof(f.b))) return nullptr;
        UObject* done = nullptr;
        std::memcpy(&done, f.b + (fScale ? 120 : 112), 8);
        return done ? done : actor;
    }

    // AActor::K2_TeleportTo(FVector DestLocation, FRotator DestRotation) -> bool. Used to move the camera actor: unlike
    // K2_SetActorLocationAndRotation it has no FHitResult in its parameters, so the layout is certain.
    inline bool TeleportTo(UObject* actor, const FVec& loc, const FRot& rot)
    {
        Buf p;
        std::memcpy(p.b, &loc, 24);
        std::memcpy(p.b + 24, &rot, 24);
        return Reflect::Get().Call(actor, L"K2_TeleportTo", p.b, 49, sizeof(p.b));
    }

    // UKismetSystemLibrary::SphereTraceSingle(WorldContext, Start, End, Radius, ETraceTypeQuery, bTraceComplex,
    //     const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace, FHitResult& OutHit, bool bIgnoreSelf,
    //     FLinearColor TraceColor, FLinearColor TraceHitColor, float DrawTime) -> bool
    // Layout up to OutHit is fixed: ctx 0, Start 8, End 32, Radius 56, channel 60, complex 61, ignore 64..80, debug 80,
    // OutHit 88. FHitResult's size S is not hand-copied: it is ParmsSize - 136 (bIgnoreSelf 88+S, colours 92+S..124+S,
    // DrawTime 124+S, ReturnValue 128+S, end 129+S rounded to 8). FHitResult starts FaceIndex, Time, Distance.
    struct BigBuf
    {
        alignas(16) uint8_t b[1024]{};
    };

    // Returns false when the call could not be made. hit / time (0..1 along Start->End) carry the answer.
    inline bool SphereTrace(UObject* kslib, UObject* worldCtx, const FVec& start, const FVec& end, float radius, uint8_t channel,
                            bool& hit, float& time)
    {
        uint16_t ps = Reflect::Get().ParmsSizeOf(kslib, L"SphereTraceSingle");
        if (ps < 136 + 128 || ps > 136 + 640)
        {
            WarnOnce("parms:SphereTraceSingle", "UFunction %s ParmsSize %d is outside the expected range - call refused", "SphereTraceSingle", ps);
            return false;
        }
        const size_t S = static_cast<size_t>(ps) - 136;
        BigBuf p;
        std::memcpy(p.b, &worldCtx, 8);
        std::memcpy(p.b + 8, &start, 24);
        std::memcpy(p.b + 32, &end, 24);
        std::memcpy(p.b + 56, &radius, 4);
        p.b[60] = channel;
        p.b[61] = 0;     // bTraceComplex
        p.b[80] = 0;     // EDrawDebugTrace::None
        p.b[88 + S] = 1; // bIgnoreSelf: the hero is the world context, so it is ignored
        if (!Reflect::Get().Call(kslib, L"SphereTraceSingle", p.b, ps, sizeof(p.b))) return false;
        hit = p.b[128 + S] != 0;
        std::memcpy(&time, p.b + 88 + 4, 4);
        return true;
    }

    inline std::string Name(UObject* obj)
    {
        if (!obj) return "null";
        try
        {
            return Narrow(obj->GetFullName(nullptr));
        }
        catch (...)
        {
            return "?";
        }
    }
} // namespace nrn::ue

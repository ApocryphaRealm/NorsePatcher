#pragma once
// Log.hpp - the mod's own log file (rule 14: levels, a self-describing header, transitions-only for per-frame code).
#include <Windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace nrn
{
    // 0 trace, 1 debug, 2 info, 3 warn, 4 error. Shipped default: 2 (info) - the INI and Config::LogLevel agree.
    enum class Level : int
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4
    };

    class Log
    {
      public:
        static Log& Get()
        {
            static Log s;
            return s;
        }

        void Open(const std::filesystem::path& file, int level)
        {
            std::lock_guard lk(m_mx);
            m_level = level;
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            m_out.open(file, std::ios::out | std::ios::trunc);
            m_path = file;
        }

        void SetLevel(int level) { m_level = level; }
        int GetLevel() const { return m_level; }
        const std::filesystem::path& Path() const { return m_path; }

        void Write(Level lv, const char* fmt, ...)
        {
            if (static_cast<int>(lv) < m_level) return;
            char buf[2048];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            SYSTEMTIME st;
            GetLocalTime(&st);
            static const char* names[] = {"trace", "debug", "info", "warn", "error"};
            char line[2200];
            snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     names[static_cast<int>(lv)], buf);
            std::lock_guard lk(m_mx);
            if (m_out.is_open())
            {
                m_out << line;
                m_out.flush();
            }
            OutputDebugStringA(line);
        }

      private:
        std::mutex m_mx;
        std::ofstream m_out;
        std::filesystem::path m_path;
        int m_level{2};
    };
} // namespace nrn

#define NRN_TRACE(...) ::nrn::Log::Get().Write(::nrn::Level::Trace, __VA_ARGS__)
#define NRN_DEBUG(...) ::nrn::Log::Get().Write(::nrn::Level::Debug, __VA_ARGS__)
#define NRN_INFO(...) ::nrn::Log::Get().Write(::nrn::Level::Info, __VA_ARGS__)
#define NRN_WARN(...) ::nrn::Log::Get().Write(::nrn::Level::Warn, __VA_ARGS__)
#define NRN_ERROR(...) ::nrn::Log::Get().Write(::nrn::Level::Error, __VA_ARGS__)

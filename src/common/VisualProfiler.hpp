#pragma once

#ifndef VISUAL_PROFILER_HPP
#define VISUAL_PROFILER_HPP

#include <memory>
#include <fcntl.h>

#ifdef PROFILING
#include <perfetto.h>

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("default"),
    perfetto::Category("fs"),
    perfetto::Category("nvme"),
    perfetto::Category("ftl")
);
#endif

#ifndef PROFILING
#define PROFILER_INIT()

#define PROFILE_SCOPE(category, name)
#define PROFILE_FUNCTION_CATEGORY(category)
#define PROFILE_FUNCTION()
#define PROFILER_FLUSH()
#else
#define PROFILER_FLUSH() perfetto::TrackEvent::Flush()
#define PROFILE_SCOPE(category, name) TRACE_EVENT(category, perfetto::DynamicString{name})
#define PROFILE_FUNCTION_CATEGORY(category) const char *ProfileFunction_##__LINE__ = __FUNCTION__; PROFILE_SCOPE(category, ProfileFunction_##__LINE__)
#define PROFILE_FUNCTION() PROFILE_FUNCTION_CATEGORY("default");

#define PROFILER_INIT() \
perfetto::TracingInitArgs args; \
args.backends |= perfetto::kInProcessBackend; \
perfetto::Tracing::Initialize(args); \
perfetto::TrackEvent::Register();
#endif

class VisualProfiler {
public:
    static VisualProfiler& Instance() {
        static VisualProfiler instance;
        return instance;
    }

    ~VisualProfiler() {
        endSession();
    }

    void beginSession(const std::string& filepath = "results.data");
    void endSession();

private:
    bool m_activeSession = false;
    int m_fd;
#ifdef PROFILING
    std::unique_ptr<perfetto::TracingSession> m_tracingSession;
#endif
    VisualProfiler() {}
};

#endif

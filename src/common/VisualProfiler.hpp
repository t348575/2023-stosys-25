#pragma once

#ifndef VISUAL_PROFILER_HPP
#define VISUAL_PROFILER_HPP

#include <thread>
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
#define PROFILE_SCOPE(category, name) TRACE_EVENT(category, name)
#define PROFILE_FUNCTION_CATEGORY(category) const char *ProfileFunction_##__LINE__ = __FUNCTION__; PROFILE_SCOPE(category, ProfileFunction_##__LINE__)
#define PROFILE_FUNCTION() PROFILE_FUNCTION_CATEGORY("default");

#define PROFILER_INIT() \
perfetto::TracingInitArgs args; \
args.backends |= perfetto::kInProcessBackend; \
perfetto::Tracing::Initialize(args); \
perfetto::TrackEvent::Register();

#define PROFILER_FLUSH() perfetto::TrackEvent::Flush()
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

    void beginSession(const std::string& filepath = "results.data") {
        if (m_activeSession) {
            endSession();
        }

        m_activeSession = true;
#ifdef PROFILING
        perfetto::protos::gen::TrackEventConfig track_event_cfg;
        track_event_cfg.add_enabled_categories("*");

        perfetto::TraceConfig cfg;
        cfg.add_buffers()->set_size_kb(1024 * 1024);
        auto* ds_cfg = cfg.add_data_sources()->mutable_config();
        ds_cfg->set_name("track_event");
        ds_cfg->set_track_event_config_raw(track_event_cfg.SerializeAsString());

        m_tracingSession = perfetto::Tracing::NewTrace();

        m_fd = open(filepath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0664);
        m_tracingSession->Setup(cfg, m_fd);
        m_tracingSession->StartBlocking();
#endif
    }

    void endSession() {
#ifdef PROFILING
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m_tracingSession->StopBlocking();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        close(m_fd);
#endif
    }

private:
    bool m_activeSession = false;
    int m_fd;
#ifdef PROFILING
    std::unique_ptr<perfetto::TracingSession> m_tracingSession;
#endif
    VisualProfiler() {}
};

#endif

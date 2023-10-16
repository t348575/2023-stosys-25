#include "VisualProfiler.hpp"

#include <thread>

#ifdef PROFILING
PERFETTO_TRACK_EVENT_STATIC_STORAGE();
#endif

void VisualProfiler::beginSession(const std::string& filepath) {
    if (m_activeSession) {
        endSession();
    }

    m_activeSession = true;
#ifdef PROFILING
    perfetto::protos::gen::TrackEventConfig track_event_cfg;
    track_event_cfg.add_enabled_categories("*");

    perfetto::TraceConfig cfg;
    cfg.add_buffers()->set_size_kb(1024 * 128);
    cfg.set_file_write_period_ms(100);
    cfg.set_max_file_size_bytes(1024 * 1024 * 1024);
    auto* ds_cfg = cfg.add_data_sources()->mutable_config();
    ds_cfg->set_name("track_event");
    ds_cfg->set_track_event_config_raw(track_event_cfg.SerializeAsString());

    m_tracingSession = perfetto::Tracing::NewTrace();

    m_fd = open(filepath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0664);
    m_tracingSession->Setup(cfg, m_fd);
    m_tracingSession->StartBlocking();
#endif
}

void VisualProfiler::endSession() {
#ifdef PROFILING
        PROFILER_FLUSH();
        m_tracingSession->StopBlocking();
        close(m_fd);
#endif
}
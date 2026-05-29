// Copyright (c) 2013-2025 The SRS Authors
// SPDX-License-Identifier: MIT

#ifndef SRS_APP_DYNAMIC_INGEST_HPP
#define SRS_APP_DYNAMIC_INGEST_HPP

#include <srs_core.hpp>

#ifdef SRS_FFMPEG_STUB

#include <string>
#include <vector>
#include <map>

#include <srs_app_st.hpp>

class SrsFFMPEG;

// A single dynamic ingest rule: pull src_url, push to dst_url via ffmpeg.
struct SrsDynamicIngestRule {
    std::string id;          // Random hex ID
    std::string src_url;     // e.g. rtmp://10.20.40.10/live/cctv1hd
    std::string dst_url;     // e.g. rtmp://rtmp-qukan.cztv.com/live/xxx
    std::string created_at;  // ISO8601 UTC
};

// Persists rules to disk and manages their lifecycle.
class SrsDynamicIngestRegistry
{
public:
    SrsDynamicIngestRegistry();
    virtual ~SrsDynamicIngestRegistry();
public:
    srs_error_t load(const std::string& path);
    srs_error_t save();
public:
    srs_error_t add(SrsDynamicIngestRule& rule);
    srs_error_t remove_by_id(const std::string& id);
public:
    std::vector<SrsDynamicIngestRule> query_all();
private:
    std::string generate_id();
private:
    std::string file_path_;
    std::vector<SrsDynamicIngestRule> rules_;
};

// Runs one ffmpeg process for a single ingest rule.
class SrsDynamicIngestWorker : public ISrsCoroutineHandler
{
public:
    SrsDynamicIngestWorker(const SrsDynamicIngestRule& rule, const std::string& ffmpeg_bin);
    virtual ~SrsDynamicIngestWorker();
public:
    srs_error_t start();
    void stop();
    const std::string& id() const { return rule_.id; }
    bool is_exited() const { return exited_; }
// Interface ISrsCoroutineHandler.
public:
    virtual srs_error_t cycle();
private:
    SrsDynamicIngestRule rule_;
    std::string ffmpeg_bin_;
    SrsCoroutine* trd_;
    SrsFFMPEG* ffmpeg_;
    bool stopped_;
    bool exited_;
};

// Manages all running ingest workers, reconciles with registry on demand.
class SrsDynamicIngestManager
{
public:
    SrsDynamicIngestManager();
    virtual ~SrsDynamicIngestManager();
public:
    srs_error_t start_worker(const SrsDynamicIngestRule& rule);
    void stop_worker(const std::string& id);
    bool is_running(const std::string& id);
private:
    std::string ffmpeg_bin_;
    std::map<std::string, SrsDynamicIngestWorker*> workers_;
};

extern SrsDynamicIngestRegistry* _srs_dynamic_ingest;
extern SrsDynamicIngestManager*  _srs_dynamic_ingest_mgr;

#endif // SRS_FFMPEG_STUB

#endif

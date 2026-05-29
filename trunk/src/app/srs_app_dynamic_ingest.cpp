// Copyright (c) 2013-2025 The SRS Authors
// SPDX-License-Identifier: MIT

#include <srs_app_dynamic_ingest.hpp>

#ifdef SRS_FFMPEG_STUB

#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>
#include <srs_protocol_json.hpp>
#include <srs_core_autofree.hpp>
#include <srs_app_ffmpeg.hpp>
#include <srs_app_st.hpp>
#include <srs_core_time.hpp>

#include <fstream>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#include <unistd.h>

SrsDynamicIngestRegistry* _srs_dynamic_ingest     = NULL;
SrsDynamicIngestManager*  _srs_dynamic_ingest_mgr = NULL;

// ---- SrsDynamicIngestRegistry ----

SrsDynamicIngestRegistry::SrsDynamicIngestRegistry()
{
}

SrsDynamicIngestRegistry::~SrsDynamicIngestRegistry()
{
}

std::string SrsDynamicIngestRegistry::generate_id()
{
    // Seed once with time+pid for uniqueness across restarts.
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL) ^ ((unsigned int)getpid() << 16));
        seeded = true;
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%08x%08x", (unsigned int)rand(), (unsigned int)rand());
    return std::string(buf);
}

srs_error_t SrsDynamicIngestRegistry::load(const std::string& path)
{
    srs_error_t err = srs_success;
    file_path_ = path;

    std::ifstream f(path.c_str());
    if (!f.is_open()) {
        srs_trace("dynamic ingest: no persistence file at %s, starting empty", path.c_str());
        return err;
    }

    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    if (content.empty()) return err;

    SrsJsonAny* any = SrsJsonAny::loads(content);
    if (!any) return srs_error_new(ERROR_JSON_LOADS, "dynamic_ingest.json parse failed");
    SrsUniquePtr<SrsJsonAny> auto_free(any);

    if (!any->is_object()) return srs_error_new(ERROR_JSON_LOADS, "dynamic_ingest.json root must be object");

    SrsJsonObject* obj = any->to_object();
    SrsJsonAny* rules_any = obj->ensure_property_array("rules");
    if (!rules_any) return err;

    SrsJsonArray* arr = rules_any->to_array();
    for (int i = 0; i < arr->count(); i++) {
        SrsJsonAny* item = arr->at(i);
        if (!item || !item->is_object()) continue;
        SrsJsonObject* o = item->to_object();

        SrsDynamicIngestRule rule;
        SrsJsonAny* p;
        if ((p = o->ensure_property_string("id")))         rule.id         = p->to_str();
        if ((p = o->ensure_property_string("src_url")))    rule.src_url    = p->to_str();
        if ((p = o->ensure_property_string("dst_url")))    rule.dst_url    = p->to_str();
        if ((p = o->ensure_property_string("created_at"))) rule.created_at = p->to_str();

        if (rule.src_url.empty() || rule.dst_url.empty()) continue;
        rules_.push_back(rule);
    }

    srs_trace("dynamic ingest: loaded %d rules from %s", (int)rules_.size(), path.c_str());
    return err;
}

srs_error_t SrsDynamicIngestRegistry::save()
{
    srs_error_t err = srs_success;
    if (file_path_.empty()) return err;

    SrsJsonObject* root = SrsJsonAny::object();
    SrsUniquePtr<SrsJsonObject> auto_free(root);

    SrsJsonArray* arr = SrsJsonAny::array();
    root->set("rules", arr);

    for (size_t i = 0; i < rules_.size(); i++) {
        SrsDynamicIngestRule& r = rules_[i];
        SrsJsonObject* item = SrsJsonAny::object();
        item->set("id",         SrsJsonAny::str(r.id.c_str()));
        item->set("src_url",    SrsJsonAny::str(r.src_url.c_str()));
        item->set("dst_url",    SrsJsonAny::str(r.dst_url.c_str()));
        item->set("created_at", SrsJsonAny::str(r.created_at.c_str()));
        arr->add(item);
    }

    std::string content = root->dumps();
    std::string tmp_path = file_path_ + ".tmp";
    std::ofstream f(tmp_path.c_str());
    if (!f.is_open()) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "open %s for write failed", tmp_path.c_str());
    }
    f << content;
    f.close();
    if (!f.good()) {
        return srs_error_new(ERROR_SYSTEM_FILE_WRITE, "write %s failed", tmp_path.c_str());
    }
    if (rename(tmp_path.c_str(), file_path_.c_str()) != 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_WRITE, "rename %s to %s failed", tmp_path.c_str(), file_path_.c_str());
    }
    return err;
}

srs_error_t SrsDynamicIngestRegistry::add(SrsDynamicIngestRule& rule)
{
    rule.id = generate_id();

    time_t now = time(NULL);
    char buf[32];
    struct tm tm_buf;
    struct tm* tm_info = gmtime_r(&now, &tm_buf);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    rule.created_at = buf;

    rules_.push_back(rule);
    return save();
}

srs_error_t SrsDynamicIngestRegistry::remove_by_id(const std::string& id)
{
    for (std::vector<SrsDynamicIngestRule>::iterator it = rules_.begin(); it != rules_.end(); ++it) {
        if (it->id == id) {
            SrsDynamicIngestRule backup = *it;
            rules_.erase(it);
            srs_error_t err = save();
            if (err != srs_success) {
                rules_.push_back(backup); // rollback on save failure
            }
            return err;
        }
    }
    return srs_error_new(ERROR_RTMP_STREAM_NOT_FOUND, "ingest rule id=%s not found", id.c_str());
}

std::vector<SrsDynamicIngestRule> SrsDynamicIngestRegistry::query_all()
{
    return rules_;
}

// ---- SrsDynamicIngestWorker ----

SrsDynamicIngestWorker::SrsDynamicIngestWorker(const SrsDynamicIngestRule& rule, const std::string& ffmpeg_bin)
    : rule_(rule), ffmpeg_bin_(ffmpeg_bin), trd_(NULL), ffmpeg_(NULL), stopped_(false), exited_(false)
{
}

SrsDynamicIngestWorker::~SrsDynamicIngestWorker()
{
    stop();
    srs_freep(trd_);
    srs_freep(ffmpeg_);
}

srs_error_t SrsDynamicIngestWorker::start()
{
    stopped_ = false;
    trd_ = new SrsSTCoroutine("dingest-" + rule_.id, this);
    return trd_->start();
}

void SrsDynamicIngestWorker::stop()
{
    stopped_ = true;
    if (ffmpeg_) {
        ffmpeg_->fast_stop();
        ffmpeg_->stop();
    }
    if (trd_) trd_->stop();
}

srs_error_t SrsDynamicIngestWorker::cycle()
{
    srs_error_t err = srs_success;

    // Initialize ffmpeg argv once; start()/cycle() reuse it on restarts.
    ffmpeg_ = new SrsFFMPEG(ffmpeg_bin_);
    ffmpeg_->append_iparam("-re");
    ffmpeg_->append_iparam("-rw_timeout");
    ffmpeg_->append_iparam("15000000"); // 15s read timeout (microseconds)

    if ((err = ffmpeg_->initialize(rule_.src_url, rule_.dst_url, SRS_CONSTS_NULL_FILE)) != srs_success) {
        exited_ = true;
        return srs_error_wrap(err, "dingest init");
    }
    ffmpeg_->set_oformat("flv");
    if ((err = ffmpeg_->initialize_copy()) != srs_success) {
        exited_ = true;
        return srs_error_wrap(err, "dingest init_copy");
    }

    srs_trace("dingest %s: initialized, src=%s dst=%s", rule_.id.c_str(),
        rule_.src_url.c_str(), rule_.dst_url.c_str());

    while (!stopped_) {
        if ((err = trd_->pull()) != srs_success) {
            ffmpeg_->fast_stop();
            exited_ = true;
            return srs_error_wrap(err, "dingest worker interrupted");
        }

        // start() is idempotent: no-op if process is running, restarts if it exited.
        if ((err = ffmpeg_->start()) != srs_success) {
            srs_warn("dingest %s: start failed: %s", rule_.id.c_str(), srs_error_desc(err).c_str());
            srs_freep(err);
            srs_usleep(3 * SRS_UTIME_SECONDS);
            continue;
        }

        // cycle() uses WNOHANG to reap the process if it has exited;
        // only errors on waitpid syscall failure (very rare).
        if ((err = ffmpeg_->cycle()) != srs_success) {
            srs_warn("dingest %s: cycle error: %s", rule_.id.c_str(), srs_error_desc(err).c_str());
            srs_freep(err);
        }

        srs_usleep(500 * SRS_UTIME_MILLISECONDS);
    }

    exited_ = true;
    return err;
}

// ---- SrsDynamicIngestManager ----

SrsDynamicIngestManager::SrsDynamicIngestManager()
{
    // Prefer SRS bundled ffmpeg, then fall back to system ffmpeg.
    const char* candidates[] = {
        "/usr/local/srs/objs/ffmpeg/bin/ffmpeg",
        "/usr/local/bin/ffmpeg",
        "/usr/bin/ffmpeg",
        NULL
    };
    for (int i = 0; candidates[i] != NULL; i++) {
        if (access(candidates[i], X_OK) == 0) {
            ffmpeg_bin_ = candidates[i];
            break;
        }
    }
}

SrsDynamicIngestManager::~SrsDynamicIngestManager()
{
    std::map<std::string, SrsDynamicIngestWorker*>::iterator it;
    for (it = workers_.begin(); it != workers_.end(); ++it) {
        SrsDynamicIngestWorker* w = it->second;
        w->stop();
        srs_freep(w);
    }
    workers_.clear();
}

srs_error_t SrsDynamicIngestManager::start_worker(const SrsDynamicIngestRule& rule)
{
    srs_error_t err = srs_success;

    if (ffmpeg_bin_.empty()) {
        return srs_error_new(ERROR_SYSTEM_FILE_NOT_EXISTS, "no ffmpeg binary found, ingest unavailable");
    }

    // Reap stale worker if the coroutine has already exited naturally.
    std::map<std::string, SrsDynamicIngestWorker*>::iterator it = workers_.find(rule.id);
    if (it != workers_.end()) {
        if (it->second->is_exited()) {
            srs_freep(it->second);
            workers_.erase(it);
        } else {
            return err; // still running
        }
    }

    SrsDynamicIngestWorker* w = new SrsDynamicIngestWorker(rule, ffmpeg_bin_);
    if ((err = w->start()) != srs_success) {
        srs_freep(w);
        return srs_error_wrap(err, "start dingest worker");
    }

    workers_[rule.id] = w;
    return err;
}

void SrsDynamicIngestManager::stop_worker(const std::string& id)
{
    std::map<std::string, SrsDynamicIngestWorker*>::iterator it = workers_.find(id);
    if (it == workers_.end()) return;

    SrsDynamicIngestWorker* w = it->second;
    w->stop();
    srs_freep(w);
    workers_.erase(it);
}

bool SrsDynamicIngestManager::is_running(const std::string& id)
{
    std::map<std::string, SrsDynamicIngestWorker*>::iterator it = workers_.find(id);
    if (it == workers_.end()) return false;
    if (it->second->is_exited()) {
        srs_freep(it->second);
        workers_.erase(it);
        return false;
    }
    return true;
}

#endif // SRS_FFMPEG_STUB

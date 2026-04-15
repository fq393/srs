// Copyright (c) 2013-2025 The SRS Authors
// SPDX-License-Identifier: MIT

#include <srs_app_dynamic_forward.hpp>

#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>
#include <srs_protocol_json.hpp>
#include <srs_core_autofree.hpp>

#include <fstream>
#include <sstream>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

SrsDynamicForwardRegistry* _srs_dynamic_forward = NULL;

SrsDynamicForwardRegistry::SrsDynamicForwardRegistry()
{
}

SrsDynamicForwardRegistry::~SrsDynamicForwardRegistry()
{
}

std::string SrsDynamicForwardRegistry::make_key(const std::string& vhost,
    const std::string& app, const std::string& stream)
{
    return vhost + "/" + app + "/" + stream;
}

std::string SrsDynamicForwardRegistry::generate_id()
{
    char buf[17];
    snprintf(buf, sizeof(buf), "%08x%08x", (unsigned int)rand(), (unsigned int)rand());
    return std::string(buf);
}

srs_error_t SrsDynamicForwardRegistry::load(const std::string& path)
{
    srs_error_t err = srs_success;
    file_path_ = path;

    std::ifstream f(path.c_str());
    if (!f.is_open()) {
        // File doesn't exist yet — normal on first run.
        srs_trace("dynamic forward: no persistence file at %s, starting empty", path.c_str());
        return err;
    }

    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    if (content.empty()) {
        return err;
    }

    SrsJsonAny* any = SrsJsonAny::loads(content);
    if (!any) {
        return srs_error_new(ERROR_JSON_LOADS, "dynamic_forward.json parse failed");
    }
    SrsUniquePtr<SrsJsonAny> auto_free(any);

    if (!any->is_object()) {
        return srs_error_new(ERROR_JSON_LOADS, "dynamic_forward.json root must be object");
    }

    SrsJsonObject* obj = any->to_object();
    SrsJsonAny* rules_any = obj->ensure_property_array("rules");
    if (!rules_any) {
        return err; // empty rules key is fine
    }

    SrsJsonArray* arr = rules_any->to_array();
    for (int i = 0; i < arr->count(); i++) {
        SrsJsonAny* item = arr->at(i);
        if (!item || !item->is_object()) continue;
        SrsJsonObject* o = item->to_object();

        SrsDynamicForwardRule rule;
        SrsJsonAny* p;
        if ((p = o->ensure_property_string("id")))         rule.id         = p->to_str();
        if ((p = o->ensure_property_string("vhost")))      rule.vhost      = p->to_str();
        if ((p = o->ensure_property_string("app")))        rule.app        = p->to_str();
        if ((p = o->ensure_property_string("stream")))     rule.stream     = p->to_str();
        if ((p = o->ensure_property_string("ep")))         rule.ep         = p->to_str();
        if ((p = o->ensure_property_string("created_at"))) rule.created_at = p->to_str();

        if (rule.vhost.empty() || rule.ep.empty()) continue;

        rules_[make_key(rule.vhost, rule.app, rule.stream)].push_back(rule);
    }

    srs_trace("dynamic forward: loaded %d rule groups from %s",
        (int)rules_.size(), path.c_str());
    return err;
}

srs_error_t SrsDynamicForwardRegistry::save()
{
    srs_error_t err = srs_success;
    if (file_path_.empty()) return err;

    SrsJsonObject* root = SrsJsonAny::object();
    SrsUniquePtr<SrsJsonObject> auto_free(root);

    SrsJsonArray* arr = SrsJsonAny::array();
    root->set("rules", arr);

    std::map<std::string, std::vector<SrsDynamicForwardRule> >::iterator kit;
    for (kit = rules_.begin(); kit != rules_.end(); ++kit) {
        std::vector<SrsDynamicForwardRule>& vec = kit->second;
        for (size_t i = 0; i < vec.size(); i++) {
            SrsDynamicForwardRule& r = vec[i];
            SrsJsonObject* item = SrsJsonAny::object();
            item->set("id",         SrsJsonAny::str(r.id.c_str()));
            item->set("vhost",      SrsJsonAny::str(r.vhost.c_str()));
            item->set("app",        SrsJsonAny::str(r.app.c_str()));
            item->set("stream",     SrsJsonAny::str(r.stream.c_str()));
            item->set("ep",         SrsJsonAny::str(r.ep.c_str()));
            item->set("created_at", SrsJsonAny::str(r.created_at.c_str()));
            arr->add(item);
        }
    }

    std::string content = root->dumps();

    std::ofstream f(file_path_.c_str());
    if (!f.is_open()) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "open %s for write failed",
            file_path_.c_str());
    }
    f << content;
    if (!f.good()) {
        return srs_error_new(ERROR_SYSTEM_FILE_WRITE, "write %s failed", file_path_.c_str());
    }

    return err;
}

srs_error_t SrsDynamicForwardRegistry::add(SrsDynamicForwardRule& rule)
{
    rule.id = generate_id();

    time_t now = time(NULL);
    char buf[32];
    struct tm* tm_info = gmtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    rule.created_at = buf;

    rules_[make_key(rule.vhost, rule.app, rule.stream)].push_back(rule);

    return save();
}

srs_error_t SrsDynamicForwardRegistry::remove_by_id(const std::string& id)
{
    std::map<std::string, std::vector<SrsDynamicForwardRule> >::iterator kit;
    for (kit = rules_.begin(); kit != rules_.end(); ++kit) {
        std::vector<SrsDynamicForwardRule>& vec = kit->second;
        for (std::vector<SrsDynamicForwardRule>::iterator it = vec.begin(); it != vec.end(); ++it) {
            if (it->id == id) {
                vec.erase(it);
                return save();
            }
        }
    }

    return srs_error_new(ERROR_RTMP_STREAM_NOT_FOUND, "rule id=%s not found", id.c_str());
}

srs_error_t SrsDynamicForwardRegistry::remove_by_ep(const std::string& vhost,
    const std::string& app, const std::string& stream, const std::string& ep)
{
    std::string key = make_key(vhost, app, stream);
    std::map<std::string, std::vector<SrsDynamicForwardRule> >::iterator kit = rules_.find(key);
    if (kit == rules_.end()) {
        return srs_error_new(ERROR_RTMP_STREAM_NOT_FOUND, "stream %s not found", key.c_str());
    }

    std::vector<SrsDynamicForwardRule>& vec = kit->second;
    for (std::vector<SrsDynamicForwardRule>::iterator it = vec.begin(); it != vec.end(); ++it) {
        if (it->ep == ep) {
            vec.erase(it);
            return save();
        }
    }

    return srs_error_new(ERROR_RTMP_STREAM_NOT_FOUND, "ep=%s not found in %s",
        ep.c_str(), key.c_str());
}

std::vector<SrsDynamicForwardRule> SrsDynamicForwardRegistry::query_all()
{
    std::vector<SrsDynamicForwardRule> result;
    std::map<std::string, std::vector<SrsDynamicForwardRule> >::iterator kit;
    for (kit = rules_.begin(); kit != rules_.end(); ++kit) {
        std::vector<SrsDynamicForwardRule>& vec = kit->second;
        for (size_t i = 0; i < vec.size(); i++) {
            result.push_back(vec[i]);
        }
    }
    return result;
}

std::vector<SrsDynamicForwardRule> SrsDynamicForwardRegistry::query_stream(
    const std::string& vhost, const std::string& app, const std::string& stream)
{
    std::string key = make_key(vhost, app, stream);
    std::map<std::string, std::vector<SrsDynamicForwardRule> >::iterator kit = rules_.find(key);
    if (kit == rules_.end()) {
        return std::vector<SrsDynamicForwardRule>();
    }
    return kit->second;
}

// Copyright (c) 2013-2025 The SRS Authors
// SPDX-License-Identifier: MIT

#ifndef SRS_APP_DYNAMIC_FORWARD_HPP
#define SRS_APP_DYNAMIC_FORWARD_HPP

#include <srs_core.hpp>

#include <string>
#include <vector>
#include <map>

// A single dynamic forward rule, persisted to disk.
struct SrsDynamicForwardRule {
    std::string id;          // Random hex ID for deletion
    std::string vhost;
    std::string app;
    std::string stream;
    std::string ep;          // e.g. "rtmp://192.168.1.100/live/stream"
    std::string created_at;  // ISO8601 UTC
};

// Global registry: in-memory map + JSON file persistence.
// Rules survive SRS restarts. Does NOT touch srs.conf.
class SrsDynamicForwardRegistry
{
public:
    SrsDynamicForwardRegistry();
    virtual ~SrsDynamicForwardRegistry();
public:
    // Load rules from JSON file on startup (missing file = empty, not error).
    srs_error_t load(const std::string& path);
    // Persist current rules to disk.
    srs_error_t save();
public:
    // Add a rule; rule.id and rule.created_at are filled in.
    srs_error_t add(SrsDynamicForwardRule& rule);
    // Remove by ID (precise).
    srs_error_t remove_by_id(const std::string& id);
    // Remove by vhost/app/stream + ep.
    srs_error_t remove_by_ep(const std::string& vhost, const std::string& app,
        const std::string& stream, const std::string& ep);
public:
    // Return all rules across all streams.
    std::vector<SrsDynamicForwardRule> query_all();
    // Return rules for a specific stream.
    std::vector<SrsDynamicForwardRule> query_stream(const std::string& vhost,
        const std::string& app, const std::string& stream);
private:
    std::string make_key(const std::string& vhost, const std::string& app,
        const std::string& stream);
    std::string generate_id();
private:
    std::string file_path_;
    // key: "vhost/app/stream"
    std::map<std::string, std::vector<SrsDynamicForwardRule> > rules_;
};

// Global singleton instance.
extern SrsDynamicForwardRegistry* _srs_dynamic_forward;

#endif

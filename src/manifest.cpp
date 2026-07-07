#include "lsm/manifest.hpp"

#include "lsm/errors.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace lsm {

namespace fs = std::filesystem;
using json = nlohmann::json;

Manifest Manifest::load_or_create(fs::path path) {
    Manifest m;
    m.path_ = std::move(path);

    // A leftover .tmp means we crashed before the atomic rename; the real
    // manifest is authoritative and the .tmp is garbage (5.3).
    std::error_code tmp_ec;
    fs::remove(fs::path(m.path_.string() + ".tmp"), tmp_ec);

    if (!fs::exists(m.path_)) {
        return m;   // fresh store
    }

    std::ifstream in(m.path_);
    if (!in) {
        throw Error(ErrorCode::IOFailure, "cannot open manifest: " + m.path_.string());
    }
    json doc;
    try {
        in >> doc;
        m.next_sst_id_ = doc.at("next_sst_id").get<std::uint64_t>();
        m.epoch_ = doc.value("epoch", std::uint64_t{0});
        for (const auto& t : doc.at("tables")) {
            TableMeta meta;
            meta.id = t.at("id").get<std::uint64_t>();
            meta.file_name = t.at("file_name").get<std::string>();
            meta.min_key = t.at("min_key").get<std::string>();
            meta.max_key = t.at("max_key").get<std::string>();
            meta.min_seqno = t.at("min_seqno").get<std::uint64_t>();
            meta.max_seqno = t.at("max_seqno").get<std::uint64_t>();
            meta.created_at = t.at("created_at").get<std::string>();
            meta.file_size = t.at("file_size").get<std::uint64_t>();
            meta.entries = t.at("entries").get<std::uint64_t>();
            meta.bloom_bits_per_key = t.at("bloom_bits_per_key").get<double>();
            meta.bloom_hashes = t.at("bloom_hashes").get<std::uint32_t>();
            m.tables_.push_back(std::move(meta));
        }
    } catch (const json::exception& e) {
        throw Error(ErrorCode::CorruptionDetected,
                    "invalid manifest JSON in " + m.path_.string() + ": " + e.what());
    }
    // Normalize to newest-first BY DATA AGE. After compaction, ids no longer
    // track age (an output file has a new id but old seqNos), so max_seqno —
    // unique across tables because ranges stay disjoint — is the order the
    // read path's first-hit-wins rule needs.
    std::sort(m.tables_.begin(), m.tables_.end(),
              [](const TableMeta& a, const TableMeta& b) { return a.max_seqno > b.max_seqno; });
    for (const auto& t : m.tables_) {
        m.next_sst_id_ = std::max(m.next_sst_id_, t.id + 1);
    }
    return m;
}

void Manifest::add_table(const TableMeta& meta) {
    tables_.insert(tables_.begin(), meta);   // newest first
    next_sst_id_ = std::max(next_sst_id_, meta.id + 1);
    epoch_ += 1;
    save();
}

void Manifest::apply_compaction(const std::vector<std::uint64_t>& removed_ids,
                                const TableMeta* added) {
    std::erase_if(tables_, [&](const TableMeta& t) {
        return std::find(removed_ids.begin(), removed_ids.end(), t.id) != removed_ids.end();
    });
    if (added) {
        tables_.push_back(*added);
        next_sst_id_ = std::max(next_sst_id_, added->id + 1);
    }
    std::sort(tables_.begin(), tables_.end(),
              [](const TableMeta& a, const TableMeta& b) { return a.max_seqno > b.max_seqno; });
    epoch_ += 1;
    save();   // one atomic rewrite: old and new sets are never both visible
}

void Manifest::save() const {
    json doc;
    doc["manifest_version"] = 1;
    doc["epoch"] = epoch_;
    doc["next_sst_id"] = next_sst_id_;
    doc["tables"] = json::array();
    for (const auto& t : tables_) {
        doc["tables"].push_back({
            {"id", t.id},
            {"file_name", t.file_name},
            {"min_key", t.min_key},
            {"max_key", t.max_key},
            {"min_seqno", t.min_seqno},
            {"max_seqno", t.max_seqno},
            {"created_at", t.created_at},
            {"file_size", t.file_size},
            {"entries", t.entries},
            {"bloom_bits_per_key", t.bloom_bits_per_key},
            {"bloom_hashes", t.bloom_hashes},
        });
    }

    const fs::path tmp = path_.string() + ".tmp";
    const std::string body = doc.dump(2);

    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) {
        throw Error(ErrorCode::IOFailure, "cannot create temp manifest: " + tmp.string());
    }
    const bool wrote = std::fwrite(body.data(), 1, body.size(), f) == body.size() &&
                       std::fflush(f) == 0 &&
#ifdef _WIN32
                       _commit(_fileno(f)) == 0;
#else
                       ::fsync(::fileno(f)) == 0;
#endif
    std::fclose(f);
    if (!wrote) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw Error(ErrorCode::IOFailure, "cannot write temp manifest: " + tmp.string());
    }

    std::error_code ec;
    fs::rename(tmp, path_, ec);   // atomic swap over the old manifest
    if (ec) {
        throw Error(ErrorCode::IOFailure,
                    "cannot publish manifest: " + path_.string() + ": " + ec.message());
    }
}

std::uint64_t Manifest::max_seqno() const noexcept {
    std::uint64_t v = 0;
    for (const auto& t : tables_) v = std::max(v, t.max_seqno);
    return v;
}

std::uint64_t Manifest::total_bytes() const noexcept {
    std::uint64_t v = 0;
    for (const auto& t : tables_) v += t.file_size;
    return v;
}

} // namespace lsm

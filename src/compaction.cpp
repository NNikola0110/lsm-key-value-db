#include "lsm/compaction.hpp"

#include "lsm/errors.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace lsm {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::optional<CompactionPick> pick_compaction(const std::vector<TableMeta>& tables,
                                              const Config& cfg) {
    const std::size_t k = std::max<std::uint32_t>(2, cfg.size_tiered_fan_in);
    if (tables.size() < k) return std::nullopt;

    std::optional<CompactionPick> best;         // qualifying window, fewest bytes
    std::optional<CompactionPick> fallback;     // any window, fewest bytes

    for (std::size_t i = 0; i + k <= tables.size(); ++i) {
        std::uint64_t min_size = UINT64_MAX, max_size = 0, total = 0;
        CompactionPick pick;
        for (std::size_t j = i; j < i + k; ++j) {
            const std::uint64_t s = tables[j].file_size;
            min_size = std::min(min_size, s);
            max_size = std::max(max_size, s);
            total += s;
            pick.ids.push_back(tables[j].id);
        }
        pick.total_bytes = total;

        if (!fallback || total < fallback->total_bytes) {
            pick.reason = "fallback smallest window";
            fallback = pick;
        }
        if (static_cast<double>(max_size) <=
            cfg.size_tiered_size_ratio * static_cast<double>(min_size)) {
            if (!best || total < best->total_bytes) {
                pick.reason = "size-similar window";
                best = pick;
            }
        }
    }
    return best ? best : fallback;
}

namespace {

fs::path state_path(const fs::path& data_dir) {
    return data_dir / "compaction_stats.json";
}

} // namespace

CompactionState load_compaction_state(const fs::path& data_dir) {
    CompactionState s;
    const fs::path path = state_path(data_dir);
    if (!fs::exists(path)) return s;

    std::ifstream in(path);
    json doc;
    try {
        in >> doc;
        s.next_job_id = doc.value("next_job_id", std::uint64_t{1});
        if (doc.contains("last_job")) {
            const auto& j = doc["last_job"];
            CompactionJobResult r;
            r.job_id = j.value("job_id", std::uint64_t{0});
            r.input_ids = j.value("input_ids", std::vector<std::uint64_t>{});
            r.output_file = j.value("output_file", std::string{});
            r.bytes_in = j.value("bytes_in", std::uint64_t{0});
            r.bytes_out = j.value("bytes_out", std::uint64_t{0});
            r.keys_in = j.value("keys_in", std::uint64_t{0});
            r.keys_out = j.value("keys_out", std::uint64_t{0});
            r.keys_dropped = j.value("keys_dropped", std::uint64_t{0});
            r.tombstones_kept = j.value("tombstones_kept", std::uint64_t{0});
            r.tombstones_dropped = j.value("tombstones_dropped", std::uint64_t{0});
            r.duration_ms = j.value("duration_ms", std::uint64_t{0});
            s.last_job = std::move(r);
            s.last_job_finished_at = doc.value("finished_at", std::string{});
        }
    } catch (const json::exception&) {
        // Stats are advisory; a garbled file just resets them.
        return CompactionState{};
    }
    return s;
}

void save_compaction_state(const fs::path& data_dir, const CompactionState& s) {
    json doc;
    doc["next_job_id"] = s.next_job_id;
    if (s.last_job) {
        const CompactionJobResult& r = *s.last_job;
        doc["last_job"] = {
            {"job_id", r.job_id},
            {"input_ids", r.input_ids},
            {"output_file", r.output_file},
            {"bytes_in", r.bytes_in},
            {"bytes_out", r.bytes_out},
            {"keys_in", r.keys_in},
            {"keys_out", r.keys_out},
            {"keys_dropped", r.keys_dropped},
            {"tombstones_kept", r.tombstones_kept},
            {"tombstones_dropped", r.tombstones_dropped},
            {"duration_ms", r.duration_ms},
        };
        doc["finished_at"] = s.last_job_finished_at;
    }
    std::ofstream out(state_path(data_dir), std::ios::trunc);
    out << doc.dump(2);
    // Advisory stats: no fsync/rename dance needed here.
}

std::int64_t parse_iso8601_utc(const std::string& s) {
    std::tm tm{};
    std::istringstream in(s);
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (in.fail()) return 0;
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

std::string format_job_line(const CompactionJobResult& r) {
    const double drop_pct = r.keys_in
        ? 100.0 * static_cast<double>(r.keys_dropped) / static_cast<double>(r.keys_in)
        : 0.0;
    std::ostringstream os;
    os << "compact job=" << r.job_id << " inputs=" << r.input_ids.size()
       << " bytes_in=" << r.bytes_in << " bytes_out=" << r.bytes_out
       << " keys_in=" << r.keys_in << " keys_out=" << r.keys_out
       << " drop=" << std::fixed << std::setprecision(0) << drop_pct << '%'
       << " tombstones_kept=" << r.tombstones_kept
       << " tombstones_dropped=" << r.tombstones_dropped
       << " dur=" << r.duration_ms << "ms"
       << " output=" << (r.output_file.empty() ? "(none)" : r.output_file);
    return os.str();
}

} // namespace lsm

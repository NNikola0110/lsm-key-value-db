#include "lsm/wal.hpp"

#include "lsm/errors.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <ostream>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace lsm {

namespace fs = std::filesystem;

namespace {

constexpr std::array<unsigned char, 4> kMagic = {'L', 'S', 'M', 'W'};
constexpr std::uint8_t  kVersion       = 1;
constexpr std::size_t   kHeaderSize    = 8;   // magic(4) + version(1) + reserved(3)
constexpr std::uint8_t  kTypePut       = 1;
constexpr std::uint8_t  kTypeDel       = 2;
constexpr std::size_t   kMinPayload    = 1 + 8 + 4 + 4;      // type+seqno+keylen+vallen
constexpr std::uint32_t kMaxPayload    = 1u << 30;           // sanity cap: 1 GiB
constexpr std::size_t   kFrameOverhead = 4 + 4;              // payload_len + crc32

// CRC-32 (IEEE, polynomial 0xEDB88320), table-based.
std::uint32_t crc32(const unsigned char* data, std::size_t len) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void put_u32(std::vector<unsigned char>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

void put_u64(std::vector<unsigned char>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

std::uint32_t get_u32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint64_t get_u64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

std::string segment_name(std::uint64_t id) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06llu.wal", static_cast<unsigned long long>(id));
    return buf;
}

// Segment files sorted by numeric id. Non-numeric *.wal names are ignored.
std::vector<std::pair<std::uint64_t, fs::path>> list_segments(const fs::path& dir) {
    std::vector<std::pair<std::uint64_t, fs::path>> out;
    if (!fs::exists(dir)) return out;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".wal") continue;
        const std::string stem = entry.path().stem().string();
        if (stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos) continue;
        out.emplace_back(std::stoull(stem), entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct SegmentScan {
    std::uint64_t records    = 0;
    std::uint64_t max_seqno  = 0;
    std::uint64_t good_bytes = 0;  // offset just past the last valid record
    bool          clean      = true;
};

// Scan one segment record by record. Any decode/checksum failure is treated
// as tail corruption (Section 1.10): stop at the previous good offset.
SegmentScan scan_segment(const fs::path& path) {
    SegmentScan result;

    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) {
        throw Error(ErrorCode::IOFailure, "cannot open WAL segment: " + path.string());
    }

    unsigned char header[kHeaderSize];
    if (std::fread(header, 1, kHeaderSize, f) != kHeaderSize ||
        std::memcmp(header, kMagic.data(), kMagic.size()) != 0 || header[4] != kVersion) {
        // Bad/short header: the whole file is unusable; truncate to zero.
        result.clean = false;
        result.good_bytes = 0;
        std::fclose(f);
        return result;
    }
    result.good_bytes = kHeaderSize;

    std::vector<unsigned char> buf;
    for (;;) {
        unsigned char lenb[4];
        const std::size_t n = std::fread(lenb, 1, 4, f);
        if (n == 0) break;                       // clean end of segment
        if (n < 4) { result.clean = false; break; }

        const std::uint32_t payload_len = get_u32(lenb);
        if (payload_len < kMinPayload || payload_len > kMaxPayload) {
            result.clean = false;                // absurd length => garbage tail
            break;
        }

        buf.resize(payload_len + 4);             // payload + crc
        if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
            result.clean = false;                // partial record at tail
            break;
        }
        if (crc32(buf.data(), payload_len) != get_u32(buf.data() + payload_len)) {
            result.clean = false;                // garbled bytes
            break;
        }

        const std::uint8_t type = buf[0];
        const std::uint64_t seqno = get_u64(buf.data() + 1);
        const std::uint32_t key_len = get_u32(buf.data() + 9);
        const std::uint32_t val_len = get_u32(buf.data() + 13);
        if ((type != kTypePut && type != kTypeDel) ||
            kMinPayload + static_cast<std::uint64_t>(key_len) + val_len != payload_len ||
            (type == kTypeDel && val_len != 0)) {
            result.clean = false;                // self-description doesn't add up
            break;
        }

        result.records += 1;
        result.max_seqno = std::max(result.max_seqno, seqno);
        result.good_bytes += kFrameOverhead + payload_len;
    }

    std::fclose(f);
    return result;
}

WalRecoveryReport scan_all(const fs::path& dir, bool truncate) {
    WalRecoveryReport report;
    for (const auto& [id, path] : list_segments(dir)) {
        const SegmentScan scan = scan_segment(path);
        report.segments += 1;
        report.records += scan.records;
        report.last_seqno = std::max(report.last_seqno, scan.max_seqno);
        if (!scan.clean) {
            report.truncated += 1;
            report.truncated_segment = path.filename().string();
            report.truncated_to = scan.good_bytes;
            if (truncate) {
                std::error_code ec;
                fs::resize_file(path, scan.good_bytes, ec);
                if (ec) {
                    throw Error(ErrorCode::IOFailure,
                                "cannot truncate corrupt WAL tail in " + path.string() +
                                    ": " + ec.message());
                }
            }
        }
    }
    return report;
}

void fsync_file(std::FILE* f, const fs::path& path) {
    if (std::fflush(f) != 0) {
        throw Error(ErrorCode::IOFailure, "fflush failed for " + path.string());
    }
#ifdef _WIN32
    if (_commit(_fileno(f)) != 0)
#else
    if (::fsync(::fileno(f)) != 0)
#endif
    {
        throw Error(ErrorCode::IOFailure, "fsync failed for " + path.string());
    }
}

} // namespace

void WalRecoveryReport::print(std::ostream& os) const {
    os << "recovery: segments=" << segments << " records=" << records
       << " truncated=" << truncated << " last_seqno=" << last_seqno << " status=OK";
    if (truncated > 0) {
        os << " truncated_segment=" << truncated_segment << " truncated_to=" << truncated_to;
    }
    os << '\n';
}

Wal::Wal(fs::path dir, std::uint32_t fsync_every_n, std::uint64_t roll_bytes)
    : dir_(std::move(dir)),
      fsync_every_n_(fsync_every_n == 0 ? 1 : fsync_every_n),
      roll_bytes_(roll_bytes) {}

Wal::~Wal() {
    if (file_) {
        // Best effort only; explicit close() reports errors properly.
        try { close(); } catch (...) {}
    }
}

WalRecoveryReport Wal::open() {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
        throw Error(ErrorCode::IOFailure,
                    "cannot create WAL dir '" + dir_.string() + "': " + ec.message());
    }

    const WalRecoveryReport report = scan_all(dir_, /*truncate=*/true);
    last_seqno_ = report.last_seqno;
    total_segments_ = report.segments;

    const auto segments = list_segments(dir_);
    if (segments.empty()) {
        open_active_segment(1, /*fresh=*/true);
        total_segments_ = 1;
    } else {
        const auto& [id, path] = segments.back();
        // A segment truncated all the way to zero needs its header rewritten.
        open_active_segment(id, /*fresh=*/fs::file_size(path) < kHeaderSize);
    }
    return report;
}

void Wal::open_active_segment(std::uint64_t id, bool fresh) {
    const fs::path path = dir_ / segment_name(id);
    std::FILE* f = std::fopen(path.string().c_str(), "ab");
    if (!f) {
        throw Error(ErrorCode::IOFailure, "cannot open WAL segment: " + path.string());
    }
    file_ = f;
    active_id_ = id;

    if (fresh) {
        std::vector<unsigned char> header(kMagic.begin(), kMagic.end());
        header.push_back(kVersion);
        header.insert(header.end(), 3, 0);  // reserved/flags
        if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
            throw Error(ErrorCode::IOFailure, "cannot write WAL header: " + path.string());
        }
        fsync_file(file_, path);
    }
    active_bytes_ = fs::file_size(path);
}

void Wal::roll_segment() {
    const fs::path old_path = dir_ / segment_name(active_id_);
    fsync_file(file_, old_path);
    std::fclose(file_);
    file_ = nullptr;

    open_active_segment(active_id_ + 1, /*fresh=*/true);
    total_segments_ += 1;
    appends_since_sync_ = 0;
}

std::uint64_t Wal::append_put(std::string_view key, std::string_view value) {
    return append(kTypePut, key, value);
}

std::uint64_t Wal::append_del(std::string_view key) {
    return append(kTypeDel, key, {});
}

std::uint64_t Wal::append(std::uint8_t type, std::string_view key, std::string_view value) {
    if (!file_) {
        throw Error(ErrorCode::StoreClosed, "WAL is not open");
    }
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    if (key.size() > kMaxPayload || value.size() > kMaxPayload) {
        throw Error(ErrorCode::InvalidArgument, "key/value exceeds 1 GiB limit");
    }

    const std::uint64_t seqno = last_seqno_ + 1;
    const std::uint32_t payload_len =
        static_cast<std::uint32_t>(kMinPayload + key.size() + value.size());

    std::vector<unsigned char> rec;
    rec.reserve(kFrameOverhead + payload_len);
    put_u32(rec, payload_len);
    rec.push_back(type);
    put_u64(rec, seqno);
    put_u32(rec, static_cast<std::uint32_t>(key.size()));
    put_u32(rec, static_cast<std::uint32_t>(value.size()));
    rec.insert(rec.end(), key.begin(), key.end());
    rec.insert(rec.end(), value.begin(), value.end());
    put_u32(rec, crc32(rec.data() + 4, payload_len));

    if (active_bytes_ > kHeaderSize && active_bytes_ + rec.size() > roll_bytes_) {
        roll_segment();
    }

    const fs::path path = dir_ / segment_name(active_id_);
    if (std::fwrite(rec.data(), 1, rec.size(), file_) != rec.size()) {
        throw Error(ErrorCode::IOFailure, "cannot append WAL record: " + path.string());
    }
    active_bytes_ += rec.size();

    // Only after the record meets the sync policy do we acknowledge success.
    appends_since_sync_ += 1;
    if (appends_since_sync_ >= fsync_every_n_) {
        fsync_file(file_, path);
        appends_since_sync_ = 0;
    }

    last_seqno_ = seqno;
    return seqno;
}

void Wal::sync() {
    if (file_) {
        fsync_file(file_, dir_ / segment_name(active_id_));
        appends_since_sync_ = 0;
    }
}

void Wal::close() {
    if (!file_) return;
    sync();
    std::fclose(file_);
    file_ = nullptr;
}

WalStats Wal::stats() const {
    WalStats s;
    s.active_segment_id = active_id_;
    s.active_segment_bytes = active_bytes_;
    s.total_segments = total_segments_;
    s.last_seqno = last_seqno_;
    s.fsync_every_n = fsync_every_n_;
    s.roll_bytes = roll_bytes_;
    return s;
}

WalRecoveryReport Wal::verify(const fs::path& dir) {
    return scan_all(dir, /*truncate=*/false);
}

} // namespace lsm

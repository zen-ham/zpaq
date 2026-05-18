// _zpaq.cpp - pybind11 binding for libzpaq, providing pure in-memory
// ZPAQ compression and decompression for Python.
//
// libzpaq (Matt Mahoney, public domain) exposes abstract Reader/Writer
// interfaces. We implement byte-vector-backed adapters so all I/O stays
// in memory - no temp files, no subprocess invocation.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "vendor/libzpaq.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <array>
#include <future>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace py = pybind11;

namespace zpaq_internal {

class ZpaqError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Reader over a contiguous byte buffer (zero-copy view).
class BytesReader : public libzpaq::Reader {
public:
    BytesReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size), offset_(0) {}

    int get() override {
        if (offset_ >= size_) return -1;
        return static_cast<int>(data_[offset_++]);
    }

    int read(char* buf, int n) override {
        if (n <= 0 || offset_ >= size_) return 0;
        std::size_t remaining = size_ - offset_;
        std::size_t take = static_cast<std::size_t>(n) < remaining
                               ? static_cast<std::size_t>(n)
                               : remaining;
        std::memcpy(buf, data_ + offset_, take);
        offset_ += take;
        return static_cast<int>(take);
    }

    void rewind() { offset_ = 0; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_;
};

// Writer that accumulates into a std::vector<uint8_t>.
class BytesWriter : public libzpaq::Writer {
public:
    void put(int c) override {
        buf_.push_back(static_cast<std::uint8_t>(c));
    }

    void write(const char* buf, int n) override {
        if (n <= 0) return;
        buf_.insert(buf_.end(),
                    reinterpret_cast<const std::uint8_t*>(buf),
                    reinterpret_cast<const std::uint8_t*>(buf) + n);
    }

    std::vector<std::uint8_t>& buffer() { return buf_; }

private:
    std::vector<std::uint8_t> buf_;
};

// Writer that accumulates into a std::string. Used to read out per-segment
// filenames and comments so we can identify which segments are JIDAC index
// metadata vs actual file data.
class StringCollector : public libzpaq::Writer {
public:
    void put(int c) override { s_.push_back(static_cast<char>(c)); }

    void write(const char* buf, int n) override {
        if (n > 0) s_.append(buf, static_cast<std::size_t>(n));
    }

    void clear() { s_.clear(); }
    const std::string& str() const { return s_; }

private:
    std::string s_;
};

// `zpaq a` writes archives whose every segment is named with the JIDAC
// convention: exactly "jDC" + 14 ascii-digit date + 1 type char + decimal
// fragment number, total 28 bytes. The type char distinguishes:
//   'c' - block/cluster header (metadata)
//   'd' - data (the actual file content we care about)
//   'h' - fragment hash index (metadata)
//   'i' - directory / file info (metadata)
inline bool is_jidac_metadata_segment(const std::string& fname) {
    if (fname.size() < 18) return false;
    if (fname.compare(0, 3, "jDC") != 0) return false;
    const char t = fname[17];
    return t == 'c' || t == 'h' || t == 'i';
}

inline bool is_jidac_segment(const std::string& fname) {
    return fname.size() >= 3 && fname.compare(0, 3, "jDC") == 0;
}

// JIDAC 'd' segments contain concatenated file fragments followed by a
// trailing footer:
//   [frag_1 data][frag_2 data]...[frag_N data]
//   [4-byte LE size_1]...[4-byte LE size_N]
//   [4-byte LE zero sentinel] [4-byte LE N]
// Returns the number of trailing bytes to strip, or 0 if the payload is
// not a JIDAC data segment.
inline std::size_t jidac_data_footer_size(const std::uint8_t* data,
                                          std::size_t size) {
    if (size < 8) return 0;
    auto read_le32 = [](const std::uint8_t* p) -> std::uint32_t {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    };
    const std::uint32_t N = read_le32(data + size - 4);
    if (N == 0 || N > (1u << 20)) return 0;
    const std::size_t footer = static_cast<std::size_t>(N) * 4 + 8;
    if (footer >= size) return 0;
    if (read_le32(data + size - 8) != 0) return 0;
    const std::uint8_t* sizes = data + size - footer;
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < N; ++i) {
        sum += read_le32(sizes + i * 4);
        if (sum > size) return 0;
    }
    if (sum != size - footer) return 0;
    return footer;
}

// Compute the same N2 (redundancy) and N3 (exe/text flag) hints that the
// official zpaq CLI passes to libzpaq when invoked with -mN. Mirrors the
// per-fragment heuristics in zpaq.cpp around line 2436 collapsed to a
// single fragment covering the whole buffer.
inline std::string method_with_hints(int level,
                                     const std::uint8_t* data,
                                     std::size_t size) {
    if (size == 0) {
        char buf[2] = {static_cast<char>('0' + level), '\0'};
        return std::string(buf);
    }
    unsigned char o1[256] = {0};
    std::uint64_t hits = 0;
    unsigned char prev = 0;
    for (std::size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (i > 0 && c == o1[prev]) ++hits;
        o1[prev] = c;
        prev = c;
    }
    int text_score = 0, exe_score = 0;
    for (int i = 0; i < 256; ++i) {
        unsigned char p = o1[i];
        if (p == ' ' && (std::isalnum(i) || i == '.' || i == ',')) ++text_score;
        if (p && (i < 9 || i == 11 || i == 12 ||
                  (i >= 14 && i <= 31) || i >= 240))
            --text_score;
        if (i >= 192 && i < 240 && p && (p < 128 || p >= 192))
            --text_score;
        if (p == 139) ++exe_score;
    }
    const bool is_text = (text_score >= 3);
    const bool is_exe = (exe_score >= 5);
    unsigned n2 = static_cast<unsigned>(
        hits / (static_cast<std::uint64_t>(size) / 256 + 1));
    if (n2 > 255) n2 = 255;
    const int n3 = (is_exe ? 2 : 0) + (is_text ? 1 : 0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d,%u,%d", level, n2, n3);
    return std::string(buf);
}

// 64 KB minimum chunk per worker thread. Below this the per-block
// compressor overhead and the ratio cost of more block boundaries
// outweigh the parallelism gain.
static constexpr std::size_t kMinChunkBytes = 64 * 1024;

// =================== JIDAC dedup encoder ===================

struct FragmentInfo {
    unsigned char sha1[20];
    std::uint32_t usize;
    std::size_t offset;  // offset in input
    // CLI-compatible per-fragment scoring used when computing per-block
    // method hints. `hits` is the max of four redundancy tests, `text1`
    // and `exe1` are the boolean fragment-type flags. Default 0 so non
    // -scoring code paths still work.
    std::uint32_t hits = 0;
    bool text1 = false;
    bool exe1 = false;
};

// Per-byte fragment-end probability decay table. Verbatim from zpaq.cpp;
// used in the "non-uniform o1 distribution" redundancy test.
static const unsigned char kFragDecayTable[256] = {
    160,80,53,40,32,26,22,20,17,16,14,13,12,11,10,10,
      9, 8, 8, 8, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5,
      4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3,
      3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

struct Sha20Hash {
    std::size_t operator()(const std::array<unsigned char, 20>& a) const noexcept {
        std::size_t h = 0;
        std::memcpy(&h, a.data(), sizeof(h));
        return h;
    }
};
struct Sha20Eq {
    bool operator()(const std::array<unsigned char, 20>& a,
                    const std::array<unsigned char, 20>& b) const noexcept {
        return std::memcmp(a.data(), b.data(), 20) == 0;
    }
};

// Content-defined chunking using zpaq's rolling-hash algorithm. Produces
// fragments sized between MIN_FRAGMENT and MAX_FRAGMENT bytes. The same
// inputs yield the same fragment boundaries as `zpaq.exe a`, so identical
// content across files dedupes cleanly.
inline std::vector<FragmentInfo> chunk_input(const std::uint8_t* ptr,
                                             std::size_t size,
                                             int fragment_param = 6) {
    // For method "5" the blocksize is (1<<24)-4096 ~= 16 MB.
    const unsigned blocksize = (1u << 24) - 4096;
    const unsigned MAX_FRAGMENT =
        (fragment_param > 19 || (8128u << fragment_param) > blocksize - 12)
            ? blocksize - 12 : (8128u << fragment_param);
    const unsigned MIN_FRAGMENT =
        (fragment_param > 25 || (64u << fragment_param) > MAX_FRAGMENT)
            ? MAX_FRAGMENT : (64u << fragment_param);

    std::vector<FragmentInfo> frags;
    if (size == 0) return frags;

    // Tracks the previous N fragments' o1 tables for the cross-fragment
    // correlation redundancy test. ON=4 matches zpaq.cpp.
    static constexpr int ON = 4;
    unsigned char o1prev[256 * ON] = {0};

    std::size_t pos = 0;
    while (pos < size) {
        unsigned char o1[256] = {0};
        int c1 = 0;
        unsigned h = 0;
        libzpaq::SHA1 sha1;
        unsigned sz = 0;
        std::uint32_t hits_count = 0;  // order-1 successful predictions
        std::size_t start = pos;

        while (pos < size) {
            unsigned char c = ptr[pos++];
            if (c == o1[c1]) {
                h = (h + c + 1) * 314159265u;
                ++hits_count;
            } else {
                h = (h + c + 1) * 271828182u;
            }
            o1[c1] = c;
            c1 = c;
            sha1.put(c);
            ++sz;
            if (sz >= MAX_FRAGMENT) break;
            if (fragment_param <= 22 && sz >= MIN_FRAGMENT &&
                h < (1u << (22 - fragment_param))) break;
        }

        // Per-fragment scoring (mirrors zpaq.cpp lines 2436-2471).
        // Four redundancy tests; take the max as `hits`. Plus boolean
        // text1 / exe1 flags from order-1 transition table analysis.
        int text_score = 0, exe_score = 0;
        std::int64_t h1 = sz;
        unsigned char o1ct[256] = {0};
        for (int i = 0; i < 256; ++i) {
            if (o1ct[o1[i]] < 255) {
                h1 -= (static_cast<std::int64_t>(sz) *
                       kFragDecayTable[o1ct[o1[i]]++]) >> 15;
            }
            if (o1[i] == ' ' && (std::isalnum(i) || i == '.' || i == ','))
                ++text_score;
            if (o1[i] && (i < 9 || i == 11 || i == 12 ||
                          (i >= 14 && i <= 31) || i >= 240))
                --text_score;
            if (i >= 192 && i < 240 && o1[i] && (o1[i] < 128 || o1[i] >= 192))
                --text_score;
            if (o1[i] == 139) ++exe_score;
        }
        bool text1 = (text_score >= 3);
        bool exe1 = (exe_score >= 5);
        if (sz > 0) h1 = h1 * h1 / sz;  // Test 2: near 0 if random
        std::uint32_t hits = hits_count;  // Test 1: o1 hit rate
        std::uint32_t h2 = static_cast<std::uint32_t>(h1);
        if (h2 > hits) hits = h2;
        h2 = static_cast<std::uint32_t>(o1ct[0]) * sz / 256;  // Test 3
        if (h2 > hits) hits = h2;
        h2 = 0;
        for (int i = 0; i < 256 * ON; ++i)  // Test 4: o1 vs previous frag
            h2 += (o1prev[i] == o1[i & 255]) ? 1u : 0u;
        h2 = h2 * sz / (256 * ON);
        if (h2 > hits) hits = h2;
        if (hits > sz) hits = sz;

        // Shift the rolling history.
        std::memmove(o1prev, o1prev + 256, 256 * (ON - 1 > 0 ? ON - 1 : 0));
        std::memcpy(o1prev + 256 * (ON - 1), o1, 256);

        FragmentInfo f;
        std::memcpy(f.sha1, sha1.result(), 20);
        f.usize = sz;
        f.offset = start;
        f.hits = hits;
        f.text1 = text1;
        f.exe1 = exe1;
        frags.push_back(f);
    }
    return frags;
}

// Write a little-endian integer of N bytes through a libzpaq Writer.
inline void put_le(libzpaq::Writer& w, std::uint64_t x, int n) {
    for (int i = 0; i < n; ++i) {
        w.put(static_cast<int>(x & 0xFF));
        x >>= 8;
    }
}

// Pad a non-negative integer to at least `n` decimal digits.
inline std::string pad_digits(std::int64_t x, int n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*lld", n, static_cast<long long>(x));
    return std::string(buf);
}

// Build a JIDAC-format archive with fragment-level deduplication. The
// archive contains:
//   1. a 'c' header segment with the total compressed-data size
//   2. one or more 'd' data segments, each containing concatenated unique
//      fragments followed by a footer of fragment sizes
//   3. one 'h' segment per 'd' block, listing fragment SHA-1 and size
//   4. one 'i' segment with the virtual file's fragment-ID list
// The result decompresses correctly with both `zpaq.decompress()` and the
// official `zpaq x` CLI, but compresses better than raw streaming on
// inputs with repeated content because identical fragments are stored once.
py::bytes compress_dedup(py::buffer data, int level, py::object method_obj,
                         bool hints) {
    if (level < 0 || level > 5) {
        throw py::value_error("level must be in 0..5");
    }
    py::buffer_info info = data.request();
    if (info.ndim != 1 || info.itemsize != 1) {
        throw py::value_error("data must be a 1-D bytes-like buffer");
    }
    const std::uint8_t* ptr = reinterpret_cast<const std::uint8_t*>(info.ptr);
    const std::size_t size = static_cast<std::size_t>(info.size);
    if (size == 0) return py::bytes("", 0);

    // Resolve method string for the 'd' segments.
    std::string method_str;
    bool have_override = false;
    if (!method_obj.is_none()) {
        method_str = py::cast<std::string>(method_obj);
        have_override = true;
    }

    // Fixed JIDAC date - any valid 14-digit YYYYMMDDHHMMSS works.
    const std::int64_t date = 20240101000000LL;
    auto seg_name = [&](char type, std::int64_t num) -> std::string {
        return "jDC" + pad_digits(date, 14) + std::string(1, type) + pad_digits(num, 10);
    };

    BytesWriter out;

    {
        py::gil_scoped_release release;

        // 1. Chunk the input.
        auto fragments = chunk_input(ptr, size);

        // 2. Dedup - track unique fragments, record per-input-fragment refs.
        std::unordered_map<std::array<unsigned char, 20>, std::uint32_t,
                           Sha20Hash, Sha20Eq> seen;
        std::vector<FragmentInfo> unique_frags;
        std::vector<std::uint32_t> file_refs;
        unique_frags.reserve(fragments.size());
        file_refs.reserve(fragments.size());
        for (auto& f : fragments) {
            std::array<unsigned char, 20> key;
            std::memcpy(key.data(), f.sha1, 20);
            auto it = seen.find(key);
            if (it != seen.end()) {
                file_refs.push_back(it->second);
            } else {
                std::uint32_t id =
                    static_cast<std::uint32_t>(unique_frags.size()) + 1;
                seen.emplace(key, id);
                unique_frags.push_back(f);
                file_refs.push_back(id);
            }
        }

        // 3. Group unique fragments into 'd' blocks (~16 MB uncompressed each).
        const std::size_t BLOCK_TARGET = 16 * 1024 * 1024;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> block_ranges;
        {
            std::uint32_t start_id = 1;
            std::size_t accum = 0;
            for (std::uint32_t i = 0; i < unique_frags.size(); ++i) {
                accum += unique_frags[i].usize;
                if (accum >= BLOCK_TARGET || i + 1 == unique_frags.size()) {
                    block_ranges.emplace_back(start_id, i + 2);
                    start_id = i + 2;
                    accum = 0;
                }
            }
        }

        // 4. Header: reserve. The 'c' segment for an "0"-method block with a
        // single 8-byte payload has a fixed compressed-size, so we can write
        // a placeholder, compress the 'd' segments, then overwrite the
        // header bytes in-place with the real cdata value.
        BytesWriter placeholder_header;
        {
            libzpaq::StringBuffer hsb;
            put_le(hsb, 0, 8);
            libzpaq::compressBlock(&hsb, &placeholder_header, "0",
                                   seg_name('c', static_cast<std::int64_t>(
                                       unique_frags.size() + 1)).c_str(),
                                   "jDC\x01", false);
        }
        const std::size_t header_size = placeholder_header.buffer().size();
        out.write(reinterpret_cast<const char*>(placeholder_header.buffer().data()),
                  static_cast<int>(header_size));

        // 5. Compress each 'd' block, in parallel. Each block is an
        // independent libzpaq invocation so they can run on separate
        // threads; we collect outputs in a slot per block and write
        // them back to `out` in order. Work-stealing via an atomic
        // index so blocks of uneven size load-balance across cores.
        std::vector<std::vector<std::uint8_t>> d_outputs(block_ranges.size());
        std::vector<std::size_t> d_csizes(block_ranges.size(), 0);
        std::vector<std::string> d_errors(block_ranges.size());

        auto compress_one = [&](std::size_t bi) {
            try {
                auto& range = block_ranges[bi];
                const std::uint32_t start = range.first;
                const std::uint32_t end = range.second;
                const std::uint32_t n = end - start;

                libzpaq::StringBuffer sb;
                for (std::uint32_t i = start; i < end; ++i) {
                    const auto& f = unique_frags[i - 1];
                    sb.write(reinterpret_cast<const char*>(ptr + f.offset),
                             static_cast<int>(f.usize));
                }
                for (std::uint32_t i = start; i < end; ++i) {
                    put_le(sb, unique_frags[i - 1].usize, 4);
                }
                put_le(sb, 0, 4);
                put_le(sb, n, 4);

                std::string this_method;
                if (have_override) {
                    this_method = method_str;
                } else if (hints) {
                    std::uint64_t redundancy = 0;
                    std::uint32_t exe_count = 0;
                    std::uint32_t text_count = 0;
                    std::uint32_t frag_count = n;
                    for (std::uint32_t i = start; i < end; ++i) {
                        const auto& f = unique_frags[i - 1];
                        redundancy += f.hits;
                        if (f.exe1) exe_count += 4;
                        if (f.text1) text_count += 2;
                    }
                    const std::size_t block_usize_no_footer =
                        sb.size() - (4ull * n + 8);
                    std::uint64_t denom =
                        static_cast<std::uint64_t>(block_usize_no_footer) / 256 + 1;
                    std::uint64_t n2 = redundancy / denom;
                    if (n2 > 255) n2 = 255;
                    int n3 = ((exe_count > frag_count) ? 2 : 0) +
                             ((text_count > frag_count) ? 1 : 0);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%d,%u,%d",
                                  level, static_cast<unsigned>(n2), n3);
                    this_method = std::string(buf);
                } else {
                    this_method = std::string(1, static_cast<char>('0' + level));
                }

                BytesWriter d_sink;
                libzpaq::compressBlock(&sb, &d_sink, this_method.c_str(),
                                       seg_name('d', static_cast<std::int64_t>(
                                           start)).c_str(),
                                       "jDC\x01", false);
                d_csizes[bi] = d_sink.buffer().size();
                d_outputs[bi] = std::move(d_sink.buffer());
            } catch (const std::exception& e) {
                d_errors[bi] = e.what();
            } catch (...) {
                d_errors[bi] = "unknown error in d-block worker";
            }
        };

        int nworkers = static_cast<int>(std::thread::hardware_concurrency());
        if (nworkers < 1) nworkers = 1;
        if (static_cast<std::size_t>(nworkers) > block_ranges.size()) {
            nworkers = static_cast<int>(block_ranges.size());
        }

        if (nworkers <= 1 || block_ranges.size() <= 1) {
            for (std::size_t bi = 0; bi < block_ranges.size(); ++bi) {
                compress_one(bi);
            }
        } else {
            std::atomic<std::size_t> next_block{0};
            auto worker_loop = [&]() {
                while (true) {
                    std::size_t bi = next_block.fetch_add(1,
                        std::memory_order_relaxed);
                    if (bi >= block_ranges.size()) break;
                    compress_one(bi);
                }
            };
            std::vector<std::thread> workers;
            workers.reserve(nworkers - 1);
            for (int i = 0; i < nworkers - 1; ++i) {
                workers.emplace_back(worker_loop);
            }
            worker_loop();
            for (auto& w : workers) w.join();
        }

        for (auto& e : d_errors) {
            if (!e.empty()) {
                throw zpaq_internal::ZpaqError(e);
            }
        }

        const std::size_t d_section_start = out.buffer().size();
        for (auto& d_out : d_outputs) {
            out.write(reinterpret_cast<const char*>(d_out.data()),
                      static_cast<int>(d_out.size()));
        }
        const std::size_t cdata = out.buffer().size() - d_section_start;

        // 6. Patch the header with the real cdata value.
        BytesWriter real_header;
        {
            libzpaq::StringBuffer hsb;
            put_le(hsb, cdata, 8);
            libzpaq::compressBlock(&hsb, &real_header, "0",
                                   seg_name('c', static_cast<std::int64_t>(
                                       unique_frags.size() + 1)).c_str(),
                                   "jDC\x01", false);
        }
        if (real_header.buffer().size() != header_size) {
            throw zpaq_internal::ZpaqError(
                "internal: JIDAC header size mismatch");
        }
        std::memcpy(out.buffer().data(), real_header.buffer().data(),
                    header_size);

        // 7. 'h' segments - one per 'd' block.
        for (std::size_t b = 0; b < block_ranges.size(); ++b) {
            std::uint32_t start = block_ranges[b].first;
            std::uint32_t end = block_ranges[b].second;
            libzpaq::StringBuffer is;
            put_le(is, d_csizes[b], 4);
            for (std::uint32_t i = start; i < end; ++i) {
                const auto& f = unique_frags[i - 1];
                is.write(reinterpret_cast<const char*>(f.sha1), 20);
                put_le(is, f.usize, 4);
            }
            BytesWriter h_sink;
            libzpaq::compressBlock(&is, &h_sink, "0",
                                   seg_name('h', static_cast<std::int64_t>(
                                       start)).c_str(),
                                   "jDC\x01", false);
            out.write(reinterpret_cast<const char*>(h_sink.buffer().data()),
                      static_cast<int>(h_sink.buffer().size()));
        }

        // 8. 'i' segment - virtual file "data" with all refs.
        // We use a fixed filename so `zpaq x` has somewhere to extract to.
        // (Empty filenames are filtered by the CLI and result in 0 extracted
        // files; the bytes-in / bytes-out caller doesn't see the name.)
        {
            libzpaq::StringBuffer is;
            put_le(is, date, 8);
            const char* vname = "data";
            is.write(vname, static_cast<int>(std::strlen(vname)));
            is.put(0);            // null terminator
            put_le(is, 0, 4);     // no attributes
            put_le(is, file_refs.size(), 4);
            for (auto r : file_refs) put_le(is, r, 4);
            BytesWriter i_sink;
            libzpaq::compressBlock(&is, &i_sink, "1",
                                   seg_name('i', 1).c_str(),
                                   "jDC\x01", false);
            out.write(reinterpret_cast<const char*>(i_sink.buffer().data()),
                      static_cast<int>(i_sink.buffer().size()));
        }
    }

    auto& v = out.buffer();
    return py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
}

// =================== streaming-format compress ===================
// Forward declaration for the dispatcher below.
py::bytes compress_bytes(py::buffer data, int level, int threads,
                         bool hints, bool verify, py::object method_obj);

// Public-facing dispatcher: streaming format by default, JIDAC dedup
// archive when `dedup=True`. JIDAC mode ignores `threads` and `verify`
// (parallel JIDAC encoding + segment-level SHA-1 are bigger features
// that aren't in this release yet).
py::bytes compress_dispatch(py::buffer data, int level, int threads,
                            bool hints, bool verify, py::object method_obj,
                            bool dedup) {
    if (dedup) {
        return compress_dedup(data, level, method_obj, hints);
    }
    return compress_bytes(data, level, threads, hints, verify, method_obj);
}

py::bytes compress_bytes(py::buffer data, int level, int threads,
                         bool hints, bool verify, py::object method_obj) {
    if (level < 0 || level > 5) {
        throw py::value_error("level must be in 0..5");
    }
    py::buffer_info info = data.request();
    if (info.ndim != 1 || info.itemsize != 1) {
        throw py::value_error("data must be a 1-D bytes-like buffer");
    }
    const std::uint8_t* const ptr =
        reinterpret_cast<const std::uint8_t*>(info.ptr);
    const std::size_t size = static_cast<std::size_t>(info.size);

    // Resolve thread count and clamp to what the input size can support.
    int t = threads;
    if (t <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        t = hw > 0 ? static_cast<int>(hw) : 1;
    }
    if (t < 1) t = 1;
    std::size_t max_threads_by_size = size / kMinChunkBytes;
    if (max_threads_by_size < 1) max_threads_by_size = 1;
    if (static_cast<std::size_t>(t) > max_threads_by_size) {
        t = static_cast<int>(max_threads_by_size);
    }

    // Resolve method string. Explicit override > computed hints > bare level.
    std::string override_method;
    bool have_override = false;
    if (!method_obj.is_none()) {
        override_method = py::cast<std::string>(method_obj);
        have_override = true;
    }
    auto pick_method = [&](const std::uint8_t* chunk_ptr,
                           std::size_t chunk_size) -> std::string {
        if (have_override) return override_method;
        if (hints) return method_with_hints(level, chunk_ptr, chunk_size);
        return std::string(1, static_cast<char>('0' + level));
    };

    std::vector<std::uint8_t> result;

    if (t <= 1) {
        BytesReader reader(ptr, size);
        BytesWriter writer;
        const std::string method = pick_method(ptr, size);
        {
            py::gil_scoped_release release;
            libzpaq::compress(&reader, &writer, method.c_str(),
                              nullptr, nullptr, verify);
        }
        result = std::move(writer.buffer());
    } else {
        const int nworkers = t;
        const std::size_t chunk = size / nworkers;
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        ranges.reserve(nworkers);
        for (int i = 0; i < nworkers; ++i) {
            std::size_t start = i * chunk;
            std::size_t end = (i + 1 == nworkers) ? size : (i + 1) * chunk;
            ranges.emplace_back(start, end);
        }
        std::vector<std::vector<std::uint8_t>> outs(nworkers);
        std::vector<std::string> errors(nworkers);
        {
            py::gil_scoped_release release;
            std::vector<std::thread> workers;
            workers.reserve(nworkers - 1);
            auto run_block = [&](int i) {
                try {
                    const std::size_t s = ranges[i].first;
                    const std::size_t e = ranges[i].second;
                    const std::size_t len = e - s;
                    libzpaq::StringBuffer sb(len);
                    sb.write(reinterpret_cast<const char*>(ptr + s),
                             static_cast<int>(len));
                    BytesWriter w;
                    const std::string method = pick_method(ptr + s, len);
                    libzpaq::compressBlock(&sb, &w, method.c_str(),
                                           nullptr, nullptr, verify);
                    outs[i] = std::move(w.buffer());
                } catch (const std::exception& e) {
                    errors[i] = e.what();
                } catch (...) {
                    errors[i] = "unknown error in worker thread";
                }
            };
            for (int i = 1; i < nworkers; ++i) {
                workers.emplace_back(run_block, i);
            }
            run_block(0);
            for (auto& w : workers) w.join();
        }
        for (int i = 0; i < nworkers; ++i) {
            if (!errors[i].empty()) {
                throw zpaq_internal::ZpaqError(errors[i]);
            }
        }
        std::size_t total = 0;
        for (auto& o : outs) total += o.size();
        result.reserve(total);
        for (auto& o : outs) {
            result.insert(result.end(), o.begin(), o.end());
        }
    }
    return py::bytes(reinterpret_cast<const char*>(result.data()),
                     result.size());
}

// 13-byte locator tag that opens every libzpaq-produced block. We use it
// to reject inputs that obviously aren't ZPAQ streams up-front (otherwise
// libzpaq silently emits zero bytes for garbage data).
static const unsigned char kZpaqLocatorTag[13] = {
    0x37, 0x6B, 0x53, 0x74, 0xA0, 0x31, 0x83, 0xD3,
    0x8C, 0xB2, 0x28, 0xB0, 0xD3,
};

// Holder for the JIDAC two-pass decompress: data segments populate
// `frags` keyed by fragment ID; 'i' segments populate `refs` with the
// per-file fragment-ID sequence. The final output is built by replaying
// `refs` through `frags`.
struct JidacBuilder {
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> frags;
    std::vector<std::uint32_t> refs;
    bool saw_refs = false;
};

inline std::uint32_t read_le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8)
         | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

// Parse the trailing 10-digit number out of a 28-byte JIDAC segment
// filename "jDC<14-digit-date><type><10-digit-num>". Returns -1 on
// malformed input.
inline long parse_jidac_num(const std::string& fname) {
    if (fname.size() != 28) return -1;
    long n = 0;
    for (std::size_t i = 18; i < 28; ++i) {
        char c = fname[i];
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

// Walk the Decompresser over a contiguous chunk of one or more whole
// ZPAQ blocks. For non-JIDAC segments (or when `jb` is null) data is
// written straight to `out`. For JIDAC archives `jb` collects per-ID
// fragment bytes and the file's fragment-ID list; the caller assembles
// the final output by replaying refs through frags.
inline void decompress_chunk(const std::uint8_t* ptr,
                             std::size_t size,
                             BytesWriter& out,
                             bool verify,
                             JidacBuilder* jb) {
    BytesReader reader(ptr, size);
    libzpaq::Decompresser d;
    StringCollector fname, comment;
    char sha1out[21];

    d.setInput(&reader);
    while (d.findBlock()) {
        while (true) {
            fname.clear();
            if (!d.findFilename(&fname)) break;
            comment.clear();
            d.readComment(&comment);

            const std::string& f = fname.str();
            const bool is_jidac = f.size() == 28 && f.compare(0, 3, "jDC") == 0;
            const char type = is_jidac ? f[17] : '\0';

            if (is_jidac && type == 'd') {
                long start_id = parse_jidac_num(f);
                BytesWriter segbuf;
                libzpaq::SHA1 s2;
                if (verify) d.setSHA1(&s2);
                d.setOutput(&segbuf);
                while (d.decompress()) {}
                d.readSegmentEnd(sha1out);
                if (verify && sha1out[0] == 1) {
                    if (std::memcmp(s2.result(), sha1out + 1, 20) != 0) {
                        throw zpaq_internal::ZpaqError("SHA-1 checksum mismatch");
                    }
                }
                auto& sb = segbuf.buffer();
                const std::size_t footer = jidac_data_footer_size(
                    sb.data(), sb.size());
                if (footer == 0) continue;  // unrecognized layout - skip
                const std::uint32_t N = read_le32(sb.data() + sb.size() - 4);
                const std::uint8_t* sizes = sb.data() + sb.size() - footer;
                std::size_t off = 0;
                for (std::uint32_t i = 0; i < N; ++i) {
                    const std::uint32_t fsize = read_le32(sizes + i * 4);
                    if (off + fsize > sb.size() - footer) break;
                    if (jb && start_id >= 0) {
                        std::vector<std::uint8_t> frag(
                            sb.begin() + off,
                            sb.begin() + off + fsize);
                        jb->frags[static_cast<std::uint32_t>(start_id) + i] =
                            std::move(frag);
                    } else {
                        // No builder - fall back to storage-order concat.
                        out.buffer().insert(out.buffer().end(),
                            sb.begin() + off,
                            sb.begin() + off + fsize);
                    }
                    off += fsize;
                }
            } else if (is_jidac && type == 'i') {
                BytesWriter segbuf;
                d.setOutput(&segbuf);
                while (d.decompress()) {}
                d.readSegmentEnd(sha1out);
                if (!jb) continue;
                // 'i' payload is a concatenation of file entries:
                //   [date 8][filename null-terminated]
                //   [attr_len 4][attrs]
                //   [nfrags 4][frag_ids 4*nfrags]
                auto& sb = segbuf.buffer();
                std::size_t pos = 0;
                while (pos + 8 <= sb.size()) {
                    pos += 8;  // skip date
                    while (pos < sb.size() && sb[pos] != 0) ++pos;
                    if (pos >= sb.size()) break;
                    ++pos;  // skip null
                    if (pos + 4 > sb.size()) break;
                    std::uint32_t attrlen = read_le32(sb.data() + pos);
                    pos += 4 + attrlen;
                    if (pos + 4 > sb.size()) break;
                    std::uint32_t nfrags = read_le32(sb.data() + pos);
                    pos += 4;
                    if (pos + 4ull * nfrags > sb.size()) break;
                    for (std::uint32_t i = 0; i < nfrags; ++i) {
                        jb->refs.push_back(read_le32(sb.data() + pos));
                        pos += 4;
                    }
                    jb->saw_refs = true;
                }
            } else if (is_jidac) {
                // 'c' header / 'h' index / anything else - discard.
                d.setOutput(nullptr);
                while (d.decompress()) {}
                d.readSegmentEnd(sha1out);
            } else {
                // Non-JIDAC fast path: real data straight to output buffer.
                libzpaq::SHA1 sha1;
                if (verify) d.setSHA1(&sha1);
                d.setOutput(&out);
                while (d.decompress()) {}
                d.readSegmentEnd(sha1out);
                if (verify && sha1out[0] == 1) {
                    if (std::memcmp(sha1.result(), sha1out + 1, 20) != 0) {
                        throw zpaq_internal::ZpaqError("SHA-1 checksum mismatch");
                    }
                }
            }
        }
    }
}

py::bytes decompress_bytes(py::buffer data, bool verify, int threads) {
    py::buffer_info info = data.request();
    if (info.ndim != 1 || info.itemsize != 1) {
        throw py::value_error("data must be a 1-D bytes-like buffer");
    }
    const std::size_t size = static_cast<std::size_t>(info.size);
    const std::uint8_t* ptr = reinterpret_cast<const std::uint8_t*>(info.ptr);

    if (size == 0) {
        return py::bytes("", 0);
    }
    if (size < sizeof(kZpaqLocatorTag) ||
        std::memcmp(ptr, kZpaqLocatorTag, sizeof(kZpaqLocatorTag)) != 0) {
        throw zpaq_internal::ZpaqError(
            "input does not appear to be a ZPAQ stream "
            "(missing 13-byte locator tag at offset 0)");
    }

    // Locate every block boundary by scanning for the 13-byte locator tag.
    // The tag opens every libzpaq-produced block (whether streaming output
    // or JIDAC). It's 13 specific bytes - probability of a random run of 13
    // bytes inside the compressed bitstream matching all of them is ~5e-32,
    // so on real archives the scan finds exactly the block starts.
    std::vector<std::size_t> block_starts;
    block_starts.push_back(0);
    const std::size_t tag_len = sizeof(kZpaqLocatorTag);
    const std::uint8_t tag0 = kZpaqLocatorTag[0];
    for (std::size_t i = 1; i + tag_len <= size; ++i) {
        if (ptr[i] != tag0) continue;
        if (std::memcmp(ptr + i, kZpaqLocatorTag, tag_len) == 0) {
            block_starts.push_back(i);
        }
    }
    block_starts.push_back(size);  // sentinel end
    const int num_blocks = static_cast<int>(block_starts.size()) - 1;

    // Resolve worker count and clamp to block count.
    int t = threads;
    if (t <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        t = hw > 0 ? static_cast<int>(hw) : 1;
    }
    if (t < 1) t = 1;
    if (t > num_blocks) t = num_blocks;

    BytesWriter writer;
    JidacBuilder jb;

    if (t <= 1) {
        py::gil_scoped_release release;
        decompress_chunk(ptr, size, writer, verify, &jb);
    } else {
        // Multi-threaded: partition the blocks evenly across workers, give
        // each worker a contiguous byte range covering its blocks, run
        // independent Decompressers in parallel. Each worker has its own
        // JidacBuilder; we merge after.
        std::vector<std::vector<std::uint8_t>> outs(t);
        std::vector<JidacBuilder> local_jbs(t);
        std::vector<std::string> errors(t);
        {
            py::gil_scoped_release release;
            const int blocks_per = num_blocks / t;
            const int remainder = num_blocks % t;
            auto worker = [&](int wid) {
                try {
                    int my_start = wid * blocks_per + std::min(wid, remainder);
                    int my_count = blocks_per + (wid < remainder ? 1 : 0);
                    int my_end = my_start + my_count;
                    std::size_t off0 = block_starts[my_start];
                    std::size_t off1 = block_starts[my_end];
                    BytesWriter w;
                    decompress_chunk(ptr + off0, off1 - off0, w, verify,
                                     &local_jbs[wid]);
                    outs[wid] = std::move(w.buffer());
                } catch (const std::exception& e) {
                    errors[wid] = e.what();
                } catch (...) {
                    errors[wid] = "unknown error in decompress worker";
                }
            };
            std::vector<std::thread> workers;
            workers.reserve(t - 1);
            for (int i = 1; i < t; ++i) workers.emplace_back(worker, i);
            worker(0);
            for (auto& w : workers) w.join();
        }
        for (int i = 0; i < t; ++i) {
            if (!errors[i].empty()) {
                throw zpaq_internal::ZpaqError(errors[i]);
            }
        }

        // Merge worker JidacBuilders + concatenate non-JIDAC writer outputs.
        std::size_t non_jidac_total = 0;
        for (auto& o : outs) non_jidac_total += o.size();
        writer.buffer().reserve(non_jidac_total);
        for (auto& o : outs) {
            writer.buffer().insert(writer.buffer().end(), o.begin(), o.end());
        }
        for (auto& w_jb : local_jbs) {
            for (auto& kv : w_jb.frags) {
                jb.frags.emplace(kv.first, std::move(kv.second));
            }
            if (w_jb.saw_refs) {
                jb.refs.insert(jb.refs.end(),
                               w_jb.refs.begin(), w_jb.refs.end());
                jb.saw_refs = true;
            }
        }
    }

    // Assemble output. If 'i' segment(s) were seen we have the file's
    // fragment-ID sequence and replay it. Otherwise fall back: JIDAC
    // archive without 'i' -> emit fragments in ID order; pure stream
    // -> writer already has the data.
    if (jb.saw_refs) {
        py::gil_scoped_release release;
        std::size_t total = 0;
        for (std::uint32_t r : jb.refs) {
            auto it = jb.frags.find(r);
            if (it == jb.frags.end()) {
                throw zpaq_internal::ZpaqError(
                    "JIDAC archive references unknown fragment id");
            }
            total += it->second.size();
        }
        std::vector<std::uint8_t> assembled;
        assembled.reserve(total);
        for (std::uint32_t r : jb.refs) {
            const auto& frag = jb.frags[r];
            assembled.insert(assembled.end(), frag.begin(), frag.end());
        }
        return py::bytes(reinterpret_cast<const char*>(assembled.data()),
                         assembled.size());
    }
    if (!jb.frags.empty()) {
        // JIDAC 'd' segments but no 'i' - emit fragments sorted by ID.
        py::gil_scoped_release release;
        std::vector<std::pair<std::uint32_t,
                              const std::vector<std::uint8_t>*>> sorted;
        sorted.reserve(jb.frags.size());
        std::size_t total = 0;
        for (auto& kv : jb.frags) {
            sorted.emplace_back(kv.first, &kv.second);
            total += kv.second.size();
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) {
                      return a.first < b.first;
                  });
        std::vector<std::uint8_t> assembled;
        assembled.reserve(total);
        for (auto& p : sorted) {
            assembled.insert(assembled.end(),
                             p.second->begin(), p.second->end());
        }
        return py::bytes(reinterpret_cast<const char*>(assembled.data()),
                         assembled.size());
    }
    // Non-JIDAC path - writer holds the answer.
    auto& v = writer.buffer();
    return py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
}

}  // namespace zpaq_internal

// libzpaq requires the host application to provide this error handler.
// We translate libzpaq errors into a C++ exception that pybind11 maps to
// the Python-level zpaq.Error class.
void libzpaq::error(const char* msg) {
    throw zpaq_internal::ZpaqError(msg ? msg : "unknown libzpaq error");
}

PYBIND11_MODULE(_zpaq, m) {
    m.doc() = "Low-level pybind11 binding for libzpaq. Use the zpaq "
              "package's public API rather than this module directly.";

    py::register_exception<zpaq_internal::ZpaqError>(m, "Error",
                                                       PyExc_RuntimeError);

    m.def("compress", &zpaq_internal::compress_dispatch,
          py::arg("data"),
          py::arg("level") = 5,
          py::arg("threads") = 0,
          py::arg("hints") = true,
          py::arg("verify") = false,
          py::arg("method") = py::none(),
          py::arg("dedup") = false,
          R"(Compress a bytes-like object using ZPAQ. Returns bytes.

level: 0..5 - 0 stores without compression, 5 is the strongest.
threads: number of worker threads. 0 (default) auto-detects the host's
  hardware concurrency and caps it by input size (64KB minimum chunk
  per worker), so small inputs stay single-threaded and big inputs
  use all cores. Set to 1 explicitly for deterministic / single-block
  output - that gives ~0.5-3 percentage points better compression
  ratio at the cost of throughput. Any positive integer pins the
  thread count to exactly that value (still clamped by input size).
hints: if True, pre-scan the input for text/exe signatures and order-1
  redundancy and pass them to libzpaq via the method string, matching
  what the zpaq CLI does. Default False. On pure text data it slightly
  hurts the ratio; on mixed/binary content it can help. Negligible
  speed impact either way.
verify: if True, libzpaq computes and stores a SHA-1 checksum per
  segment. Default False for max speed. Set True if you want
  decompress(..., verify=True) to detect corruption. zpaq.exe also
  verifies these on extract; turning verify off means zpaq.exe won't
  catch corruption either.
method: optional raw libzpaq method string override (e.g. "x4,4,1" for
  custom predictor specs). When set, level/hints are ignored. See
  libzpaq.h for the format; intended for power users.
dedup: if True, emit a JIDAC-format archive with fragment-level
  deduplication. Input is content-defined-chunked into ~64KB fragments
  via rolling hash; identical fragments are stored once. The output
  works with both this package's decompress() and the official zpaq.exe
  CLI. Improves ratio on repetitive content (logs, large text corpora,
  similar binaries). Currently ignores threads (single-threaded
  JIDAC encode) and verify; expect more flexibility in a later release.)");

    m.def("decompress", &zpaq_internal::decompress_bytes,
          py::arg("data"),
          py::arg("verify") = false,
          py::arg("threads") = 0,
          R"(Decompress a ZPAQ-compressed bytes-like object. Returns bytes.

verify: if True, recompute the SHA-1 checksum of each segment and
  compare against the one stored in the archive. Raises zpaq.Error on
  mismatch. Default False for speed. Only meaningful if the archive
  was created with verify=True (or by zpaq.exe, which defaults on).
threads: number of worker threads. 0 (default) auto-detects the host's
  hardware concurrency and caps it by the number of independent ZPAQ
  blocks in the input. 1 forces single-threaded. Multi-block archives
  benefit; archives with a single block (e.g. small files compressed
  at threads=1) are forced to single-threaded regardless.)");
}

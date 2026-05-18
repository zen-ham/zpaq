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
#include <future>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
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

py::bytes decompress_bytes(py::buffer data, bool verify) {
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

    BytesReader reader(ptr, size);
    BytesWriter writer;

    // Peek at the first segment's filename. If it isn't a JIDAC name we
    // can take a much faster path: stream every block's output straight
    // into `writer` without per-segment buffering, JIDAC filtering, or
    // footer stripping. The decompressed bytes are the file contents
    // verbatim. This is the common case for any data produced by our own
    // compress() (or by any libzpaq::compress() user that didn't add
    // explicit segment filenames).
    {
        py::gil_scoped_release release;

        libzpaq::Decompresser d;
        StringCollector fname, comment;
        char sha1out[21];

        d.setInput(&reader);
        if (!d.findBlock()) {
            // No blocks - well-formed empty stream. Return empty.
            return py::bytes(reinterpret_cast<const char*>(writer.buffer().data()),
                             writer.buffer().size());
        }
        if (!d.findFilename(&fname)) {
            // Block with no segments - nothing to extract.
            return py::bytes(reinterpret_cast<const char*>(writer.buffer().data()),
                             writer.buffer().size());
        }
        d.readComment(&comment);

        const bool fast_path = !is_jidac_segment(fname.str());

        if (fast_path) {
            // Non-JIDAC stream: every segment is real file data, no
            // metadata to filter, no footer to trim. Write straight to
            // the output buffer. Repeat for all remaining segments and
            // blocks - we already consumed the first segment's filename
            // and comment, so we just need to decompress its body.
            libzpaq::SHA1 sha1;
            if (verify) d.setSHA1(&sha1);
            d.setOutput(&writer);
            while (d.decompress()) {}
            d.readSegmentEnd(sha1out);
            if (verify && sha1out[0] == 1) {
                if (std::memcmp(sha1.result(), sha1out + 1, 20) != 0) {
                    throw zpaq_internal::ZpaqError("SHA-1 checksum mismatch");
                }
            }
            // Subsequent segments in the first block.
            while (true) {
                fname.clear();
                if (!d.findFilename(&fname)) break;
                comment.clear();
                d.readComment(&comment);
                libzpaq::SHA1 s2;
                if (verify) d.setSHA1(&s2);
                d.setOutput(&writer);
                while (d.decompress()) {}
                d.readSegmentEnd(sha1out);
                if (verify && sha1out[0] == 1) {
                    if (std::memcmp(s2.result(), sha1out + 1, 20) != 0) {
                        throw zpaq_internal::ZpaqError("SHA-1 checksum mismatch");
                    }
                }
            }
            // Subsequent blocks.
            while (d.findBlock()) {
                while (true) {
                    fname.clear();
                    if (!d.findFilename(&fname)) break;
                    comment.clear();
                    d.readComment(&comment);
                    libzpaq::SHA1 s2;
                    if (verify) d.setSHA1(&s2);
                    d.setOutput(&writer);
                    while (d.decompress()) {}
                    d.readSegmentEnd(sha1out);
                    if (verify && sha1out[0] == 1) {
                        if (std::memcmp(s2.result(), sha1out + 1, 20) != 0) {
                            throw zpaq_internal::ZpaqError("SHA-1 checksum mismatch");
                        }
                    }
                }
            }
        } else {
            // JIDAC archive: filter metadata segments, strip data-segment
            // footers, only keep actual file payload. This costs a per-
            // segment intermediate buffer but is unavoidable for the
            // journaling format.
            auto process_segment = [&](const std::string& seg_fname) {
                if (is_jidac_metadata_segment(seg_fname)) {
                    d.setOutput(nullptr);
                    while (d.decompress()) {}
                    d.readSegmentEnd(sha1out);
                } else {
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
                    auto& seg = segbuf.buffer();
                    const std::size_t trim = jidac_data_footer_size(
                        seg.data(), seg.size());
                    const std::size_t keep = seg.size() - trim;
                    if (keep > 0) {
                        writer.buffer().insert(
                            writer.buffer().end(),
                            seg.begin(),
                            seg.begin() + static_cast<std::ptrdiff_t>(keep));
                    }
                }
            };
            // First segment (already have filename + comment in hand).
            process_segment(fname.str());
            // Remaining segments in first block.
            while (true) {
                fname.clear();
                if (!d.findFilename(&fname)) break;
                comment.clear();
                d.readComment(&comment);
                process_segment(fname.str());
            }
            // Remaining blocks.
            while (d.findBlock()) {
                while (true) {
                    fname.clear();
                    if (!d.findFilename(&fname)) break;
                    comment.clear();
                    d.readComment(&comment);
                    process_segment(fname.str());
                }
            }
        }
    }

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

    m.def("compress", &zpaq_internal::compress_bytes,
          py::arg("data"),
          py::arg("level") = 5,
          py::arg("threads") = 1,
          py::arg("hints") = false,
          py::arg("verify") = false,
          py::arg("method") = py::none(),
          R"(Compress a bytes-like object using ZPAQ. Returns bytes.

level: 0..5 - 0 stores without compression, 5 is the strongest.
threads: number of worker threads. 1 (default) is single-threaded;
  >1 splits the input across N threads using compressBlock. 0 picks
  the host's hardware concurrency. Inputs smaller than 64KB*threads
  are forced to single-thread regardless of this value.
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
  libzpaq.h for the format; intended for power users.)");

    m.def("decompress", &zpaq_internal::decompress_bytes,
          py::arg("data"),
          py::arg("verify") = false,
          R"(Decompress a ZPAQ-compressed bytes-like object. Returns bytes.

verify: if True, recompute the SHA-1 checksum of each segment and
  compare against the one stored in the archive. Raises zpaq.Error on
  mismatch. Default False for speed. Only meaningful if the archive
  was created with verify=True (or by zpaq.exe, which defaults on).)");
}

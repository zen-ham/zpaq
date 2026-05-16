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
#include <string>
#include <stdexcept>
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

    // Override block read for speed; default loops over get() one byte at a time.
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
// Returns true if the segment name is a JIDAC metadata segment.
inline bool is_jidac_metadata_segment(const std::string& fname) {
    if (fname.size() < 18) return false;
    if (fname.compare(0, 3, "jDC") != 0) return false;
    const char t = fname[17];
    return t == 'c' || t == 'h' || t == 'i';
}

// JIDAC 'd' segments contain concatenated file fragments followed by a
// trailing footer describing them:
//   [frag_1 data][frag_2 data]...[frag_N data]
//   [4-byte LE size_1]...[4-byte LE size_N]
//   [4-byte LE zero sentinel] [4-byte LE N]
// The zero sentinel is "omit first frag ID to make block movable" in zpaq.cpp.
// puti() writes low-order byte first (little-endian).
//
// We detect the footer by reading N from the last 4 bytes, verifying the
// zero sentinel directly before it, then checking that the N preceding
// 4-byte fragment-size values sum exactly to the remaining payload size.
// The "sum matches data length" constraint is strong enough that false
// positives on non-JIDAC data are vanishingly rare.
//
// Returns the number of trailing bytes to strip, or 0 if the payload is
// not a JIDAC data segment (in which case the segment is left untouched).
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
    if (N == 0 || N > (1u << 20)) return 0;  // 1M fragments per segment ceiling

    const std::size_t footer = static_cast<std::size_t>(N) * 4 + 8;
    if (footer >= size) return 0;

    // Zero sentinel immediately precedes the count.
    if (read_le32(data + size - 8) != 0) return 0;

    // Sum the N fragment sizes and confirm they account for the payload
    // bytes that come before the footer.
    const std::uint8_t* sizes = data + size - footer;
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < N; ++i) {
        sum += read_le32(sizes + i * 4);
        if (sum > size) return 0;
    }
    if (sum != size - footer) return 0;

    return footer;
}

py::bytes compress_bytes(py::buffer data, int level) {
    if (level < 0 || level > 5) {
        throw py::value_error("level must be in 0..5");
    }
    py::buffer_info info = data.request();
    if (info.ndim != 1 || info.itemsize != 1) {
        throw py::value_error("data must be a 1-D bytes-like buffer");
    }

    BytesReader reader(reinterpret_cast<const std::uint8_t*>(info.ptr),
                       static_cast<std::size_t>(info.size));
    BytesWriter writer;

    const char method[2] = {static_cast<char>('0' + level), '\0'};

    {
        py::gil_scoped_release release;
        try {
            libzpaq::compress(&reader, &writer, method);
        } catch (const std::exception&) {
            // Re-acquire GIL before re-raising so pybind11 can translate.
            throw;
        }
    }

    auto& v = writer.buffer();
    return py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
}

// The 13-byte locator tag that libzpaq's compress() writes at the start of
// every block. libzpaq::decompress() will silently emit nothing if it can't
// find any blocks in the input, which makes our public decompress() unable
// to distinguish "ZPAQ stream encoding zero bytes" from "random garbage".
// We reject inputs that obviously aren't ZPAQ streams up-front so callers
// get a real exception instead of a confusing empty result.
static const unsigned char kZpaqLocatorTag[13] = {
    0x37, 0x6B, 0x53, 0x74, 0xA0, 0x31, 0x83, 0xD3,
    0x8C, 0xB2, 0x28, 0xB0, 0xD3,
};

py::bytes decompress_bytes(py::buffer data) {
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

    // We use the libzpaq::Decompresser low-level API instead of the
    // convenience free-function libzpaq::decompress(). That lets us inspect
    // each segment's filename: if it looks like a JIDAC archive metadata
    // segment (block index / hash index / directory entry), we discard its
    // decompressed bytes instead of concatenating them onto the output.
    // The result: a `zpaq a` archive decompresses to byte-exact file content.
    {
        py::gil_scoped_release release;

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

                if (is_jidac_metadata_segment(fname.str())) {
                    // Pure metadata: discard the segment's decompressed bytes.
                    d.setOutput(nullptr);
                    while (d.decompress()) {}
                    d.readSegmentEnd(sha1out);
                } else {
                    // Either our own streaming segment OR a JIDAC 'd' segment.
                    // Decompress into a temp buffer, strip the JIDAC 'd' header
                    // if one is present, then append actual file bytes to the
                    // accumulating output.
                    BytesWriter segbuf;
                    d.setOutput(&segbuf);
                    while (d.decompress()) {}
                    d.readSegmentEnd(sha1out);

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
          py::arg("data"), py::arg("level") = 5,
          "Compress a bytes-like object using ZPAQ. Returns bytes.");

    m.def("decompress", &zpaq_internal::decompress_bytes,
          py::arg("data"),
          "Decompress a ZPAQ-compressed bytes-like object. Returns bytes.");
}

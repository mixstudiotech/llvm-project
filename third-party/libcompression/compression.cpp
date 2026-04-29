//===-- compression.cpp -----------------------------------------*- C++ -*-===//
//
// Portable implementation of Apple's <compression.h> API surface.
//
// On Apple platforms /usr/lib/libcompression.dylib provides this. Elsewhere we
// provide our own implementation backed by:
//
//   * lz4    -> third-party/libcompression/lz4
//   * lzfse  -> third-party/libcompression/lzfse
//   * zlib   -> third-party/libcompression/zlib   (miniz, raw DEFLATE per RFC 1951)
//   * lzma / brotli / lzbitmap -> not provided, the matching algorithm IDs
//     return failure from the buffer API and ERROR from the stream API.
//
// The COMPRESSION_LZ4 buffer API uses Apple's framed LZ4 stream format
// documented in <compression.h>: bv41/bv4-/bv4$ blocks, little-endian sizes.
// COMPRESSION_LZ4_RAW uses the bare LZ4 block format (compatible with
// upstream lz4 LZ4_compress_default / LZ4_decompress_safe).
//
//===----------------------------------------------------------------------===//

#include "compression.h"

extern "C" {
#include "lz4/lz4.h"
#include "lzfse/lzfse.h"
#include "zlib/miniz.h"
}

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {

// LZ4 framed format magic words (little-endian on the wire).
constexpr uint32_t kLZ4MagicCompressed   = 0x31347662u; // "bv41"
constexpr uint32_t kLZ4MagicUncompressed = 0x2d347662u; // "bv4-"
constexpr uint32_t kLZ4MagicEndOfStream  = 0x24347662u; // "bv4$"

// Minimum bytes needed to wrap *any* LZ4 framed payload:
//   12 bytes for the bv41 (or 8 for bv4-) header + 4 bytes for bv4$ trailer.
constexpr size_t kLZ4FrameMinOverhead = 8 + 4;

inline uint32_t LoadLE32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline void StoreLE32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

// ---- Buffer API: per-algorithm helpers -------------------------------------

size_t EncodeLZ4Raw(uint8_t *dst, size_t dst_size,
                    const uint8_t *src, size_t src_size) {
  if (src_size > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) return 0;
  if (dst_size > static_cast<size_t>(INT_MAX)) dst_size = INT_MAX;
  int n = LZ4_compress_default(reinterpret_cast<const char *>(src),
                               reinterpret_cast<char *>(dst),
                               static_cast<int>(src_size),
                               static_cast<int>(dst_size));
  return n > 0 ? static_cast<size_t>(n) : 0;
}

size_t DecodeLZ4Raw(uint8_t *dst, size_t dst_size,
                    const uint8_t *src, size_t src_size) {
  if (src_size > static_cast<size_t>(INT_MAX)) return 0;
  if (dst_size > static_cast<size_t>(INT_MAX)) dst_size = INT_MAX;
  int n = LZ4_decompress_safe(reinterpret_cast<const char *>(src),
                              reinterpret_cast<char *>(dst),
                              static_cast<int>(src_size),
                              static_cast<int>(dst_size));
  return n > 0 ? static_cast<size_t>(n) : 0;
}

size_t EncodeLZ4Framed(uint8_t *dst, size_t dst_size,
                       const uint8_t *src, size_t src_size) {
  if (src_size > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) return 0;
  if (src_size > 0xFFFFFFFFu) return 0;

  // Try a compressed bv41 block first.
  if (dst_size >= 12u + 4u) {
    size_t cap = dst_size - 12u - 4u;
    if (cap > static_cast<size_t>(INT_MAX)) cap = INT_MAX;
    int n = LZ4_compress_default(reinterpret_cast<const char *>(src),
                                 reinterpret_cast<char *>(dst) + 12,
                                 static_cast<int>(src_size),
                                 static_cast<int>(cap));
    if (n > 0 && static_cast<size_t>(n) < src_size) {
      StoreLE32(dst + 0, kLZ4MagicCompressed);
      StoreLE32(dst + 4, static_cast<uint32_t>(src_size));
      StoreLE32(dst + 8, static_cast<uint32_t>(n));
      StoreLE32(dst + 12 + n, kLZ4MagicEndOfStream);
      return 12u + static_cast<size_t>(n) + 4u;
    }
  }

  // Fall back to a literal bv4- block.
  if (dst_size < 8u + src_size + 4u) return 0;
  StoreLE32(dst + 0, kLZ4MagicUncompressed);
  StoreLE32(dst + 4, static_cast<uint32_t>(src_size));
  if (src_size) std::memcpy(dst + 8, src, src_size);
  StoreLE32(dst + 8 + src_size, kLZ4MagicEndOfStream);
  return 8u + src_size + 4u;
}

size_t DecodeLZ4Framed(uint8_t *dst, size_t dst_size,
                       const uint8_t *src, size_t src_size) {
  size_t in = 0, out = 0;
  while (in + 4 <= src_size) {
    uint32_t magic = LoadLE32(src + in);
    if (magic == kLZ4MagicEndOfStream) {
      return out;
    }
    if (magic == kLZ4MagicCompressed) {
      if (in + 12 > src_size) return 0;
      uint32_t decoded = LoadLE32(src + in + 4);
      uint32_t encoded = LoadLE32(src + in + 8);
      if (in + 12u + encoded > src_size) return 0;
      if (out + decoded > dst_size) return dst_size; // truncate per Apple spec
      int n = LZ4_decompress_safe(
          reinterpret_cast<const char *>(src + in + 12),
          reinterpret_cast<char *>(dst + out),
          static_cast<int>(encoded),
          static_cast<int>(dst_size - out));
      if (n < 0 || static_cast<uint32_t>(n) != decoded) return 0;
      in += 12u + encoded;
      out += decoded;
    } else if (magic == kLZ4MagicUncompressed) {
      if (in + 8 > src_size) return 0;
      uint32_t plain = LoadLE32(src + in + 4);
      if (in + 8u + plain > src_size) return 0;
      if (out + plain > dst_size) return dst_size;
      if (plain) std::memcpy(dst + out, src + in + 8, plain);
      in += 8u + plain;
      out += plain;
    } else {
      return 0;
    }
  }
  // Truncated stream (no bv4$ marker).
  return 0;
}

size_t EncodeZlib(uint8_t *dst, size_t dst_size,
                  const uint8_t *src, size_t src_size) {
  // Apple's COMPRESSION_ZLIB is raw DEFLATE (RFC 1951) — no zlib wrapper.
  // window_bits is negated to request raw deflate output.
  mz_stream s;
  std::memset(&s, 0, sizeof(s));
  if (mz_deflateInit2(&s, 5, MZ_DEFLATED, -15, 8, MZ_DEFAULT_STRATEGY) != MZ_OK)
    return 0;

  size_t produced = 0;
  size_t in_left  = src_size;
  size_t out_left = dst_size;
  const uint8_t *in_ptr  = src;
  uint8_t       *out_ptr = dst;

  for (;;) {
    unsigned chunk_in  = static_cast<unsigned>(std::min<size_t>(in_left,  UINT_MAX));
    unsigned chunk_out = static_cast<unsigned>(std::min<size_t>(out_left, UINT_MAX));
    s.next_in   = in_ptr;
    s.avail_in  = chunk_in;
    s.next_out  = out_ptr;
    s.avail_out = chunk_out;

    int flush = (in_left == chunk_in) ? MZ_FINISH : MZ_NO_FLUSH;
    int r = mz_deflate(&s, flush);

    size_t consumed_in  = chunk_in  - s.avail_in;
    size_t consumed_out = chunk_out - s.avail_out;
    in_ptr   += consumed_in;
    in_left  -= consumed_in;
    out_ptr  += consumed_out;
    out_left -= consumed_out;
    produced += consumed_out;

    if (r == MZ_STREAM_END) break;
    if (r != MZ_OK) { produced = 0; break; }
    if (out_left == 0) { produced = 0; break; } // would not fit
  }

  mz_deflateEnd(&s);
  return produced;
}

size_t DecodeZlib(uint8_t *dst, size_t dst_size,
                  const uint8_t *src, size_t src_size) {
  mz_stream s;
  std::memset(&s, 0, sizeof(s));
  if (mz_inflateInit2(&s, -15) != MZ_OK)
    return 0;

  size_t produced = 0;
  size_t in_left  = src_size;
  size_t out_left = dst_size;
  const uint8_t *in_ptr  = src;
  uint8_t       *out_ptr = dst;
  bool truncated = false;

  for (;;) {
    unsigned chunk_in  = static_cast<unsigned>(std::min<size_t>(in_left,  UINT_MAX));
    unsigned chunk_out = static_cast<unsigned>(std::min<size_t>(out_left, UINT_MAX));
    s.next_in   = in_ptr;
    s.avail_in  = chunk_in;
    s.next_out  = out_ptr;
    s.avail_out = chunk_out;

    int flush = (in_left == chunk_in) ? MZ_FINISH : MZ_NO_FLUSH;
    int r = mz_inflate(&s, flush);

    size_t consumed_in  = chunk_in  - s.avail_in;
    size_t consumed_out = chunk_out - s.avail_out;
    in_ptr   += consumed_in;
    in_left  -= consumed_in;
    out_ptr  += consumed_out;
    out_left -= consumed_out;
    produced += consumed_out;

    if (r == MZ_STREAM_END) break;
    if (r == MZ_BUF_ERROR && out_left == 0) { truncated = true; break; }
    if (r != MZ_OK) { produced = 0; break; }
    if (out_left == 0) { truncated = true; break; }
  }

  mz_inflateEnd(&s);
  // Per Apple's contract: if the decompressed output didn't fit, fill the
  // destination buffer and return its size.
  if (truncated) return dst_size;
  return produced;
}

// ---- Stream API state ------------------------------------------------------

struct StreamState {
  compression_algorithm algo;
  compression_stream_operation op;

  // Used only when algo == COMPRESSION_ZLIB.
  mz_stream zstream;
  bool zstream_initialized = false;

  // Used for non-streaming algorithms: accumulate input until FINALIZE,
  // produce all output up-front, then drain it across subsequent calls.
  std::vector<uint8_t> in_buf;
  std::vector<uint8_t> out_buf;
  size_t out_pos = 0;
  bool finished = false;
};

bool IsBufferedAlgo(compression_algorithm a) {
  return a == COMPRESSION_LZ4 || a == COMPRESSION_LZ4_RAW ||
         a == COMPRESSION_LZFSE;
}

}  // namespace

// =============================================================================
// Buffer API
// =============================================================================

extern "C" size_t
compression_encode_scratch_buffer_size(compression_algorithm /*algorithm*/) {
  // Our routines manage their own scratch internally.
  return 0;
}

extern "C" size_t
compression_decode_scratch_buffer_size(compression_algorithm /*algorithm*/) {
  return 0;
}

extern "C" size_t
compression_encode_buffer(uint8_t *dst, size_t dst_size,
                          const uint8_t *src, size_t src_size,
                          void * /*scratch*/, compression_algorithm algo) {
  if (!dst || (!src && src_size)) return 0;
  switch (algo) {
    case COMPRESSION_LZ4_RAW:
      return EncodeLZ4Raw(dst, dst_size, src, src_size);
    case COMPRESSION_LZ4:
      return EncodeLZ4Framed(dst, dst_size, src, src_size);
    case COMPRESSION_LZFSE:
      return lzfse_encode_buffer(dst, dst_size, src, src_size, nullptr);
    case COMPRESSION_ZLIB:
      return EncodeZlib(dst, dst_size, src, src_size);
    case COMPRESSION_LZMA:
    case COMPRESSION_BROTLI:
    case COMPRESSION_LZBITMAP:
      return 0; // not implemented
  }
  return 0;
}

extern "C" size_t
compression_decode_buffer(uint8_t *dst, size_t dst_size,
                          const uint8_t *src, size_t src_size,
                          void * /*scratch*/, compression_algorithm algo) {
  if (!dst || (!src && src_size)) return 0;
  switch (algo) {
    case COMPRESSION_LZ4_RAW:
      return DecodeLZ4Raw(dst, dst_size, src, src_size);
    case COMPRESSION_LZ4:
      return DecodeLZ4Framed(dst, dst_size, src, src_size);
    case COMPRESSION_LZFSE:
      return lzfse_decode_buffer(dst, dst_size, src, src_size, nullptr);
    case COMPRESSION_ZLIB:
      return DecodeZlib(dst, dst_size, src, src_size);
    case COMPRESSION_LZMA:
    case COMPRESSION_BROTLI:
    case COMPRESSION_LZBITMAP:
      return 0;
  }
  return 0;
}

// =============================================================================
// Stream API
// =============================================================================

extern "C" compression_status
compression_stream_init(compression_stream *stream,
                        compression_stream_operation op,
                        compression_algorithm algo) {
  if (!stream) return COMPRESSION_STATUS_ERROR;
  if (op != COMPRESSION_STREAM_ENCODE && op != COMPRESSION_STREAM_DECODE)
    return COMPRESSION_STATUS_ERROR;

  StreamState *s = new (std::nothrow) StreamState();
  if (!s) return COMPRESSION_STATUS_ERROR;
  s->algo = algo;
  s->op   = op;

  if (algo == COMPRESSION_ZLIB) {
    std::memset(&s->zstream, 0, sizeof(s->zstream));
    int r = (op == COMPRESSION_STREAM_ENCODE)
              ? mz_deflateInit2(&s->zstream, 5, MZ_DEFLATED, -15, 8,
                                MZ_DEFAULT_STRATEGY)
              : mz_inflateInit2(&s->zstream, -15);
    if (r != MZ_OK) { delete s; return COMPRESSION_STATUS_ERROR; }
    s->zstream_initialized = true;
  } else if (!IsBufferedAlgo(algo)) {
    delete s;
    return COMPRESSION_STATUS_ERROR;
  }

  stream->dst_ptr  = nullptr;
  stream->dst_size = 0;
  stream->src_ptr  = nullptr;
  stream->src_size = 0;
  stream->state    = s;
  return COMPRESSION_STATUS_OK;
}

extern "C" compression_status
compression_stream_destroy(compression_stream *stream) {
  if (!stream || !stream->state) return COMPRESSION_STATUS_ERROR;
  StreamState *s = static_cast<StreamState *>(stream->state);
  if (s->zstream_initialized) {
    if (s->op == COMPRESSION_STREAM_ENCODE) mz_deflateEnd(&s->zstream);
    else                                    mz_inflateEnd(&s->zstream);
  }
  delete s;
  stream->state = nullptr;
  return COMPRESSION_STATUS_OK;
}

namespace {

compression_status ProcessZlibStream(compression_stream *stream,
                                     StreamState *s, bool finalize) {
  s->zstream.next_in   = stream->src_ptr;
  s->zstream.avail_in  = static_cast<unsigned>(
      std::min<size_t>(stream->src_size, UINT_MAX));
  s->zstream.next_out  = stream->dst_ptr;
  s->zstream.avail_out = static_cast<unsigned>(
      std::min<size_t>(stream->dst_size, UINT_MAX));

  int flush = finalize ? MZ_FINISH : MZ_NO_FLUSH;
  int r = (s->op == COMPRESSION_STREAM_ENCODE)
              ? mz_deflate(&s->zstream, flush)
              : mz_inflate(&s->zstream, flush);

  size_t consumed_in  =
      static_cast<size_t>(stream->src_size > UINT_MAX ? UINT_MAX : stream->src_size)
      - s->zstream.avail_in;
  size_t consumed_out =
      static_cast<size_t>(stream->dst_size > UINT_MAX ? UINT_MAX : stream->dst_size)
      - s->zstream.avail_out;

  stream->src_ptr  += consumed_in;
  stream->src_size -= consumed_in;
  stream->dst_ptr  += consumed_out;
  stream->dst_size -= consumed_out;

  if (r == MZ_STREAM_END) return COMPRESSION_STATUS_END;
  if (r == MZ_OK || r == MZ_BUF_ERROR) return COMPRESSION_STATUS_OK;
  return COMPRESSION_STATUS_ERROR;
}

bool RunBufferedEncode(StreamState *s) {
  size_t worst = 0;
  switch (s->algo) {
    case COMPRESSION_LZ4_RAW:
      worst = static_cast<size_t>(LZ4_compressBound(
          static_cast<int>(std::min<size_t>(s->in_buf.size(), LZ4_MAX_INPUT_SIZE))));
      if (s->in_buf.size() > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) return false;
      break;
    case COMPRESSION_LZ4: {
      if (s->in_buf.size() > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) return false;
      worst = static_cast<size_t>(LZ4_compressBound(
                  static_cast<int>(s->in_buf.size()))) + kLZ4FrameMinOverhead;
      // Make sure the literal-fallback path also fits.
      worst = std::max<size_t>(worst, s->in_buf.size() + kLZ4FrameMinOverhead);
      break;
    }
    case COMPRESSION_LZFSE:
      // lzfse has no compressBound. Output may exceed input slightly for
      // incompressible data. Pad generously.
      worst = s->in_buf.size() + 1024 + (s->in_buf.size() >> 2);
      break;
    default:
      return false;
  }
  if (worst == 0) worst = 1;
  s->out_buf.assign(worst, 0);

  size_t produced = 0;
  switch (s->algo) {
    case COMPRESSION_LZ4_RAW:
      produced = EncodeLZ4Raw(s->out_buf.data(), s->out_buf.size(),
                              s->in_buf.data(),  s->in_buf.size());
      break;
    case COMPRESSION_LZ4:
      produced = EncodeLZ4Framed(s->out_buf.data(), s->out_buf.size(),
                                 s->in_buf.data(),  s->in_buf.size());
      break;
    case COMPRESSION_LZFSE:
      produced = lzfse_encode_buffer(s->out_buf.data(), s->out_buf.size(),
                                     s->in_buf.data(),  s->in_buf.size(),
                                     nullptr);
      break;
    default: break;
  }
  if (produced == 0 && !s->in_buf.empty()) return false;
  s->out_buf.resize(produced);
  return true;
}

bool RunBufferedDecode(StreamState *s) {
  // We don't know the decoded size up-front for LZ4_RAW / LZFSE.  Grow the
  // output buffer until decoding produces a value strictly smaller than the
  // capacity we offered.  Capped at a generous multiple to bound runtime.
  size_t cap = std::max<size_t>(s->in_buf.size() * 4, 4096);
  for (int tries = 0; tries < 24; ++tries) {
    s->out_buf.assign(cap, 0);
    size_t produced = 0;
    switch (s->algo) {
      case COMPRESSION_LZ4_RAW:
        produced = DecodeLZ4Raw(s->out_buf.data(), s->out_buf.size(),
                                s->in_buf.data(),  s->in_buf.size());
        break;
      case COMPRESSION_LZ4:
        produced = DecodeLZ4Framed(s->out_buf.data(), s->out_buf.size(),
                                   s->in_buf.data(),  s->in_buf.size());
        break;
      case COMPRESSION_LZFSE:
        produced = lzfse_decode_buffer(s->out_buf.data(), s->out_buf.size(),
                                       s->in_buf.data(),  s->in_buf.size(),
                                       nullptr);
        break;
      default: return false;
    }
    if (produced == 0 && !s->in_buf.empty()) return false;
    if (produced < cap) {
      s->out_buf.resize(produced);
      return true;
    }
    if (cap > (SIZE_MAX / 2)) return false;
    cap *= 2;
  }
  return false;
}

}  // namespace

extern "C" compression_status
compression_stream_process(compression_stream *stream, int flags) {
  if (!stream || !stream->state) return COMPRESSION_STATUS_ERROR;
  StreamState *s = static_cast<StreamState *>(stream->state);
  bool finalize = (flags & COMPRESSION_STREAM_FINALIZE) != 0;

  if (s->algo == COMPRESSION_ZLIB)
    return ProcessZlibStream(stream, s, finalize);

  // Buffered path (LZ4, LZ4_RAW, LZFSE): ingest available input first.
  if (stream->src_size) {
    s->in_buf.insert(s->in_buf.end(),
                     stream->src_ptr,
                     stream->src_ptr + stream->src_size);
    stream->src_ptr  += stream->src_size;
    stream->src_size  = 0;
  }

  // On finalize, do the one-shot encode/decode and stash the output.
  if (!s->finished && finalize) {
    bool ok = (s->op == COMPRESSION_STREAM_ENCODE) ? RunBufferedEncode(s)
                                                   : RunBufferedDecode(s);
    if (!ok) return COMPRESSION_STATUS_ERROR;
    s->finished = true;
  }

  // Drain accumulated output into the caller's destination buffer.
  if (s->finished) {
    size_t remaining = s->out_buf.size() - s->out_pos;
    size_t to_copy = std::min(remaining, stream->dst_size);
    if (to_copy) {
      std::memcpy(stream->dst_ptr, s->out_buf.data() + s->out_pos, to_copy);
      stream->dst_ptr  += to_copy;
      stream->dst_size -= to_copy;
      s->out_pos       += to_copy;
    }
    if (s->out_pos == s->out_buf.size()) return COMPRESSION_STATUS_END;
  }
  return COMPRESSION_STATUS_OK;
}

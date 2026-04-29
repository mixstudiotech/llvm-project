# Portable, vendored implementation of Apple's libcompression API.
#
# Backends:
#   * lz4    — Yann Collet, BSD-2-Clause (lz4/)
#   * lzfse  — Apple Inc.,  BSD-3-Clause (lzfse/)
#   * miniz  — Rich Geldreich, public-domain / MIT (zlib/)
#
# LZMA / Brotli / LZBitmap algorithm IDs are accepted by the API but their
# encode/decode entry points return failure (no implementation provided).

set(LIBCOMPRESSION_LZ4_SOURCES
  lz4/lz4.c
  lz4/lz4hc.c
  lz4/lz4frame.c
  lz4/lz4file.c
  lz4/xxhash.c
)

set(LIBCOMPRESSION_LZFSE_SOURCES
  lzfse/lzfse_decode.c
  lzfse/lzfse_decode_base.c
  lzfse/lzfse_encode.c
  lzfse/lzfse_encode_base.c
  lzfse/lzfse_fse.c
  lzfse/lzvn_decode_base.c
  lzfse/lzvn_encode_base.c
)

set(LIBCOMPRESSION_ZLIB_SOURCES
  zlib/miniz.c
  zlib/miniz_tinfl.c
  zlib/miniz_tdef.c
  # miniz_zip.c / miniz_common.c are intentionally omitted —
  # we only need deflate/inflate; the "common" code is header-only in 3.0.2.
)

add_library(libcompression STATIC
  compression.cpp
  ${LIBCOMPRESSION_LZ4_SOURCES}
  ${LIBCOMPRESSION_LZFSE_SOURCES}
  ${LIBCOMPRESSION_ZLIB_SOURCES}
)

# Consumers do `#include "compression.h"`; vendored backends live under
# subdirectories and are referenced via `lz4/lz4.h` etc. from compression.cpp.
target_include_directories(libcompression
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lz4
          ${CMAKE_CURRENT_SOURCE_DIR}/lzfse
          ${CMAKE_CURRENT_SOURCE_DIR}/zlib
)

# Strip miniz down to deflate/inflate only.
target_compile_definitions(libcompression PRIVATE
  MINIZ_NO_ARCHIVE_APIS
  MINIZ_NO_ARCHIVE_WRITING_APIS
  MINIZ_NO_STDIO
  MINIZ_NO_TIME
)

set_target_properties(libcompression PROPERTIES
  C_STANDARD   99
  CXX_STANDARD 14
  POSITION_INDEPENDENT_CODE ON
)

# Match the headers' nullability annotations on Apple/clang only.
if (NOT MSVC)
  target_compile_options(libcompression PRIVATE
    -Wno-unknown-pragmas
  )
endif()

# Provide a uniform alias so callers can use `compression::compression`
# regardless of where this directory is included from.
add_library(compression::compression ALIAS libcompression)

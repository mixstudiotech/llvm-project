# This file contains all the logic for running configure-time checks

include(CheckSymbolExists)
include(CheckIncludeFile)
include(CheckIncludeFiles)
include(CheckLibraryExists)

set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(ppoll poll.h HAVE_PPOLL)
check_symbol_exists(ptsname_r stdlib.h HAVE_PTSNAME_R)
set(CMAKE_REQUIRED_DEFINITIONS)
check_cxx_symbol_exists(accept4 "sys/socket.h" HAVE_ACCEPT4)

check_include_file(termios.h HAVE_TERMIOS_H)
check_include_files("sys/types.h;sys/event.h" HAVE_SYS_EVENT_H)

check_cxx_symbol_exists(process_vm_readv "sys/uio.h" HAVE_PROCESS_VM_READV)
check_cxx_symbol_exists(__NR_process_vm_readv "sys/syscall.h" HAVE_NR_PROCESS_VM_READV)

check_library_exists(compression compression_encode_buffer "" HAVE_LIBCOMPRESSION)

# When the system libcompression isn't available (i.e. anywhere outside Apple
# platforms) fall back to our portable vendored implementation under
# third-party/libcompression.  It provides LZ4 / LZ4_RAW / LZFSE / ZLIB; the
# LZMA path is stubbed (returns failure) and lldb's gdb-remote layer simply
# falls back to an uncompressed packet in that case.
set(LLDB_LIBCOMPRESSION_TARGET "")
if(HAVE_LIBCOMPRESSION)
  set(LLDB_LIBCOMPRESSION_TARGET compression)
else()
  set(_lldb_vendored_libcompression
      "${LLVM_MAIN_SRC_DIR}/../third-party/libcompression")
  if(EXISTS "${_lldb_vendored_libcompression}/CMakeLists.txt")
    if(NOT TARGET libcompression)
      add_subdirectory("${_lldb_vendored_libcompression}"
                       "${CMAKE_BINARY_DIR}/third-party/libcompression"
                       EXCLUDE_FROM_ALL)
    endif()
    set(HAVE_LIBCOMPRESSION 1)
    set(LLDB_LIBCOMPRESSION_TARGET libcompression)
  endif()
endif()

set(LLDB_INSTALL_LIBDIR_BASENAME "lib${LLDB_LIBDIR_SUFFIX}")

# These checks exist in LLVM's configuration, so I want to match the LLVM names
# so that the check isn't duplicated, but we translate them into the LLDB names
# so that I don't have to change all the uses at the moment.
set(LLDB_ENABLE_TERMIOS ${HAVE_TERMIOS_H})
if (UNIX)
  set(LLDB_ENABLE_POSIX ON)
else()
  set(LLDB_ENABLE_POSIX OFF)
endif()

if(NOT LLDB_CONFIG_HEADER_INPUT)
 set(LLDB_CONFIG_HEADER_INPUT ${LLDB_INCLUDE_ROOT}/lldb/Host/Config.h.cmake)
endif()

if(NOT LLDB_CONFIG_HEADER_OUTPUT)
 set(LLDB_CONFIG_HEADER_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/include/lldb/Host/Config.h)
endif()

# This should be done at the end
configure_file(
  ${LLDB_CONFIG_HEADER_INPUT}
  ${LLDB_CONFIG_HEADER_OUTPUT}
  )

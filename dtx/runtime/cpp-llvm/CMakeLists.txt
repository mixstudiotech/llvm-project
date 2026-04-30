add_library(dtxRuntime
  lib/AuxList.cpp
  lib/Connection.cpp
  lib/Fragment.cpp
  lib/KeyedArchiver.cpp
  lib/NSObject.cpp
  lib/Protocol.cpp
  lib/Server.cpp
  lib/TcpTransport.cpp
)

add_library(dtx::runtime ALIAS dtxRuntime)

target_include_directories(dtxRuntime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
  target_link_libraries(dtxRuntime PUBLIC ws2_32)
endif()

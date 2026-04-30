add_executable(dtx-codegen
  Main.cpp
)

target_include_directories(dtx-codegen
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(NOT TARGET dtxRuntime OR NOT TARGET dtx-codegen)
  message(STATUS "lldb-mixdev is disabled because dtxRuntime or dtx-codegen is not available")
  return()
endif()

set(YCODE_MIXDEVICE_ROOT
  "C:/Users/Station/Desktop/IrisBuild"
  CACHE PATH "Path to the IrisBuild checkout that provides mix_device.h and mix_device.dll")
set(YCODE_MIXDEVICE_INCLUDE_DIR
  "${YCODE_MIXDEVICE_ROOT}/tools/unreal/Plugin/Source/libmixdevice/include"
  CACHE PATH "Directory containing mix_device.h")
set(YCODE_MIXDEVICE_LIBRARY
  "${YCODE_MIXDEVICE_ROOT}/target/debug/mix_device.dll.lib"
  CACHE FILEPATH "mix_device import library")
set(YCODE_MIXDEVICE_RUNTIME_DLL
  "${YCODE_MIXDEVICE_ROOT}/target/debug/mix_device.dll"
  CACHE FILEPATH "mix_device runtime DLL")

set(YCODE_MIXDEV_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(YCODE_MIXDEV_GENERATED_HEADERS
  ${YCODE_MIXDEV_GENERATED_DIR}/DebugHostTypes.h.inc
  ${YCODE_MIXDEV_GENERATED_DIR}/DebugHostClient.h.inc
  ${YCODE_MIXDEV_GENERATED_DIR}/DebugHostService.h.inc
  ${YCODE_MIXDEV_GENERATED_DIR}/DebugHostServer.h.inc
)

add_custom_command(
  OUTPUT ${YCODE_MIXDEV_GENERATED_HEADERS}
  COMMAND dtx-codegen
    --input ${LLVM_MAIN_SRC_DIR}/../dtx/schema/DebugHostCore.td
    --lang cpp
    --out-dir ${YCODE_MIXDEV_GENERATED_DIR}
  DEPENDS dtx-codegen ${LLVM_MAIN_SRC_DIR}/../dtx/schema/DebugHostCore.td
  VERBATIM
)

add_custom_target(ycode-debughost-generated-headers
  DEPENDS ${YCODE_MIXDEV_GENERATED_HEADERS})

add_lldb_tool(ycode-debughost
  Main.cpp
  DebugHostServices.cpp
  LldbBackend.cpp
  MixDeviceBridge.cpp
  ${YCODE_MIXDEV_GENERATED_HEADERS}

  LINK_LIBS
    dtxRuntime
    liblldb

  LINK_COMPONENTS
    Support
)

set_target_properties(ycode-debughost PROPERTIES
  OUTPUT_NAME "YCode.DebugHost")

target_include_directories(ycode-debughost PRIVATE
  ${YCODE_MIXDEV_GENERATED_DIR}
)
add_dependencies(ycode-debughost ycode-debughost-generated-headers)

if(EXISTS "${YCODE_MIXDEVICE_INCLUDE_DIR}/mix_device.h" AND EXISTS "${YCODE_MIXDEVICE_LIBRARY}")
  target_compile_definitions(ycode-debughost PRIVATE YCODE_MIXDEV_HAS_MIX_DEVICE=1)
  target_include_directories(ycode-debughost PRIVATE ${YCODE_MIXDEVICE_INCLUDE_DIR})
  target_link_libraries(ycode-debughost PRIVATE ${YCODE_MIXDEVICE_LIBRARY})
  if(EXISTS "${YCODE_MIXDEVICE_RUNTIME_DLL}")
    add_custom_command(TARGET ycode-debughost POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${YCODE_MIXDEVICE_RUNTIME_DLL}
        $<TARGET_FILE_DIR:ycode-debughost>
      VERBATIM
    )
  endif()
else()
  message(STATUS "mix_device SDK was not found; YCode.DebugHost will build with device FFI disabled")
endif()

find_program(DOTNET_EXECUTABLE dotnet)
if(WIN32 AND DOTNET_EXECUTABLE)
  set(YCODE_DEBUGHOST_CSHARP_TEST_PROJECT
    ${LLVM_MAIN_SRC_DIR}/../dtx/test/csharp/YCodeDebugHostClientNet472/YCodeDebugHostClientNet472.csproj)
  set(YCODE_DEBUGHOST_CSHARP_TEST_GENERATED_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated-csharp-test)
  set(YCODE_DEBUGHOST_CSHARP_TEST_GENERATED
    ${YCODE_DEBUGHOST_CSHARP_TEST_GENERATED_DIR}/DebugHostTypes.g.cs
    ${YCODE_DEBUGHOST_CSHARP_TEST_GENERATED_DIR}/DebugHostClient.g.cs)
  set(YCODE_DEBUGHOST_CSHARP_TEST_OUTPUT_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/csharp-client-test-bin)

  add_custom_command(
    OUTPUT ${YCODE_DEBUGHOST_CSHARP_TEST_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${YCODE_DEBUGHOST_CSHARP_TEST_GENERATED_DIR}
    COMMAND $<TARGET_FILE:dtx-codegen>
      --input ${LLVM_MAIN_SRC_DIR}/../dtx/schema/DebugHostCore.td
      --lang csharp
      --out-dir ${YCODE_DEBUGHOST_CSHARP_TEST_GENERATED_DIR}
    DEPENDS dtx-codegen ${LLVM_MAIN_SRC_DIR}/../dtx/schema/DebugHostCore.td
    VERBATIM
  )

  add_custom_target(ycode-debughost-csharp-net472-generated
    DEPENDS ${YCODE_DEBUGHOST_CSHARP_TEST_GENERATED})

  add_custom_target(ycode-debughost-csharp-net472-build
    COMMAND ${DOTNET_EXECUTABLE} build ${YCODE_DEBUGHOST_CSHARP_TEST_PROJECT}
      --configuration Debug
      -p:GenerateDebugHostClient=false
      -p:GeneratedDir=${YCODE_DEBUGHOST_CSHARP_TEST_GENERATED_DIR}
      -p:BaseOutputPath=${YCODE_DEBUGHOST_CSHARP_TEST_OUTPUT_DIR}/
      -p:Platform=AnyCPU
      -p:PlatformTarget=AnyCPU
    DEPENDS ycode-debughost ycode-debughost-csharp-net472-generated
    USES_TERMINAL
    VERBATIM
  )

  add_custom_target(ycode-debughost-csharp-net472-test
    COMMAND ${YCODE_DEBUGHOST_CSHARP_TEST_OUTPUT_DIR}/Debug/net472/YCodeDebugHostClientNet472.exe
      $<TARGET_FILE:ycode-debughost>
    DEPENDS ycode-debughost-csharp-net472-build
    USES_TERMINAL
    VERBATIM
  )
else()
  message(STATUS "dotnet was not found; YCode.DebugHost C# net472 client test targets are disabled")
endif()

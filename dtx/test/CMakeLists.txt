add_executable(dtx-runtime-smoke
  RuntimeSmoke.cpp
)
target_link_libraries(dtx-runtime-smoke PRIVATE dtxRuntime)

add_test(NAME dtx-runtime-smoke COMMAND dtx-runtime-smoke)

add_test(
  NAME dtx-codegen-smoke
  COMMAND dtx-codegen
    --input ${DTX_MAIN_SRC_DIR}/schema/DebugHostCore.td
    --lang cpp
    --out-dir ${CMAKE_BINARY_DIR}/generated
)

add_test(
  NAME dtx-codegen-events-smoke
  COMMAND dtx-codegen
    --input ${DTX_MAIN_SRC_DIR}/schema/DebugHostEvents.td
    --lang cpp
    --out-dir ${CMAKE_BINARY_DIR}/generated-events
)

add_test(
  NAME dtx-codegen-csharp-smoke
  COMMAND dtx-codegen
    --input ${DTX_MAIN_SRC_DIR}/schema/DebugHostCore.td
    --lang csharp
    --out-dir ${CMAKE_BINARY_DIR}/generated-csharp-smoke
)

add_test(
  NAME dtx-codegen-kotlin-smoke
  COMMAND dtx-codegen
    --input ${DTX_MAIN_SRC_DIR}/schema/DebugHostCore.td
    --lang kotlin
    --out-dir ${CMAKE_BINARY_DIR}/generated-kotlin-smoke
)

set(DTX_GENERATED_BUILD_DIR ${CMAKE_BINARY_DIR}/generated-build)
set(DTX_GENERATED_HEADERS
  ${DTX_GENERATED_BUILD_DIR}/DebugHostTypes.h.inc
  ${DTX_GENERATED_BUILD_DIR}/DebugHostClient.h.inc
  ${DTX_GENERATED_BUILD_DIR}/DebugHostService.h.inc
  ${DTX_GENERATED_BUILD_DIR}/DebugHostServer.h.inc
)

add_custom_command(
  OUTPUT ${DTX_GENERATED_HEADERS}
  COMMAND dtx-codegen
    --input ${DTX_MAIN_SRC_DIR}/schema/DebugHostCore.td
    --lang cpp
    --out-dir ${DTX_GENERATED_BUILD_DIR}
  DEPENDS dtx-codegen ${DTX_MAIN_SRC_DIR}/schema/DebugHostCore.td
  VERBATIM
)

add_custom_target(dtx-generate-build-headers DEPENDS ${DTX_GENERATED_HEADERS})

add_executable(dtx-generated-smoke
  GeneratedSmoke.cpp
  ${DTX_GENERATED_HEADERS}
)
target_include_directories(dtx-generated-smoke PRIVATE ${DTX_GENERATED_BUILD_DIR})
target_link_libraries(dtx-generated-smoke PRIVATE dtxRuntime)
add_dependencies(dtx-generated-smoke dtx-generate-build-headers)

add_test(NAME dtx-generated-smoke COMMAND dtx-generated-smoke)

add_executable(dtx-debughost-tcp-smoke
  DebugHostTcpSmoke.cpp
  ${DTX_GENERATED_HEADERS}
)
target_include_directories(dtx-debughost-tcp-smoke PRIVATE ${DTX_GENERATED_BUILD_DIR})
target_link_libraries(dtx-debughost-tcp-smoke PRIVATE dtxRuntime)
add_dependencies(dtx-debughost-tcp-smoke dtx-generate-build-headers)

add_test(NAME dtx-debughost-tcp-smoke COMMAND dtx-debughost-tcp-smoke)

add_executable(dtx-debughost-tcp-server
  DebugHostTcpServer.cpp
  ${DTX_GENERATED_HEADERS}
)
target_include_directories(dtx-debughost-tcp-server PRIVATE ${DTX_GENERATED_BUILD_DIR})
target_link_libraries(dtx-debughost-tcp-server PRIVATE dtxRuntime)
add_dependencies(dtx-debughost-tcp-server dtx-generate-build-headers)

set(DTX_GENERATED_CSHARP_DIR ${CMAKE_BINARY_DIR}/generated-csharp)
set(DTX_GENERATED_CSHARP
  ${DTX_GENERATED_CSHARP_DIR}/DebugHostTypes.g.cs
  ${DTX_GENERATED_CSHARP_DIR}/DebugHostClient.g.cs
)

add_custom_command(
  OUTPUT ${DTX_GENERATED_CSHARP}
  COMMAND dtx-codegen
    --input ${DTX_MAIN_SRC_DIR}/schema/DebugHostCore.td
    --lang csharp
    --out-dir ${DTX_GENERATED_CSHARP_DIR}
  DEPENDS dtx-codegen ${DTX_MAIN_SRC_DIR}/schema/DebugHostCore.td
  VERBATIM
)

add_custom_target(dtx-generate-csharp DEPENDS ${DTX_GENERATED_CSHARP})

set(DTX_GENERATED_KOTLIN_DIR ${CMAKE_BINARY_DIR}/generated-kotlin)
set(DTX_GENERATED_KOTLIN
  ${DTX_GENERATED_KOTLIN_DIR}/DebugHostTypes.kt
  ${DTX_GENERATED_KOTLIN_DIR}/DebugHostClient.kt
)

add_custom_command(
  OUTPUT ${DTX_GENERATED_KOTLIN}
  COMMAND dtx-codegen
    --input ${DTX_MAIN_SRC_DIR}/schema/DebugHostCore.td
    --lang kotlin
    --out-dir ${DTX_GENERATED_KOTLIN_DIR}
  DEPENDS dtx-codegen ${DTX_MAIN_SRC_DIR}/schema/DebugHostCore.td
  VERBATIM
)

add_custom_target(dtx-generate-kotlin DEPENDS ${DTX_GENERATED_KOTLIN})

find_program(KOTLINC_EXE kotlinc)
find_program(JAVA_EXE java)
if(KOTLINC_EXE AND JAVA_EXE)
  file(GLOB_RECURSE DTX_KOTLIN_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${DTX_MAIN_SRC_DIR}/runtime/kotlin/src/main/kotlin/*.kt
  )
  set(DTX_KOTLIN_SMOKE_SOURCE
    ${DTX_MAIN_SRC_DIR}/test/kotlin/DtxKotlinClientSmoke/Main.kt)
  set(DTX_KOTLIN_SMOKE_JAR
    ${CMAKE_BINARY_DIR}/kotlin-bin/DtxKotlinClientSmoke.jar)

  add_custom_target(dtx-kotlin-client-smoke-build ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/kotlin-bin
    COMMAND ${KOTLINC_EXE}
      ${DTX_KOTLIN_RUNTIME_SOURCES}
      ${DTX_GENERATED_KOTLIN}
      ${DTX_KOTLIN_SMOKE_SOURCE}
      -include-runtime
      -d ${DTX_KOTLIN_SMOKE_JAR}
    DEPENDS dtx-generate-kotlin dtx-debughost-tcp-server
    VERBATIM
  )

  add_test(
    NAME dtx-kotlin-client-to-cpp-server
    COMMAND ${JAVA_EXE} -jar ${DTX_KOTLIN_SMOKE_JAR}
      $<TARGET_FILE:dtx-debughost-tcp-server>
  )
  set_tests_properties(dtx-kotlin-client-to-cpp-server PROPERTIES
    REQUIRED_FILES ${DTX_KOTLIN_SMOKE_JAR}
  )
else()
  message(STATUS "kotlinc or java was not found; Kotlin DTX client smoke test is disabled")
endif()

find_program(DOTNET_EXE dotnet)
if(DOTNET_EXE)
  set(DTX_CSHARP_SMOKE_PROJECT
    ${DTX_MAIN_SRC_DIR}/test/csharp/DtxCSharpClientSmoke/DtxCSharpClientSmoke.csproj)
  set(DTX_CSHARP_NET472_PROJECT
    ${DTX_MAIN_SRC_DIR}/test/csharp/DtxCSharpGeneratedNet472/DtxCSharpGeneratedNet472.csproj)
  set(DTX_CSHARP_OUTPUT_DIR ${CMAKE_BINARY_DIR}/csharp-bin/)
  set(DTX_CSHARP_NET472_OUTPUT_DIR ${CMAKE_BINARY_DIR}/csharp-net472-bin/)
  set(DTX_CSHARP_SMOKE_DLL
    ${DTX_CSHARP_OUTPUT_DIR}/Debug/net8.0/DtxCSharpClientSmoke.dll)

  add_custom_target(dtx-csharp-client-smoke-build ALL
    COMMAND ${DOTNET_EXE} build ${DTX_CSHARP_SMOKE_PROJECT}
      --configuration Debug
      -p:GeneratedDir=${DTX_GENERATED_CSHARP_DIR}
      -p:BaseOutputPath=${DTX_CSHARP_OUTPUT_DIR}
    DEPENDS dtx-generate-csharp dtx-debughost-tcp-server
    VERBATIM
  )

  add_custom_target(dtx-csharp-generated-net472-build ALL
    COMMAND ${DOTNET_EXE} build ${DTX_CSHARP_NET472_PROJECT}
      --configuration Debug
      -p:GeneratedDir=${DTX_GENERATED_CSHARP_DIR}
      -p:BaseOutputPath=${DTX_CSHARP_NET472_OUTPUT_DIR}
    DEPENDS dtx-generate-csharp
    VERBATIM
  )

  add_test(
    NAME dtx-csharp-client-to-cpp-server
    COMMAND ${DOTNET_EXE} ${DTX_CSHARP_SMOKE_DLL}
      $<TARGET_FILE:dtx-debughost-tcp-server>
  )
  set_tests_properties(dtx-csharp-client-to-cpp-server PROPERTIES
    REQUIRED_FILES ${DTX_CSHARP_SMOKE_DLL}
  )
else()
  message(STATUS "dotnet was not found; C# DTX client smoke test is disabled")
endif()

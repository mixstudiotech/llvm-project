# DTX

`dtx` is the DebugHost-to-IDE RPC layer used by the YCode debugger work.

Current implementation status:

- `runtime/cpp-llvm`: standalone C++17 runtime skeleton for DTX wire headers,
  payload headers, fragments, auxiliary arguments, NSObject-shaped values,
  object archiving, `Connection::call`, server-side request dispatch, and TCP
  transport.
- `runtime/csharp`: standalone .NET Framework 4.7.2 / .NET 8 client runtime for DTX headers,
  fragments, aux arguments, NSObject-shaped values, the current project-local
  object archive format, TCP transport, and `Connection.CallAsync`.
- `runtime/kotlin`: standalone JVM client runtime for the same protocol subset,
  with TCP transport and blocking `Connection.call`.
- `tools/dtx-codegen`: MVP schema generator that reads the project schema and
  emits C++ client/service/types include files, including server-side selector
  dispatch glue and a generated DebugHost server bundle. It also emits C#
  client/types `.g.cs` files and Kotlin client/types `.kt` files for IDE
  clients.
- `schema`: first DebugHost service definitions.

Build the standalone MVP:

```bat
cmake -S F:\llvm-project\dtx -B F:\llvm-project\.build\dtx -G Ninja
ninja -C F:\llvm-project\.build\dtx
ctest --test-dir F:\llvm-project\.build\dtx
```

The code intentionally keeps the public namespace as `llvm::dtx` so it can be
linked into a future LLVM-enabled DebugHost without changing generated code.

The `dtx-debughost-tcp-smoke` test starts a local TCP DebugHost mock, dispatches
through generated C++ server code, and calls it through generated C++ client
code. The `dtx-csharp-client-to-cpp-server` test builds the C# runtime and
generated C# client, starts the same C++ TCP server path, and verifies a real
C# client -> TCP -> generated C++ server DebugHost call.

The `dtx-kotlin-client-to-cpp-server` test does the equivalent Kotlin JVM
client -> TCP -> generated C++ server call when `kotlinc` and `java` are
available.

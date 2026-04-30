# DTX C# Runtime

Minimal .NET client runtime for the DTX DebugHost protocol.

Current scope:

- DTX message and payload headers.
- Project-local object archive matching `runtime/cpp-llvm`'s current
  `KeyedArchiver` MVP format.
- Aux argument encoding.
- TCP loopback transport.
- `Connection.CallAsync` for selector request / object reply RPC.
- `dtx-codegen --lang csharp` generated client/types consumption.

The runtime multi-targets `net472` and `net8.0` and has no NuGet package
dependencies. The public surface intentionally stays on APIs shared by .NET
Framework 4.7.2 and .NET 8.0.

The repository smoke test at `test/csharp/DtxCSharpClientSmoke` launches the
C++ DebugHost TCP server test binary and calls the generated
`LifecycleClient.InitializeAsync` method over the wire.

`test/csharp/DtxCSharpGeneratedNet472` is a compile-only check that consumes
the generated C# DebugHost client/types from a `net472` project.

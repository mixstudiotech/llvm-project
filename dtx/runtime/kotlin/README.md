# DTX Kotlin Runtime

Minimal JVM client runtime for the DTX DebugHost protocol.

Current scope:

- DTX message and payload headers.
- Project-local object archive matching `runtime/cpp-llvm`'s current
  `KeyedArchiver` MVP format.
- Aux argument encoding.
- TCP loopback transport.
- Blocking `Connection.call` for selector request / object reply RPC.
- `dtx-codegen --lang kotlin` generated client/types consumption.

The runtime has no external dependencies. The repository smoke test at
`test/kotlin/DtxKotlinClientSmoke` launches the C++ DebugHost TCP server test
binary and calls the generated `LifecycleClient.initialize` method over the
wire when `kotlinc` is available.

using LLVM.DTX;
using LLVM.DTX.DebugHost;

namespace LLVM.DTX.Tests;

public static class GeneratedCompileSmoke
{
    public static InitializeRequest CreateRequest() =>
        new InitializeRequest { Raw = NSObject.String("net472") };

    public static LifecycleClient CreateClient(Connection connection) =>
        new LifecycleClient(connection);
}

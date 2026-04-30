using System.Diagnostics;
using LLVM.DTX;
using LLVM.DTX.DebugHost;

if (args.Length != 1)
{
    Console.Error.WriteLine("usage: DtxCSharpClientSmoke <dtx-debughost-tcp-server>");
    return 2;
}

using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));

using var server = new Process();
server.StartInfo.FileName = args[0];
server.StartInfo.UseShellExecute = false;
server.StartInfo.RedirectStandardOutput = true;
server.StartInfo.RedirectStandardError = true;

if (!server.Start())
{
    Console.Error.WriteLine("failed to start C++ DebugHost server");
    return 1;
}

try
{
    var line = await ReadLineWithTimeoutAsync(server.StandardOutput, timeout.Token).ConfigureAwait(false);
    if (line is null || !line.StartsWith("PORT=", StringComparison.Ordinal))
        throw new DtxException("C++ DebugHost server did not print a port.");

    var port = int.Parse(line["PORT=".Length..]);
    using var transport = await TcpTransport.ConnectLoopbackAsync(port, timeout.Token).ConfigureAwait(false);
    using var connection = new Connection(transport);

    var client = new LifecycleClient(connection);
    var request = new InitializeRequest { Raw = NSObject.String("csharp-request") };
    var reply = await client.InitializeAsync(request, timeout.Token).ConfigureAwait(false);
    if (!reply.Raw.Equals(NSObject.String("csharp-initialized")))
        throw new DtxException("Unexpected initialize reply: " + reply.Raw);

    await server.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
    if (server.ExitCode != 0)
        throw new DtxException("C++ DebugHost server exited with " + server.ExitCode + ": " + server.StandardError.ReadToEnd());

    return 0;
}
catch (Exception ex)
{
    if (!server.HasExited)
        server.Kill(entireProcessTree: true);
    Console.Error.WriteLine(ex);
    Console.Error.WriteLine(server.StandardError.ReadToEnd());
    return 1;
}

static async Task<string?> ReadLineWithTimeoutAsync(StreamReader reader, CancellationToken cancellationToken)
{
    var readTask = reader.ReadLineAsync(cancellationToken).AsTask();
    return await readTask.ConfigureAwait(false);
}

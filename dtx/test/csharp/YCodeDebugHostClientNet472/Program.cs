using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using LLVM.DTX;
using LLVM.DTX.DebugHost;

namespace LLVM.DTX.Tests;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            return RunAsync(args).GetAwaiter().GetResult();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    private static async Task<int> RunAsync(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("usage: YCodeDebugHostClientNet472 <YCode.DebugHost.exe> [--device-id <id>] [--platform ios|android]");
            return 2;
        }

        var hostPath = Path.GetFullPath(args[0]);
        var deviceId = GetOption(args, "--device-id");
        var platform = GetOption(args, "--platform") ?? "ios";

        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        using var server = StartServer(hostPath);

        try
        {
            var portLine = await ReadLineWithTimeoutAsync(server.StandardOutput, timeout.Token).ConfigureAwait(false);
            if (portLine == null || !portLine.StartsWith("PORT=", StringComparison.Ordinal))
                throw new DtxException("YCode.DebugHost.exe did not print PORT=<port>.");

            var port = int.Parse(portLine.Substring("PORT=".Length));
            using var transport = await TcpTransport.ConnectLoopbackAsync(port, timeout.Token).ConfigureAwait(false);
            using var connection = new Connection(transport);

            var lifecycle = new LifecycleClient(connection);
            var session = new SessionClient(connection);
            var execution = new ExecutionClient(connection);
            var breakpoints = new BreakpointsClient(connection);
            var threads = new ThreadsClient(connection);
            var expressions = new ExpressionsClient(connection);
            var memory = new MemoryClient(connection);
            var device = new DeviceClient(connection);
            var deviceSymbols = new DeviceSymbolsClient(connection);

            await TestInitializeAsync(lifecycle, timeout.Token).ConfigureAwait(false);
            await TestDeviceAsync(device, platform, deviceId, timeout.Token).ConfigureAwait(false);
            await TestDeviceSymbolsErrorsAsync(deviceSymbols, timeout.Token).ConfigureAwait(false);

            var sessionId = await TestSessionCreateAsync(session, timeout.Token).ConfigureAwait(false);
            await TestExecutionErrorsAsync(execution, sessionId, timeout.Token).ConfigureAwait(false);
            await TestBreakpointErrorsAsync(breakpoints, sessionId, timeout.Token).ConfigureAwait(false);
            await TestThreadErrorsAsync(threads, sessionId, timeout.Token).ConfigureAwait(false);
            await TestExpressionErrorsAsync(expressions, sessionId, timeout.Token).ConfigureAwait(false);
            await TestMemoryErrorsAsync(memory, sessionId, timeout.Token).ConfigureAwait(false);
            await TestSessionCloseAsync(session, sessionId, timeout.Token).ConfigureAwait(false);
            await TestShutdownAsync(lifecycle, timeout.Token).ConfigureAwait(false);

            if (!server.WaitForExit(10000))
                throw new DtxException("YCode.DebugHost.exe did not exit after shutdown.");
            if (server.ExitCode != 0)
                throw new DtxException("YCode.DebugHost.exe exited with " + server.ExitCode + ": " + server.StandardError.ReadToEnd());

            Console.WriteLine("YCode.DebugHost C# net472 client tests passed.");
            return 0;
        }
        catch
        {
            if (!server.HasExited)
                server.Kill();
            Console.Error.WriteLine(server.StandardError.ReadToEnd());
            throw;
        }
    }

    private static Process StartServer(string hostPath)
    {
        if (!File.Exists(hostPath))
            throw new FileNotFoundException("YCode.DebugHost.exe was not found.", hostPath);

        var process = new Process();
        process.StartInfo.FileName = hostPath;
        process.StartInfo.Arguments = "--print-port";
        process.StartInfo.WorkingDirectory = Path.GetDirectoryName(hostPath) ?? Environment.CurrentDirectory;
        process.StartInfo.UseShellExecute = false;
        process.StartInfo.RedirectStandardOutput = true;
        process.StartInfo.RedirectStandardError = true;
        if (!process.Start())
            throw new DtxException("failed to start YCode.DebugHost.exe");
        return process;
    }

    private static async Task TestInitializeAsync(LifecycleClient lifecycle, CancellationToken token)
    {
        var reply = await lifecycle.InitializeAsync(new InitializeRequest { Raw = Dict() }, token).ConfigureAwait(false);
        var raw = AsDict(reply.Raw, "initialize");
        ExpectBool(raw, "supportsDebugHost", true, "initialize");
        RequireKey(raw, "lldb", "initialize");
        RequireKey(raw, "mixDevice", "initialize");
        Console.WriteLine("ok initialize");
    }

    private static async Task TestDeviceAsync(DeviceClient device, string platform, string? deviceId, CancellationToken token)
    {
        var list = await device.ListAsync(new DeviceListRequest { Raw = Dict() }, token).ConfigureAwait(false);
        var listRaw = AsDict(list.Raw, "device.list");
        RequireKey(listRaw, "success", "device.list");
        RequireArray(listRaw, "devices", "device.list");
        Console.WriteLine("ok device.list");

        var missing = await device.PrepareDebugAsync(new DevicePrepareDebugRequest { Raw = Dict() }, token).ConfigureAwait(false);
        var missingRaw = AsDict(missing.Raw, "device.prepareDebug missing id");
        ExpectBool(missingRaw, "success", false, "device.prepareDebug missing id");
        ExpectBool(missingRaw, "ready", false, "device.prepareDebug missing id");
        Console.WriteLine("ok device.prepareDebug missing id");

        if (!string.IsNullOrEmpty(deviceId))
        {
            var request = Dict(
                Pair("platform", NSObject.String(platform)),
                Pair("deviceId", NSObject.String(deviceId!)));
            var prepared = await device.PrepareDebugAsync(new DevicePrepareDebugRequest { Raw = request }, token).ConfigureAwait(false);
            var preparedRaw = AsDict(prepared.Raw, "device.prepareDebug real device");
            RequireKey(preparedRaw, "success", "device.prepareDebug real device");
            RequireKey(preparedRaw, "ready", "device.prepareDebug real device");
            RequireKey(preparedRaw, "connectUrl", "device.prepareDebug real device");
            Console.WriteLine("ok device.prepareDebug real device");
        }
    }

    private static async Task TestDeviceSymbolsErrorsAsync(DeviceSymbolsClient deviceSymbols, CancellationToken token)
    {
        await ExpectFailureAsync("deviceSymbols.status missing id",
            deviceSymbols.StatusAsync(new DeviceSymbolsStatusRequest { Raw = Dict() }, token)).ConfigureAwait(false);
        await ExpectFailureAsync("deviceSymbols.validate missing id",
            deviceSymbols.ValidateAsync(new DeviceSymbolsValidateRequest { Raw = Dict() }, token)).ConfigureAwait(false);
        await ExpectFailureAsync("deviceSymbols.prefetch missing id",
            deviceSymbols.PrefetchAsync(new DeviceSymbolsPrefetchRequest { Raw = Dict() }, token)).ConfigureAwait(false);
    }

    private static async Task<string> TestSessionCreateAsync(SessionClient session, CancellationToken token)
    {
        var request = Dict(Pair("kind", NSObject.String("unit-test")));
        var reply = await session.CreateAsync(new SessionCreateRequest { Raw = request }, token).ConfigureAwait(false);
        var raw = AsDict(reply.Raw, "session.create");
        ExpectBool(raw, "success", true, "session.create");
        ExpectString(raw, "backend", "lldb", "session.create");
        var sessionId = RequireString(raw, "sessionId", "session.create");
        Console.WriteLine("ok session.create " + sessionId);
        return sessionId;
    }

    private static async Task TestSessionCloseAsync(SessionClient session, string sessionId, CancellationToken token)
    {
        var reply = await session.CloseAsync(new SessionCloseRequest { Raw = SessionRequest(sessionId) }, token).ConfigureAwait(false);
        ExpectBool(AsDict(reply.Raw, "session.close"), "success", true, "session.close");
        Console.WriteLine("ok session.close");
    }

    private static async Task TestExecutionErrorsAsync(ExecutionClient execution, string sessionId, CancellationToken token)
    {
        await ExpectFailureAsync("execution.launch missing executable",
            execution.LaunchAsync(new LaunchRequest { Raw = SessionRequest(sessionId) }, token)).ConfigureAwait(false);
        await ExpectFailureAsync("execution.attach missing pid",
            execution.AttachAsync(new AttachRequest { Raw = SessionRequest(sessionId) }, token)).ConfigureAwait(false);
        await ExpectFailureAsync("execution.continue without process",
            execution.ContinueExecutionAsync(new ContinueRequest { Raw = SessionRequest(sessionId) }, token)).ConfigureAwait(false);
        await ExpectFailureAsync("execution.pause without process",
            execution.PauseAsync(new PauseRequest { Raw = SessionRequest(sessionId) }, token)).ConfigureAwait(false);
    }

    private static async Task TestBreakpointErrorsAsync(BreakpointsClient breakpoints, string sessionId, CancellationToken token)
    {
        var request = Dict(
            Pair("sessionId", NSObject.String(sessionId)),
            Pair("source", Dict(Pair("path", NSObject.String(@"C:\missing.cpp")))),
            Pair("breakpoints", NSObject.Array(new[] { Dict(Pair("line", NSObject.UInt(1))) })));
        await ExpectFailureAsync("breakpoints.setSource without target",
            breakpoints.SetSourceAsync(new SetSourceBreakpointsRequest { Raw = request }, token)).ConfigureAwait(false);
    }

    private static async Task TestThreadErrorsAsync(ThreadsClient threads, string sessionId, CancellationToken token)
    {
        await ExpectFailureAsync("threads.list without process",
            threads.ListAsync(new ThreadsRequest { Raw = SessionRequest(sessionId) }, token)).ConfigureAwait(false);
        await ExpectFailureAsync("threads.stackTrace without process",
            threads.StackTraceAsync(new StackTraceRequest { Raw = SessionRequest(sessionId) }, token)).ConfigureAwait(false);
        await ExpectFailureAsync("threads.scopes without process",
            threads.ScopesAsync(new ScopesRequest { Raw = SessionRequest(sessionId) }, token)).ConfigureAwait(false);
        await ExpectFailureAsync("threads.variables unknown ref",
            threads.VariablesAsync(new VariablesRequest { Raw = Dict(Pair("sessionId", NSObject.String(sessionId)), Pair("variablesReference", NSObject.UInt(1))) }, token)).ConfigureAwait(false);
    }

    private static async Task TestExpressionErrorsAsync(ExpressionsClient expressions, string sessionId, CancellationToken token)
    {
        var request = Dict(Pair("sessionId", NSObject.String(sessionId)), Pair("expression", NSObject.String("1 + 1")));
        await ExpectFailureAsync("expressions.evaluate without process",
            expressions.EvaluateAsync(new EvaluateRequest { Raw = request }, token)).ConfigureAwait(false);
    }

    private static async Task TestMemoryErrorsAsync(MemoryClient memory, string sessionId, CancellationToken token)
    {
        var read = Dict(
            Pair("sessionId", NSObject.String(sessionId)),
            Pair("address", NSObject.UInt(0x1000)),
            Pair("count", NSObject.UInt(16)));
        await ExpectFailureAsync("memory.read without process",
            memory.ReadAsync(new ReadMemoryRequest { Raw = read }, token)).ConfigureAwait(false);

        var disassemble = Dict(
            Pair("sessionId", NSObject.String(sessionId)),
            Pair("address", NSObject.UInt(0x1000)),
            Pair("count", NSObject.UInt(4)));
        await ExpectFailureAsync("memory.disassemble without target",
            memory.DisassembleAsync(new DisassembleRequest { Raw = disassemble }, token)).ConfigureAwait(false);
    }

    private static async Task TestShutdownAsync(LifecycleClient lifecycle, CancellationToken token)
    {
        var reply = await lifecycle.ShutdownAsync(new ShutdownRequest { Raw = Dict() }, token).ConfigureAwait(false);
        ExpectBool(AsDict(reply.Raw, "shutdown"), "success", true, "shutdown");
        Console.WriteLine("ok shutdown");
    }

    private static async Task ExpectFailureAsync<T>(string name, Task<T> task)
    {
        var result = await task.ConfigureAwait(false);
        var rawProperty = typeof(T).GetProperty("Raw");
        if (rawProperty == null)
            throw new DtxException(name + " result type has no Raw property.");
        var raw = (NSObject)rawProperty.GetValue(result)!;
        ExpectBool(AsDict(raw, name), "success", false, name);
        Console.WriteLine("ok " + name);
    }

    private static NSObject SessionRequest(string sessionId) =>
        Dict(Pair("sessionId", NSObject.String(sessionId)));

    private static KeyValuePair<string, NSObject> Pair(string key, NSObject value) =>
        new KeyValuePair<string, NSObject>(key, value);

    private static NSObject Dict(params KeyValuePair<string, NSObject>[] pairs)
    {
        var dict = new Dictionary<string, NSObject>(StringComparer.Ordinal);
        foreach (var pair in pairs)
            dict[pair.Key] = pair.Value;
        return NSObject.Dict(dict);
    }

    private static IReadOnlyDictionary<string, NSObject> AsDict(NSObject value, string context)
    {
        if (value.Kind != NSObjectKind.Dict)
            throw new DtxException(context + " did not return a dictionary: " + value);
        return (ReadOnlyDictionary<string, NSObject>)value.Value!;
    }

    private static NSObject RequireKey(IReadOnlyDictionary<string, NSObject> dict, string key, string context)
    {
        if (!dict.TryGetValue(key, out var value))
            throw new DtxException(context + " missing key '" + key + "'.");
        return value;
    }

    private static string RequireString(IReadOnlyDictionary<string, NSObject> dict, string key, string context)
    {
        var value = RequireKey(dict, key, context);
        if (value.Kind != NSObjectKind.String)
            throw new DtxException(context + " key '" + key + "' is not a string: " + value);
        return (string)value.Value!;
    }

    private static void ExpectString(IReadOnlyDictionary<string, NSObject> dict, string key, string expected, string context)
    {
        var actual = RequireString(dict, key, context);
        if (!string.Equals(actual, expected, StringComparison.Ordinal))
            throw new DtxException(context + " key '" + key + "' expected '" + expected + "' but got '" + actual + "'.");
    }

    private static void ExpectBool(IReadOnlyDictionary<string, NSObject> dict, string key, bool expected, string context)
    {
        var value = RequireKey(dict, key, context);
        if (value.Kind != NSObjectKind.Bool || (bool)value.Value! != expected)
            throw new DtxException(context + " key '" + key + "' expected " + expected + " but got " + value + ".");
    }

    private static NSObject[] RequireArray(IReadOnlyDictionary<string, NSObject> dict, string key, string context)
    {
        var value = RequireKey(dict, key, context);
        if (value.Kind != NSObjectKind.Array)
            throw new DtxException(context + " key '" + key + "' is not an array: " + value);
        return (NSObject[])value.Value!;
    }

    private static string? GetOption(string[] args, string name)
    {
        for (var i = 1; i + 1 < args.Length; ++i)
            if (string.Equals(args[i], name, StringComparison.Ordinal))
                return args[i + 1];
        return null;
    }

    private static async Task<string?> ReadLineWithTimeoutAsync(StreamReader reader, CancellationToken token)
    {
        var readTask = reader.ReadLineAsync();
        var timeoutTask = Task.Delay(Timeout.InfiniteTimeSpan, token);
        var completed = await Task.WhenAny(readTask, timeoutTask).ConfigureAwait(false);
        if (completed != readTask)
            throw new TimeoutException("Timed out waiting for YCode.DebugHost.exe to print PORT=<port>.");
        return await readTask.ConfigureAwait(false);
    }
}

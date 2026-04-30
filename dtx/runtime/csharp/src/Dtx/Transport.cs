using System.Net;
using System.Net.Sockets;

namespace LLVM.DTX;

public interface ITransport : IDisposable
{
    Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken = default);
    Task WriteAsync(byte[] buffer, CancellationToken cancellationToken = default);
}

public sealed class TcpTransport : ITransport
{
    private readonly TcpClient _client;
    private readonly NetworkStream _stream;

    private TcpTransport(TcpClient client)
    {
        _client = client;
        _stream = client.GetStream();
    }

    public static async Task<TcpTransport> ConnectLoopbackAsync(
        int port,
        CancellationToken cancellationToken = default)
    {
        var client = new TcpClient();
#if NET8_0_OR_GREATER
        await client.ConnectAsync(IPAddress.Loopback, port, cancellationToken).ConfigureAwait(false);
#else
        cancellationToken.ThrowIfCancellationRequested();
        using (cancellationToken.Register(client.Close))
        {
            await client.ConnectAsync(IPAddress.Loopback, port).ConfigureAwait(false);
        }
#endif
        client.NoDelay = true;
        return new TcpTransport(client);
    }

    public Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken = default) =>
#if NET8_0_OR_GREATER
        _stream.ReadAsync(buffer, offset, count, cancellationToken);
#else
        cancellationToken.IsCancellationRequested
            ? Task.FromCanceled<int>(cancellationToken)
            : _stream.ReadAsync(buffer, offset, count);
#endif

    public async Task WriteAsync(byte[] buffer, CancellationToken cancellationToken = default)
    {
#if NET8_0_OR_GREATER
        await _stream.WriteAsync(buffer, 0, buffer.Length, cancellationToken).ConfigureAwait(false);
        await _stream.FlushAsync(cancellationToken).ConfigureAwait(false);
#else
        cancellationToken.ThrowIfCancellationRequested();
        await _stream.WriteAsync(buffer, 0, buffer.Length).ConfigureAwait(false);
        await _stream.FlushAsync().ConfigureAwait(false);
#endif
    }

    public void Dispose()
    {
        _stream.Dispose();
        _client.Dispose();
    }

}

namespace LLVM.DTX;

public sealed class Connection : IDisposable
{
    private readonly ITransport _transport;
    private uint _nextMessageId;

    public Connection(ITransport transport) => _transport = transport;

    public async Task<NSObject> CallAsync(
        uint channel,
        string selector,
        AuxList args,
        bool expectsReply = true,
        CancellationToken cancellationToken = default)
    {
        var payload = Fragment.BuildSelectorPayload(selector, args);
        var messageId = ++_nextMessageId;
        var header = MessageHeader.Build(channel, checked((uint)payload.Length), messageId, 0, expectsReply);
        await _transport.WriteAsync(Fragment.BuildFrame(header, payload), cancellationToken).ConfigureAwait(false);

        if (!expectsReply)
            return NSObject.Null;

        var replyHeaderBytes = await ReadExactAsync(32, cancellationToken).ConfigureAwait(false);
        var replyHeader = MessageHeader.Decode(replyHeaderBytes);
        var replyPayload = await ReadExactAsync(checked((int)replyHeader.PayloadLength), cancellationToken).ConfigureAwait(false);
        return Fragment.ParseObjectPayload(replyPayload);
    }

    private async Task<byte[]> ReadExactAsync(int size, CancellationToken cancellationToken)
    {
        var bytes = new byte[size];
        var offset = 0;
        while (offset < size)
        {
            var count = await _transport.ReadAsync(bytes, offset, size - offset, cancellationToken).ConfigureAwait(false);
            if (count == 0)
                throw new DtxException("Transport reached EOF.");
            offset += count;
        }
        return bytes;
    }

    public void Dispose() => _transport.Dispose();
}

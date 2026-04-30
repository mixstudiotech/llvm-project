namespace LLVM.DTX;

public static class Fragment
{
    public static byte[] BuildSelectorPayload(string selector, AuxList args)
    {
        var auxBytes = args.Encode();
        var selectorBytes = KeyedArchiver.ArchiveRoot(NSObject.String(selector));
        return BuildPayload(PayloadFlag.Selector, auxBytes, selectorBytes);
    }

    public static byte[] BuildObjectPayload(NSObject value, PayloadFlag flag = PayloadFlag.Reply) =>
        BuildPayload(flag, Array.Empty<byte>(), KeyedArchiver.ArchiveRoot(value));

    public static byte[] BuildFrame(MessageHeader header, byte[] payload)
    {
        using var stream = new MemoryStream();
        var headerBytes = header.Encode();
        stream.Write(headerBytes, 0, headerBytes.Length);
        stream.Write(payload, 0, payload.Length);
        return stream.ToArray();
    }

    public static NSObject ParseObjectPayload(byte[] payload)
    {
        var header = PayloadHeader.Decode(payload);
        if (header.Flags != (uint)PayloadFlag.Reply &&
            header.Flags != (uint)PayloadFlag.Object &&
            header.Flags != (uint)PayloadFlag.AsyncObject)
            throw new DtxException("Payload is not an object reply.");
        if (header.AuxiliaryLength != 0)
            throw new DtxException("Object payload unexpectedly contains aux bytes.");
        if (16 + checked((int)header.TotalLength) != payload.Length)
            throw new DtxException("Object payload length mismatch.");
        if (header.TotalLength == 0)
            return NSObject.Null;
        return KeyedArchiver.UnarchiveRoot(payload, 16, checked((int)header.TotalLength));
    }

    private static byte[] BuildPayload(PayloadFlag flag, byte[] auxBytes, byte[] bodyBytes)
    {
        var header = new PayloadHeader(
            (uint)flag,
            checked((uint)auxBytes.Length),
            checked((ulong)(auxBytes.Length + bodyBytes.Length)));
        using var stream = new MemoryStream();
        var headerBytes = header.Encode();
        stream.Write(headerBytes, 0, headerBytes.Length);
        stream.Write(auxBytes, 0, auxBytes.Length);
        stream.Write(bodyBytes, 0, bodyBytes.Length);
        return stream.ToArray();
    }
}

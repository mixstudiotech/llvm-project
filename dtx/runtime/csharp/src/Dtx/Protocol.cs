namespace LLVM.DTX;

public enum PayloadFlag : uint
{
    Empty = 0x00,
    AsyncObject = 0x01,
    Selector = 0x02,
    Reply = 0x03,
    Object = 0x04,
    Null = 0x05,
}

public readonly struct MessageHeader
{
    public const uint ExpectedMagic = 0x1F3D5B79;
    public const uint ExpectedHeaderLength = 0x20;

    public MessageHeader(
        uint magic,
        uint headerLength,
        ushort fragmentId,
        ushort fragmentCount,
        uint payloadLength,
        uint messageId,
        uint conversationIndex,
        uint channelCode,
        uint expectsReply)
    {
        Magic = magic;
        HeaderLength = headerLength;
        FragmentId = fragmentId;
        FragmentCount = fragmentCount;
        PayloadLength = payloadLength;
        MessageId = messageId;
        ConversationIndex = conversationIndex;
        ChannelCode = channelCode;
        ExpectsReply = expectsReply;
    }

    public uint Magic { get; }
    public uint HeaderLength { get; }
    public ushort FragmentId { get; }
    public ushort FragmentCount { get; }
    public uint PayloadLength { get; }
    public uint MessageId { get; }
    public uint ConversationIndex { get; }
    public uint ChannelCode { get; }
    public uint ExpectsReply { get; }

    public static MessageHeader Build(
        uint channelCode,
        uint payloadLength,
        uint messageId,
        uint conversationIndex,
        bool expectsReply) =>
        new(ExpectedMagic, ExpectedHeaderLength, 0, 1, payloadLength, messageId,
            conversationIndex, channelCode, expectsReply ? 1u : 0u);

    public byte[] Encode()
    {
        var bytes = new byte[32];
        BinaryUtil.WriteUInt32LE(bytes, 0, Magic);
        BinaryUtil.WriteUInt32LE(bytes, 4, HeaderLength);
        BinaryUtil.WriteUInt16LE(bytes, 8, FragmentId);
        BinaryUtil.WriteUInt16LE(bytes, 10, FragmentCount);
        BinaryUtil.WriteUInt32LE(bytes, 12, PayloadLength);
        BinaryUtil.WriteUInt32LE(bytes, 16, MessageId);
        BinaryUtil.WriteUInt32LE(bytes, 20, ConversationIndex);
        BinaryUtil.WriteUInt32LE(bytes, 24, ChannelCode);
        BinaryUtil.WriteUInt32LE(bytes, 28, ExpectsReply);
        return bytes;
    }

    public static MessageHeader Decode(byte[] bytes)
    {
        if (bytes.Length < 32)
            throw new DtxException("DTX message header is shorter than 32 bytes.");
        var header = new MessageHeader(
            BinaryUtil.ReadUInt32LE(bytes, 0),
            BinaryUtil.ReadUInt32LE(bytes, 4),
            BinaryUtil.ReadUInt16LE(bytes, 8),
            BinaryUtil.ReadUInt16LE(bytes, 10),
            BinaryUtil.ReadUInt32LE(bytes, 12),
            BinaryUtil.ReadUInt32LE(bytes, 16),
            BinaryUtil.ReadUInt32LE(bytes, 20),
            BinaryUtil.ReadUInt32LE(bytes, 24),
            BinaryUtil.ReadUInt32LE(bytes, 28));
        if (header.Magic != ExpectedMagic)
            throw new DtxException("Bad DTX message magic.");
        if (header.HeaderLength != ExpectedHeaderLength)
            throw new DtxException("Bad DTX message header length.");
        return header;
    }
}

public readonly struct PayloadHeader
{
    public PayloadHeader(uint flags, uint auxiliaryLength, ulong totalLength)
    {
        Flags = flags;
        AuxiliaryLength = auxiliaryLength;
        TotalLength = totalLength;
    }

    public uint Flags { get; }
    public uint AuxiliaryLength { get; }
    public ulong TotalLength { get; }

    public byte[] Encode()
    {
        var bytes = new byte[16];
        BinaryUtil.WriteUInt32LE(bytes, 0, Flags);
        BinaryUtil.WriteUInt32LE(bytes, 4, AuxiliaryLength);
        BinaryUtil.WriteUInt64LE(bytes, 8, TotalLength);
        return bytes;
    }

    public static PayloadHeader Decode(byte[] bytes)
    {
        if (bytes.Length < 16)
            throw new DtxException("DTX payload header is shorter than 16 bytes.");
        return new PayloadHeader(
            BinaryUtil.ReadUInt32LE(bytes, 0),
            BinaryUtil.ReadUInt32LE(bytes, 4),
            BinaryUtil.ReadUInt64LE(bytes, 8));
    }
}

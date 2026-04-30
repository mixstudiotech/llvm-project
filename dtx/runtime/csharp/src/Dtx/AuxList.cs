namespace LLVM.DTX;

public enum AuxKind
{
    Null,
    I32,
    I64,
    U32,
    U64,
    Object,
}

public sealed class Arg
{
    private Arg(AuxKind kind, object? value)
    {
        Kind = kind;
        Value = value;
    }

    public AuxKind Kind { get; }
    public object? Value { get; }

    public static Arg Null() => new(AuxKind.Null, null);
    public static Arg I32(int value) => new(AuxKind.I32, value);
    public static Arg I64(long value) => new(AuxKind.I64, value);
    public static Arg U32(uint value) => new(AuxKind.U32, value);
    public static Arg U64(ulong value) => new(AuxKind.U64, value);
    public static Arg Object(NSObject value) => new(AuxKind.Object, value);
}

public sealed class AuxList
{
    private const ulong AuxMagic = 0x01F0;
    private readonly List<Arg> _args = new();

    public IReadOnlyList<Arg> Args => _args;
    public void Add(Arg arg) => _args.Add(arg);

    public byte[] Encode()
    {
        using var body = new MemoryStream();
        foreach (var arg in _args)
        {
            switch (arg.Kind)
            {
                case AuxKind.Null:
                    WriteType(body, 2);
                    WriteU32(body, 0);
                    break;
                case AuxKind.I32:
                    WriteType(body, 3);
                    WriteU32(body, unchecked((uint)(int)arg.Value!));
                    break;
                case AuxKind.I64:
                    WriteType(body, 4);
                    WriteU64(body, unchecked((ulong)(long)arg.Value!));
                    break;
                case AuxKind.U32:
                    WriteType(body, 5);
                    WriteU32(body, (uint)arg.Value!);
                    break;
                case AuxKind.U64:
                    WriteType(body, 6);
                    WriteU64(body, (ulong)arg.Value!);
                    break;
                case AuxKind.Object:
                    WriteType(body, 2);
                    var bytes = KeyedArchiver.ArchiveRoot((NSObject)arg.Value!);
                    WriteU32(body, checked((uint)bytes.Length));
                    body.Write(bytes, 0, bytes.Length);
                    break;
            }
        }

        using var output = new MemoryStream();
        WriteU64(output, AuxMagic);
        WriteU64(output, checked((ulong)body.Length));
        var bodyBytes = body.ToArray();
        output.Write(bodyBytes, 0, bodyBytes.Length);
        return output.ToArray();
    }

    public static AuxList Decode(byte[] bytes)
    {
        if (bytes.Length < 16)
            throw new DtxException("Aux list is shorter than header.");
        if (BinaryUtil.ReadUInt64LE(bytes, 0) != AuxMagic)
            throw new DtxException("Bad aux magic.");
        var bodyLength = BinaryUtil.ReadUInt64LE(bytes, 8);
        if (checked((int)bodyLength) + 16 != bytes.Length)
            throw new DtxException("Aux length does not match buffer size.");

        var list = new AuxList();
        var offset = 16;
        while (offset < bytes.Length)
        {
            var typeTag = ReadU32(bytes, ref offset);
            var type = ReadU32(bytes, ref offset);
            if (typeTag != 10)
                throw new DtxException("Bad aux type tag.");
            switch (type)
            {
                case 2:
                    var objectLength = checked((int)ReadU32(bytes, ref offset));
                    if (objectLength == 0)
                    {
                        list.Add(Arg.Null());
                        break;
                    }
                    if (offset + objectLength > bytes.Length)
                        throw new DtxException("Object aux payload extends past buffer.");
                    list.Add(Arg.Object(KeyedArchiver.UnarchiveRoot(bytes, offset, objectLength)));
                    offset += objectLength;
                    break;
                case 3:
                    list.Add(Arg.I32(unchecked((int)ReadU32(bytes, ref offset))));
                    break;
                case 4:
                    list.Add(Arg.I64(unchecked((long)ReadU64(bytes, ref offset))));
                    break;
                case 5:
                    list.Add(Arg.U32(ReadU32(bytes, ref offset)));
                    break;
                case 6:
                    list.Add(Arg.U64(ReadU64(bytes, ref offset)));
                    break;
                default:
                    throw new DtxException("Unsupported aux type.");
            }
        }
        return list;
    }

    private static void WriteType(Stream stream, uint type)
    {
        WriteU32(stream, 10);
        WriteU32(stream, type);
    }

    private static void WriteU32(Stream stream, uint value)
    {
        BinaryUtil.WriteUInt32LE(stream, value);
    }

    private static void WriteU64(Stream stream, ulong value)
    {
        BinaryUtil.WriteUInt64LE(stream, value);
    }

    private static uint ReadU32(byte[] bytes, ref int offset)
    {
        if (offset + 4 > bytes.Length)
            throw new DtxException("Unexpected EOF while reading aux u32.");
        var value = BinaryUtil.ReadUInt32LE(bytes, offset);
        offset += 4;
        return value;
    }

    private static ulong ReadU64(byte[] bytes, ref int offset)
    {
        if (offset + 8 > bytes.Length)
            throw new DtxException("Unexpected EOF while reading aux u64.");
        var value = BinaryUtil.ReadUInt64LE(bytes, offset);
        offset += 8;
        return value;
    }
}

using System.Text;

namespace LLVM.DTX;

public static class KeyedArchiver
{
    private static readonly byte[] Magic = { (byte)'D', (byte)'T', (byte)'X', (byte)'K', (byte)'A', (byte)'R', (byte)'1', 0 };

    private enum Tag : byte
    {
        Null = 0,
        Bool = 1,
        Int = 2,
        UInt = 3,
        Double = 4,
        String = 5,
        Data = 6,
        Array = 7,
        Dict = 8,
    }

    public static byte[] ArchiveRoot(NSObject root)
    {
        using var stream = new MemoryStream();
        stream.Write(Magic, 0, Magic.Length);
        WriteObject(stream, root);
        return stream.ToArray();
    }

    public static NSObject UnarchiveRoot(byte[] bytes) =>
        UnarchiveRoot(bytes, 0, bytes.Length);

    public static NSObject UnarchiveRoot(byte[] bytes, int offset, int length)
    {
        if (length < Magic.Length)
            throw new DtxException("Bad object archive magic.");
        var archive = BinaryUtil.Slice(bytes, offset, length);
        if (!BinaryUtil.StartsWith(archive, Magic))
            throw new DtxException("Bad object archive magic.");
        var reader = new Reader(archive, Magic.Length);
        var root = reader.ReadObject();
        if (!reader.Consumed)
            throw new DtxException("Trailing bytes after object archive root.");
        return root;
    }

    private static void WriteObject(Stream stream, NSObject value)
    {
        stream.WriteByte((byte)(value.Kind switch
        {
            NSObjectKind.Null => Tag.Null,
            NSObjectKind.Bool => Tag.Bool,
            NSObjectKind.Int => Tag.Int,
            NSObjectKind.UInt => Tag.UInt,
            NSObjectKind.Double => Tag.Double,
            NSObjectKind.String => Tag.String,
            NSObjectKind.Data => Tag.Data,
            NSObjectKind.Array => Tag.Array,
            NSObjectKind.Dict => Tag.Dict,
            _ => throw new DtxException("Unsupported NSObject kind."),
        }));

        switch (value.Kind)
        {
            case NSObjectKind.Null:
                break;
            case NSObjectKind.Bool:
                stream.WriteByte((bool)value.Value! ? (byte)1 : (byte)0);
                break;
            case NSObjectKind.Int:
                WriteU64(stream, unchecked((ulong)(long)value.Value!));
                break;
            case NSObjectKind.UInt:
                WriteU64(stream, (ulong)value.Value!);
                break;
            case NSObjectKind.Double:
                WriteU64(stream, BinaryUtil.DoubleToUInt64Bits((double)value.Value!));
                break;
            case NSObjectKind.String:
                WriteBytes(stream, Encoding.UTF8.GetBytes((string)value.Value!));
                break;
            case NSObjectKind.Data:
                WriteBytes(stream, (byte[])value.Value!);
                break;
            case NSObjectKind.Array:
                var array = (NSObject[])value.Value!;
                WriteU32(stream, checked((uint)array.Length));
                foreach (var item in array)
                    WriteObject(stream, item);
                break;
            case NSObjectKind.Dict:
                var dict = (IReadOnlyDictionary<string, NSObject>)value.Value!;
                WriteU32(stream, checked((uint)dict.Count));
                foreach (var item in dict.OrderBy(kv => kv.Key, StringComparer.Ordinal))
                {
                    WriteBytes(stream, Encoding.UTF8.GetBytes(item.Key));
                    WriteObject(stream, item.Value);
                }
                break;
        }
    }

    private static void WriteBytes(Stream stream, byte[] bytes)
    {
        WriteU32(stream, checked((uint)bytes.Length));
        stream.Write(bytes, 0, bytes.Length);
    }

    private static void WriteU32(Stream stream, uint value)
    {
        BinaryUtil.WriteUInt32LE(stream, value);
    }

    private static void WriteU64(Stream stream, ulong value)
    {
        BinaryUtil.WriteUInt64LE(stream, value);
    }

    private sealed class Reader
    {
        private readonly byte[] _bytes;
        private int _offset;

        public Reader(byte[] bytes, int offset)
        {
            _bytes = bytes;
            _offset = offset;
        }

        public bool Consumed => _offset == _bytes.Length;

        public NSObject ReadObject()
        {
            var tag = (Tag)ReadByte();
            return tag switch
            {
                Tag.Null => NSObject.Null,
                Tag.Bool => NSObject.Bool(ReadByte() != 0),
                Tag.Int => NSObject.Int(unchecked((long)ReadU64())),
                Tag.UInt => NSObject.UInt(ReadU64()),
                Tag.Double => NSObject.Double(BinaryUtil.UInt64BitsToDouble(ReadU64())),
                Tag.String => NSObject.String(Encoding.UTF8.GetString(ReadBytes())),
                Tag.Data => NSObject.Data(ReadBytes()),
                Tag.Array => ReadArray(),
                Tag.Dict => ReadDict(),
                _ => throw new DtxException("Unknown object archive tag."),
            };
        }

        private NSObject ReadArray()
        {
            var count = ReadU32();
            var values = new NSObject[count];
            for (var i = 0; i < values.Length; i++)
                values[i] = ReadObject();
            return NSObject.Array(values);
        }

        private NSObject ReadDict()
        {
            var count = ReadU32();
            var values = new Dictionary<string, NSObject>(StringComparer.Ordinal);
            for (uint i = 0; i < count; i++)
            {
                var key = Encoding.UTF8.GetString(ReadBytes());
                values[key] = ReadObject();
            }
            return NSObject.Dict(values);
        }

        private byte ReadByte()
        {
            if (_offset >= _bytes.Length)
                throw new DtxException("Unexpected EOF while reading object archive.");
            return _bytes[_offset++];
        }

        private uint ReadU32()
        {
            if (_offset + 4 > _bytes.Length)
                throw new DtxException("Unexpected EOF while reading u32.");
            var value = BinaryUtil.ReadUInt32LE(_bytes, _offset);
            _offset += 4;
            return value;
        }

        private ulong ReadU64()
        {
            if (_offset + 8 > _bytes.Length)
                throw new DtxException("Unexpected EOF while reading u64.");
            var value = BinaryUtil.ReadUInt64LE(_bytes, _offset);
            _offset += 8;
            return value;
        }

        private byte[] ReadBytes()
        {
            var length = checked((int)ReadU32());
            if (_offset + length > _bytes.Length)
                throw new DtxException("Object archive byte payload extends past buffer.");
            var bytes = BinaryUtil.Slice(_bytes, _offset, length);
            _offset += length;
            return bytes;
        }
    }
}

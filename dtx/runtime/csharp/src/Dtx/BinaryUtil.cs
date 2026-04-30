namespace LLVM.DTX;

internal static class BinaryUtil
{
    public static ushort ReadUInt16LE(byte[] bytes, int offset)
    {
        CheckRange(bytes, offset, 2);
        return (ushort)(bytes[offset] | (bytes[offset + 1] << 8));
    }

    public static uint ReadUInt32LE(byte[] bytes, int offset)
    {
        CheckRange(bytes, offset, 4);
        return (uint)(
            bytes[offset] |
            (bytes[offset + 1] << 8) |
            (bytes[offset + 2] << 16) |
            (bytes[offset + 3] << 24));
    }

    public static ulong ReadUInt64LE(byte[] bytes, int offset)
    {
        CheckRange(bytes, offset, 8);
        ulong value = 0;
        for (var i = 7; i >= 0; --i)
            value = (value << 8) | bytes[offset + i];
        return value;
    }

    public static void WriteUInt16LE(byte[] bytes, int offset, ushort value)
    {
        CheckRange(bytes, offset, 2);
        bytes[offset] = (byte)value;
        bytes[offset + 1] = (byte)(value >> 8);
    }

    public static void WriteUInt32LE(byte[] bytes, int offset, uint value)
    {
        CheckRange(bytes, offset, 4);
        bytes[offset] = (byte)value;
        bytes[offset + 1] = (byte)(value >> 8);
        bytes[offset + 2] = (byte)(value >> 16);
        bytes[offset + 3] = (byte)(value >> 24);
    }

    public static void WriteUInt64LE(byte[] bytes, int offset, ulong value)
    {
        CheckRange(bytes, offset, 8);
        for (var i = 0; i < 8; ++i)
        {
            bytes[offset + i] = (byte)value;
            value >>= 8;
        }
    }

    public static void WriteUInt32LE(Stream stream, uint value)
    {
        var bytes = new byte[4];
        WriteUInt32LE(bytes, 0, value);
        stream.Write(bytes, 0, bytes.Length);
    }

    public static void WriteUInt64LE(Stream stream, ulong value)
    {
        var bytes = new byte[8];
        WriteUInt64LE(bytes, 0, value);
        stream.Write(bytes, 0, bytes.Length);
    }

    public static ulong DoubleToUInt64Bits(double value)
    {
        var bytes = BitConverter.GetBytes(value);
        if (!BitConverter.IsLittleEndian)
            Array.Reverse(bytes);
        return ReadUInt64LE(bytes, 0);
    }

    public static double UInt64BitsToDouble(ulong value)
    {
        var bytes = new byte[8];
        WriteUInt64LE(bytes, 0, value);
        if (!BitConverter.IsLittleEndian)
            Array.Reverse(bytes);
        return BitConverter.ToDouble(bytes, 0);
    }

    public static byte[] Slice(byte[] bytes, int offset, int length)
    {
        CheckRange(bytes, offset, length);
        var result = new byte[length];
        Buffer.BlockCopy(bytes, offset, result, 0, length);
        return result;
    }

    public static bool StartsWith(byte[] bytes, byte[] prefix)
    {
        if (bytes.Length < prefix.Length)
            return false;
        for (var i = 0; i < prefix.Length; ++i)
        {
            if (bytes[i] != prefix[i])
                return false;
        }
        return true;
    }

    private static void CheckRange(byte[] bytes, int offset, int length)
    {
        if (offset < 0 || length < 0 || offset > bytes.Length - length)
            throw new DtxException("Byte range extends past buffer.");
    }
}

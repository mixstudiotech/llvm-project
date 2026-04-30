using System.Collections.ObjectModel;
using System.Text;

namespace LLVM.DTX;

public enum NSObjectKind
{
    Null,
    Bool,
    Int,
    UInt,
    Double,
    String,
    Data,
    Array,
    Dict,
}

public sealed class NSObject : IEquatable<NSObject>
{
    public static readonly NSObject Null = new(NSObjectKind.Null, null);

    private NSObject(NSObjectKind kind, object? value)
    {
        Kind = kind;
        Value = value;
    }

    public NSObjectKind Kind { get; }
    public object? Value { get; }

    public static NSObject Bool(bool value) => new(NSObjectKind.Bool, value);
    public static NSObject Int(long value) => new(NSObjectKind.Int, value);
    public static NSObject UInt(ulong value) => new(NSObjectKind.UInt, value);
    public static NSObject Double(double value) => new(NSObjectKind.Double, value);
    public static NSObject String(string value) => new(NSObjectKind.String, value);
    public static NSObject Data(byte[] value) => new(NSObjectKind.Data, value.ToArray());
    public static NSObject Array(IEnumerable<NSObject> value) => new(NSObjectKind.Array, value.ToArray());
    public static NSObject Dict(IDictionary<string, NSObject> value) =>
        new(NSObjectKind.Dict, new ReadOnlyDictionary<string, NSObject>(
            new SortedDictionary<string, NSObject>(value, StringComparer.Ordinal)));

    public static NSObject From(string value) => String(value);
    public static NSObject From(bool value) => Bool(value);
    public static NSObject From(int value) => Int(value);
    public static NSObject From(uint value) => UInt(value);
    public static NSObject From(long value) => Int(value);
    public static NSObject From(ulong value) => UInt(value);
    public static NSObject From(double value) => Double(value);
    public static NSObject From(byte[] value) => Data(value);

    public string AsString() =>
        Kind == NSObjectKind.String ? (string)Value! : throw new DtxException("NSObject is not a string.");

    public bool Equals(NSObject? other)
    {
        if (other is null || Kind != other.Kind)
            return false;
        return Kind switch
        {
            NSObjectKind.Null => true,
            NSObjectKind.Bool => (bool)Value! == (bool)other.Value!,
            NSObjectKind.Int => (long)Value! == (long)other.Value!,
            NSObjectKind.UInt => (ulong)Value! == (ulong)other.Value!,
            NSObjectKind.Double => (double)Value! == (double)other.Value!,
            NSObjectKind.String => (string)Value! == (string)other.Value!,
            NSObjectKind.Data => ((byte[])Value!).SequenceEqual((byte[])other.Value!),
            NSObjectKind.Array => ((NSObject[])Value!).SequenceEqual((NSObject[])other.Value!),
            NSObjectKind.Dict => DictEquals(
                (IReadOnlyDictionary<string, NSObject>)Value!,
                (IReadOnlyDictionary<string, NSObject>)other.Value!),
            _ => false,
        };
    }

    public override bool Equals(object? obj) => Equals(obj as NSObject);

    public override int GetHashCode()
    {
        var hash = (int)Kind;
        switch (Kind)
        {
            case NSObjectKind.Null:
                break;
            case NSObjectKind.Data:
                foreach (var b in (byte[])Value!)
                    hash = CombineHash(hash, b);
                break;
            case NSObjectKind.Array:
                foreach (var item in (NSObject[])Value!)
                    hash = CombineHash(hash, item.GetHashCode());
                break;
            case NSObjectKind.Dict:
                foreach (var item in (IReadOnlyDictionary<string, NSObject>)Value!)
                {
                    hash = CombineHash(hash, item.Key.GetHashCode());
                    hash = CombineHash(hash, item.Value.GetHashCode());
                }
                break;
            default:
                hash = CombineHash(hash, Value!.GetHashCode());
                break;
        }
        return hash;
    }

    public override string ToString() => Kind switch
    {
        NSObjectKind.Null => "null",
        NSObjectKind.Bool => ((bool)Value!).ToString(),
        NSObjectKind.Int => ((long)Value!).ToString(),
        NSObjectKind.UInt => ((ulong)Value!).ToString(),
        NSObjectKind.Double => ((double)Value!).ToString(),
        NSObjectKind.String => '"' + (string)Value! + '"',
        NSObjectKind.Data => "<data:" + ((byte[])Value!).Length + ">",
        NSObjectKind.Array => "[" + string.Join(", ", ((NSObject[])Value!).Select(item => item.ToString())) + "]",
        NSObjectKind.Dict => "{" + string.Join(", ",
            ((IReadOnlyDictionary<string, NSObject>)Value!).Select(kv => kv.Key + ": " + kv.Value)) + "}",
        _ => "<object>",
    };

    private static bool DictEquals(
        IReadOnlyDictionary<string, NSObject> left,
        IReadOnlyDictionary<string, NSObject> right)
    {
        if (left.Count != right.Count)
            return false;
        foreach (var item in left)
        {
            if (!right.TryGetValue(item.Key, out var value) || !item.Value.Equals(value))
                return false;
        }
        return true;
    }

    private static int CombineHash(int left, int right) => unchecked((left * 397) ^ right);
}

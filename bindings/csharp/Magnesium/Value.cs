using System;
using System.Runtime.InteropServices;

namespace Magnesium
{
    public readonly struct Value : IEquatable<Value>
    {
        private readonly ulong _bits;

        private Value(ulong bits) => _bits = bits;

        private const ulong QNAN     = 0x7ffc000000000000UL;
        private const ulong SIGN_BIT = 0x8000000000000000UL;
        private const ulong TAG_NULL  = 1;
        private const ulong TAG_FALSE = 2;
        private const ulong TAG_TRUE  = 3;
        private const ulong TAG_INT   = 4;

        public static Value Null => new Value(QNAN | TAG_NULL);
        public static Value False => new Value(QNAN | TAG_FALSE);
        public static Value True => new Value(QNAN | TAG_TRUE);

        public static Value BoolVal(bool b) => b ? True : False;

        public static Value IntVal(int n) =>
            new Value(QNAN | TAG_INT | ((ulong)(uint)n << 3));

        public static Value NumberVal(double n)
        {
            var bits = unchecked((ulong)BitConverter.DoubleToInt64Bits(n));
            if ((bits & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT))
                bits = unchecked((ulong)BitConverter.DoubleToInt64Bits(0.0));
            return new Value(bits);
        }

        public static Value ObjVal(IntPtr ptr) =>
            new Value(SIGN_BIT | QNAN | (ulong)ptr);

        public static Value AutoVal(double n)
        {
            if (n == Math.Truncate(n) && n >= int.MinValue && n <= int.MaxValue)
            {
                int i = (int)n;
                if ((double)i == n) return IntVal(i);
            }
            return NumberVal(n);
        }

        public static Value FromBits(ulong bits) => new Value(bits);
        public static Value FromRaw(ulong raw) => new Value(raw);

        public ulong ToBits() => _bits;
        public ulong ToRaw() => _bits;

        public bool IsNull => _bits == (QNAN | TAG_NULL);
        public bool IsBool => _bits == (QNAN | TAG_FALSE) || _bits == (QNAN | TAG_TRUE);
        public bool IsInt => (_bits & (QNAN | 0x7UL)) == (QNAN | TAG_INT);

        public bool IsNumber
        {
            get
            {
                if ((_bits & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT)) return false;
                double d = BitConverter.Int64BitsToDouble(unchecked((long)_bits));
                return !double.IsNaN(d);
            }
        }

        public bool IsNumeric => IsInt || IsNumber;

        public bool IsObj => (_bits & (SIGN_BIT | QNAN)) == (SIGN_BIT | QNAN) && (_bits & 0x7) == 0;

        public bool IsString => IsObj && ObjType == ObjTypeString;
        public bool IsArray => IsObj && ObjType == ObjTypeArray;
        public bool IsDict => IsObj && ObjType == ObjTypeDict;
        public bool IsNativeHandle => IsObj && ObjType == ObjTypeNativeHandle;

        public bool IsFalsey => IsNull || _bits == (QNAN | TAG_FALSE);

        public bool AsBool => _bits == (QNAN | TAG_TRUE);
        public int AsInt => (int)((_bits & ~(QNAN | 0x7UL)) >> 3);
        public double AsNumber => BitConverter.Int64BitsToDouble(unchecked((long)_bits));
        public double AsNumeric => IsInt ? (double)AsInt : AsNumber;
        public IntPtr AsObj => (IntPtr)(_bits & ~(SIGN_BIT | QNAN));

        private int ObjType => IsObj ? Marshal.ReadInt32(AsObj) : -1;

        private const int ObjTypeString = 0;
        private const int ObjTypeArray = 1;
        private const int ObjTypeDict = 2;
        private const int ObjTypeNativeHandle = 8;

        public bool? TryAsBool() => IsBool ? (bool?)AsBool : null;
        public int? TryAsInt() => IsInt ? (int?)AsInt : null;
        public double? TryAsNumber() => IsNumber ? (double?)AsNumber : null;
        public double? TryAsNumeric() => IsNumeric ? (double?)AsNumeric : null;

        public string? TryAsString()
        {
            if (!IsString) return null;
            var charsPtr = Native.vm_string_chars(_bits);
            int length = Native.vm_string_length(_bits);
            if (charsPtr == IntPtr.Zero || length <= 0) return null;
            return Marshal.PtrToStringAnsi(charsPtr, length);
        }

        public bool Equals(Value other) => _bits == other._bits;
        public override bool Equals(object? obj) => obj is Value v && Equals(v);
        public override int GetHashCode() => _bits.GetHashCode();
        public static bool operator ==(Value a, Value b) => a._bits == b._bits;
        public static bool operator !=(Value a, Value b) => a._bits != b._bits;
    }
}

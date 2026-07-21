using System;
using System.Runtime.InteropServices;

namespace Magnesium
{
    public class NativeContext
    {
        public Vm Vm { get; }
        private readonly Value[] _args;

        internal NativeContext(Vm vm, Value[] args)
        {
            Vm = vm;
            _args = args;
        }

        public int ArgCount => _args.Length;

        public Value? Arg(int index) =>
            index >= 0 && index < _args.Length ? _args[index] : null;

        public bool? ArgBool(int index) => Arg(index)?.TryAsBool();
        public int? ArgInt(int index) => Arg(index)?.TryAsInt();
        public double? ArgNumber(int index) => Arg(index)?.TryAsNumber();
        public double? ArgNumeric(int index) => Arg(index)?.TryAsNumeric();
        public string? ArgString(int index) => Arg(index)?.TryAsString();

        public Value ExpectArg(int index) =>
            Arg(index) ?? throw new InvalidOperationException($"Argument index {index} out of range");

        public bool ExpectBool(int index) => ArgBool(index) ?? throw new InvalidOperationException("Not a bool");
        public int ExpectInt(int index) => ArgInt(index) ?? throw new InvalidOperationException("Not an int");
        public double ExpectNumber(int index) => ArgNumber(index) ?? throw new InvalidOperationException("Not a number");
        public double ExpectNumeric(int index) => ArgNumeric(index) ?? throw new InvalidOperationException("Not numeric");
        public string ExpectString(int index) => ArgString(index) ?? throw new InvalidOperationException("Not a string");
    }
}

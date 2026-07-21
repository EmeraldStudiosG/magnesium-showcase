using System;

namespace Magnesium
{
    public enum InterpretErrorKind
    {
        CompileError,
        RuntimeError,
        Yield,
        HostError,
    }

    public class HostError : Exception
    {
        public HostErrorKind Kind { get; }
        public HostError(HostErrorKind kind, string message) : base(message) { Kind = kind; }

        public static HostError InteriorNul(string field) =>
            new HostError(HostErrorKind.InteriorNul, $"Interior nul byte in field: {field}");

        public static HostError AllocationFailed(string op) =>
            new HostError(HostErrorKind.AllocationFailed, $"Allocation failed: {op}");
    }

    public enum HostErrorKind
    {
        InteriorNul,
        AllocationFailed,
    }

    public class InterpretError : Exception
    {
        public InterpretErrorKind Kind { get; }
        public string? RuntimeErrorMessage { get; }

        public InterpretError(InterpretErrorKind kind, string message, string? runtimeErrMsg = null)
            : base(message)
        {
            Kind = kind;
            RuntimeErrorMessage = runtimeErrMsg;
        }
    }

    public readonly struct InterpretResult
    {
        public bool IsOk { get; }
        public bool IsErr => !IsOk;
        public InterpretError? Error { get; }

        private InterpretResult(bool ok, InterpretError? error = null)
        {
            IsOk = ok;
            Error = error;
        }

        public static InterpretResult Ok() => new InterpretResult(true);
        public static InterpretResult Err(InterpretError e) => new InterpretResult(false, e);

        public static InterpretResult FromRaw(int raw)
        {
            return raw switch
            {
                0 => Ok(),
                1 => Err(new InterpretError(InterpretErrorKind.CompileError, "Compile error")),
                2 => Err(new InterpretError(InterpretErrorKind.RuntimeError, "Runtime error")),
                3 => Err(new InterpretError(InterpretErrorKind.Yield, "Yield")),
                _ => Ok(),
            };
        }

        public static InterpretResult HostError(HostError e) =>
            Err(new InterpretError(InterpretErrorKind.HostError, e.Message));
    }
}

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace Magnesium
{
    public class Vm : IDisposable
    {
        private IntPtr _raw;
        private bool _disposed;
        private readonly List<Delegate> _nativeDelegates = new();
        private readonly List<NativeClosureWrap> _nativeClosures = new();
        internal bool IsDisposed => _disposed;

        public IntPtr RawPtr
        {
            get
            {
                ThrowIfDisposed();
                return _raw;
            }
        }

        public Vm()
        {
            _raw = Native.vm_new();
            if (_raw == IntPtr.Zero)
                throw new InvalidOperationException("Failed to allocate VM");
        }

        ~Vm() => Dispose(false);

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        protected virtual void Dispose(bool disposing)
        {
            if (!_disposed)
            {
                if (_raw != IntPtr.Zero)
                {
                    Native.vm_delete(_raw);
                    _raw = IntPtr.Zero;
                }
                _nativeDelegates.Clear();
                _nativeClosures.Clear();
                _disposed = true;
            }
        }

        public InterpretResult Interpret(string source)
        {
            ThrowIfDisposed();
            if (source.IndexOf('\0') >= 0)
                return InterpretResult.HostError(HostError.InteriorNul("source"));
            int raw = Native.vm_interpret(_raw, source);
            return SnapshotInterpretResult(raw);
        }

        public InterpretResult InterpretNamed(string source, string name)
        {
            ThrowIfDisposed();
            if (source.IndexOf('\0') >= 0)
                return InterpretResult.HostError(HostError.InteriorNul("source"));
            if (name.IndexOf('\0') >= 0)
                return InterpretResult.HostError(HostError.InteriorNul("name"));
            int raw = Native.vm_interpret_named(_raw, source, name);
            return SnapshotInterpretResult(raw);
        }

        public IntPtr? Compile(string source)
        {
            ThrowIfDisposed();
            if (source.IndexOf('\0') >= 0) return null;
            var fn = Native.vm_compile(_raw, source);
            return fn != IntPtr.Zero ? fn : null;
        }

        public InterpretResult RunFunction(IntPtr fn)
        {
            ThrowIfDisposed();
            int raw = Native.vm_run_function(_raw, fn);
            return SnapshotInterpretResult(raw);
        }

        public void Push(Value v)
        {
            ThrowIfDisposed();
            Native.vm_push(_raw, v.ToRaw());
        }

        public Value Pop()
        {
            ThrowIfDisposed();
            return Value.FromRaw(Native.vm_pop(_raw));
        }

        public GcRoot Root(Value value)
        {
            ThrowIfDisposed();
            return new GcRoot(this, value);
        }

        public void CollectGarbage()
        {
            ThrowIfDisposed();
            Native.gc_major_collect(_raw);
        }

        public void SetGlobal(string name, Value v)
        {
            ThrowIfDisposed();
            if (name.IndexOf('\0') >= 0) return;
            Native.vm_set_global_value(_raw, name, v.ToRaw());
        }

        public Value? GetGlobal(string name)
        {
            ThrowIfDisposed();
            if (name.IndexOf('\0') >= 0) return null;
            if (Native.vm_get_global_value(_raw, name, out ulong val))
                return Value.FromRaw(val);
            return null;
        }

        public void SetGlobalNumber(string name, double v) => SetGlobal(name, Value.AutoVal(v));
        public void SetGlobalBool(string name, bool v) => SetGlobal(name, Value.BoolVal(v));

        public void SetGlobalString(string name, string v)
        {
            ThrowIfDisposed();
            if (name.IndexOf('\0') >= 0 || v.IndexOf('\0') >= 0) return;
            int byteLength = Encoding.UTF8.GetByteCount(v);
            var stringObject = Native.copy_string(_raw, v, byteLength);
            if (stringObject == IntPtr.Zero) return;
            var val = Value.ObjVal(stringObject);
            Native.vm_set_global_value(_raw, name, val.ToRaw());
        }

        public unsafe string? CopyString(Value value)
        {
            ThrowIfDisposed();
            if (!value.IsString) return null;

            Native.vm_string_copy(
                _raw, value.ToRaw(), IntPtr.Zero, UIntPtr.Zero, out UIntPtr required);
            ulong requiredBytes = required.ToUInt64();
            if (requiredBytes == 0 || requiredBytes > int.MaxValue) return null;

            var bytes = new byte[(int)requiredBytes];
            fixed (byte* buffer = bytes)
            {
                if (!Native.vm_string_copy(
                        _raw,
                        value.ToRaw(),
                        (IntPtr)buffer,
                        (UIntPtr)bytes.Length,
                        out UIntPtr copiedRequired) ||
                    copiedRequired.ToUInt64() != requiredBytes)
                    return null;
            }
            return Encoding.UTF8.GetString(bytes, 0, bytes.Length - 1);
        }

        public void RegisterNative(string name, Native.NativeFnDelegate func, int arity)
        {
            ThrowIfDisposed();
            if (name.IndexOf('\0') >= 0) return;
            Native.NativeFnDelegate contained = (vm, argCount, args) =>
                InvokeContained(vm, () => func(vm, argCount, args));
            _nativeDelegates.Add(contained);
            Native.vm_register_native(_raw, name, contained, arity, IntPtr.Zero, null);
        }

        public void RegisterNativeFn(string name, int arity, Func<NativeContext, Value> f)
        {
            ThrowIfDisposed();
            if (name.IndexOf('\0') >= 0) return;
            var wrap = new NativeClosureWrap(this, f);
            _nativeClosures.Add(wrap);
            var handle = GCHandle.Alloc(wrap, GCHandleType.Weak);
            var userdata = GCHandle.ToIntPtr(handle);
            Native.vm_register_native(_raw, name, _trampoline, arity, userdata, _closureFinalizer);
        }

        public Value NewNativeHandle<T>(string type_name, T data) where T : class
        {
            ThrowIfDisposed();
            if (type_name.IndexOf('\0') >= 0) return Value.Null;
            var handle = GCHandle.Alloc(data);
            var ptr = GCHandle.ToIntPtr(handle);
            var result = Native.vm_new_native_handle(_raw, type_name, ptr,
                _handleFinalizer);
            if (result == IntPtr.Zero)
            {
                handle.Free();
                return Value.Null;
            }
            return Value.FromRaw(Native.vm_native_handle_value(result));
        }

        public void SetNativeHandleMethod(Value handle, string name,
            Native.NativeFnDelegate func, int arity)
        {
            ThrowIfDisposed();
            if (!handle.IsNativeHandle) return;
            if (name.IndexOf('\0') >= 0) return;
            Native.NativeFnDelegate contained = (vm, argCount, args) =>
                InvokeContained(vm, () => func(vm, argCount, args));
            _nativeDelegates.Add(contained);
            Native.vm_native_handle_set_method(_raw, handle.AsObj, name,
                contained, arity, IntPtr.Zero, null);
        }

        public Value NewArrayValue()
        {
            ThrowIfDisposed();
            var arr = Native.new_array(_raw);
            return arr != IntPtr.Zero ? Value.ObjVal(arr) : Value.Null;
        }

        public Value NewDictValue()
        {
            ThrowIfDisposed();
            var d = Native.new_dict(_raw);
            return d != IntPtr.Zero ? Value.ObjVal(d) : Value.Null;
        }

        public string LastErrorMessage
        {
            get
            {
                ThrowIfDisposed();
                var ptr = Native.vm_last_error(_raw);
                return ptr != IntPtr.Zero ? Marshal.PtrToStringUTF8(ptr) ?? "" : "";
            }
        }

        public int LastErrorLine
        {
            get
            {
                ThrowIfDisposed();
                return Native.vm_last_error_line(_raw);
            }
        }

        public void ClearError()
        {
            ThrowIfDisposed();
            Native.vm_clear_error(_raw);
        }

        public int FrameCount
        {
            get
            {
                ThrowIfDisposed();
                return Native.vm_frame_count(_raw);
            }
        }

        public long BytesAllocated
        {
            get
            {
                ThrowIfDisposed();
                return checked((long)Native.vm_bytes_allocated(_raw).ToUInt64());
            }
        }

        private static readonly Native.NativeFnDelegate _trampoline = Trampoline;
        private static readonly Native.NativeFinalizerDelegate _closureFinalizer = ClosureFinalizer;
        private static readonly Native.NativeHandleFinalizerDelegate _handleFinalizer = HandleFinalizer;

        private InterpretResult SnapshotInterpretResult(int raw)
        {
            string? runtimeErrorMessage = raw == 2 ? LastErrorMessage : null;
            return InterpretResult.FromRaw(raw, runtimeErrorMessage);
        }

        private static unsafe ulong Trampoline(IntPtr vm, int argCount, IntPtr args)
        {
            return InvokeContained(vm, () =>
            {
                var userdata = Native.mg_get_native_userdata(vm);
                if (userdata == IntPtr.Zero) return Value.Null.ToRaw();

                var handle = GCHandle.FromIntPtr(userdata);
                if (handle.Target is not NativeClosureWrap wrap ||
                    !wrap.Owner.TryGetTarget(out var owner))
                    return Value.Null.ToRaw();

                var vArgs = new Value[argCount > 0 ? argCount : 0];
                if (argCount > 0 && args != IntPtr.Zero)
                {
                    var ptr = (ulong*)args;
                    for (int i = 0; i < argCount; i++)
                        vArgs[i] = Value.FromRaw(ptr[i]);
                }

                var ctx = new NativeContext(owner, vArgs);
                return wrap.Fn(ctx).ToRaw();
            });
        }

        private static ulong InvokeContained(IntPtr vm, Func<ulong> callback)
        {
            try
            {
                return callback();
            }
            catch (Exception error)
            {
                try
                {
                    string message = $"managed callback failed: {error.Message}".Replace('\0', ' ');
                    Native.mg_runtime_error_simple(vm, message);
                }
                catch
                {
                    // No managed exception may escape a reverse P/Invoke boundary.
                }
                return Value.Null.ToRaw();
            }
        }

        private static void ClosureFinalizer(IntPtr data)
        {
            if (data == IntPtr.Zero) return;
            var handle = GCHandle.FromIntPtr(data);
            handle.Free();
        }

        private static void HandleFinalizer(IntPtr data)
        {
            if (data == IntPtr.Zero) return;
            var handle = GCHandle.FromIntPtr(data);
            handle.Free();
        }

        private class NativeClosureWrap
        {
            public WeakReference<Vm> Owner;
            public Func<NativeContext, Value> Fn;
            public NativeClosureWrap(Vm owner, Func<NativeContext, Value> fn)
            {
                Owner = new WeakReference<Vm>(owner);
                Fn = fn;
            }
        }

        private void ThrowIfDisposed()
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
        }
    }
}

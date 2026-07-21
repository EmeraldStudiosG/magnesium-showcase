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

        public IntPtr RawPtr => _raw;

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
                _disposed = true;
            }
        }

        public InterpretResult Interpret(string source)
        {
            if (source.IndexOf('\0') >= 0)
                return InterpretResult.HostError(HostError.InteriorNul("source"));
            int raw = Native.vm_interpret(_raw, source);
            return InterpretResult.FromRaw(raw);
        }

        public InterpretResult InterpretNamed(string source, string name)
        {
            if (source.IndexOf('\0') >= 0)
                return InterpretResult.HostError(HostError.InteriorNul("source"));
            int raw = Native.vm_interpret_named(_raw, source, name);
            return InterpretResult.FromRaw(raw);
        }

        public IntPtr? Compile(string source)
        {
            if (source.IndexOf('\0') >= 0) return null;
            var fn = Native.vm_compile(_raw, source);
            return fn != IntPtr.Zero ? fn : null;
        }

        public InterpretResult RunFunction(IntPtr fn)
        {
            int raw = Native.vm_run_function(_raw, fn);
            return InterpretResult.FromRaw(raw);
        }

        public void Push(Value v) => Native.vm_push(_raw, v.ToRaw());
        public Value Pop() => Value.FromRaw(Native.vm_pop(_raw));

        public void SetGlobal(string name, Value v)
        {
            if (name.IndexOf('\0') >= 0) return;
            Native.vm_set_global_value(_raw, name, v.ToRaw());
        }

        public Value? GetGlobal(string name)
        {
            if (name.IndexOf('\0') >= 0) return null;
            if (Native.vm_get_global_value(_raw, name, out ulong val))
                return Value.FromRaw(val);
            return null;
        }

        public void SetGlobalNumber(string name, double v) => SetGlobal(name, Value.AutoVal(v));
        public void SetGlobalBool(string name, bool v) => SetGlobal(name, Value.BoolVal(v));

        public void SetGlobalString(string name, string v)
        {
            if (name.IndexOf('\0') >= 0) return;
            var val = Value.ObjVal(Native.copy_string(_raw, v, v.Length));
            Native.vm_set_global_value(_raw, name, val.ToRaw());
        }

        public void RegisterNative(string name, Native.NativeFnDelegate func, int arity)
        {
            if (name.IndexOf('\0') >= 0) return;
            Native.vm_register_native(_raw, name, func, arity, IntPtr.Zero, null!);
        }

        public void RegisterNativeFn(string name, int arity, Func<NativeContext, Value> f)
        {
            if (name.IndexOf('\0') >= 0) return;
            var wrap = new NativeClosureWrap(this, f);
            var handle = GCHandle.Alloc(wrap);
            var userdata = GCHandle.ToIntPtr(handle);
            Native.vm_register_native(_raw, name, _trampoline, arity, userdata, _closureFinalizer);
        }

        public Value NewNativeHandle<T>(string type_name, T data) where T : class
        {
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
            if (!handle.IsNativeHandle) return;
            if (name.IndexOf('\0') >= 0) return;
            Native.vm_native_handle_set_method(_raw, handle.AsObj, name,
                func, arity, IntPtr.Zero, null!);
        }

        public Value NewArrayValue()
        {
            var arr = Native.new_array(_raw);
            return arr != IntPtr.Zero ? Value.ObjVal(arr) : Value.Null;
        }

        public Value NewDictValue()
        {
            var d = Native.new_dict(_raw);
            return d != IntPtr.Zero ? Value.ObjVal(d) : Value.Null;
        }

        public string LastErrorMessage
        {
            get
            {
                var ptr = Native.vm_last_error(_raw);
                return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) ?? "" : "";
            }
        }

        public int LastErrorLine => Native.vm_last_error_line(_raw);
        public void ClearError() => Native.vm_clear_error(_raw);

        public int FrameCount => Native.vm_frame_count(_raw);
        public long BytesAllocated => (long)Native.vm_bytes_allocated(_raw);

        private static readonly Native.NativeFnDelegate _trampoline = Trampoline;
        private static readonly Native.NativeFinalizerDelegate _closureFinalizer = ClosureFinalizer;
        private static readonly Native.NativeHandleFinalizerDelegate _handleFinalizer = HandleFinalizer;

        private static readonly List<object> _pinnedDelegates = new();

        private static unsafe ulong Trampoline(IntPtr vm, int argCount, IntPtr args)
        {
            var userdata = Native.mg_get_native_userdata(vm);
            if (userdata == IntPtr.Zero) return Value.Null.ToRaw();

            var handle = GCHandle.FromIntPtr(userdata);
            var wrap = (NativeClosureWrap)handle.Target!;

            var vArgs = new Value[argCount > 0 ? argCount : 0];
            if (argCount > 0 && args != IntPtr.Zero)
            {
                unsafe
                {
                    var ptr = (ulong*)args;
                    for (int i = 0; i < argCount; i++)
                    {
                        vArgs[i] = Value.FromRaw(ptr[i]);
                    }
                }
            }

            var ctx = new NativeContext(wrap.Owner, vArgs);
            return wrap.Fn(ctx).ToRaw();
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
            public Vm Owner;
            public Func<NativeContext, Value> Fn;
            public NativeClosureWrap(Vm owner, Func<NativeContext, Value> fn)
            {
                Owner = owner;
                Fn = fn;
            }
        }
    }
}

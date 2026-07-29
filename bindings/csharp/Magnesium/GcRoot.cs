using System;

namespace Magnesium
{
    public sealed class GcRoot : IDisposable
    {
        private Vm? _vm;
        private IntPtr _root;

        internal GcRoot(Vm vm, Value value)
        {
            _vm = vm;
            _root = Native.vm_root_value(vm.RawPtr, value.ToRaw());
            if (_root == IntPtr.Zero)
            {
                _vm = null;
                throw new OutOfMemoryException("Failed to allocate a Magnesium VM root");
            }
        }

        public Value Value
        {
            get
            {
                EnsureUsable();
                return Value.FromRaw(Native.vm_root_get(_root));
            }
            set
            {
                EnsureUsable();
                if (!Native.vm_root_set(_vm!.RawPtr, _root, value.ToRaw()))
                    throw new InvalidOperationException("The VM root is no longer registered");
            }
        }

        public void Dispose()
        {
            if (_root != IntPtr.Zero && _vm is { IsDisposed: false } vm)
                Native.vm_unroot_value(vm.RawPtr, _root);
            _root = IntPtr.Zero;
            _vm = null;
            GC.SuppressFinalize(this);
        }

        ~GcRoot() => Dispose();

        private void EnsureUsable()
        {
            if (_root == IntPtr.Zero || _vm is null || _vm.IsDisposed)
                throw new ObjectDisposedException(nameof(GcRoot));
        }
    }
}

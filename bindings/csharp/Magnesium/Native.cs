using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Magnesium
{
    public static class Native
    {
        // Matches the canonical library name produced by `make install` and
        // install.ps1 (libmagnesium.{so,dylib,dll}). The bare "magnesium"
        // name collided with the managed Magnesium.dll assembly on Windows.
        private const string DllName = "libmagnesium";

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr vm_new();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void vm_delete(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int vm_interpret(
            IntPtr vm, [MarshalAs(UnmanagedType.LPUTF8Str)] string source);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int vm_interpret_named(
            IntPtr vm,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string source,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr vm_compile(
            IntPtr vm, [MarshalAs(UnmanagedType.LPUTF8Str)] string source);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr vm_compile_named(
            IntPtr vm,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string source,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int vm_run_function(IntPtr vm, IntPtr function);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void vm_push(IntPtr vm, ulong value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ulong vm_pop(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr vm_root_value(IntPtr vm, ulong value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool vm_root_set(IntPtr vm, IntPtr root, ulong value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ulong vm_root_get(IntPtr root);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void vm_unroot_value(IntPtr vm, IntPtr root);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void gc_major_collect(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool vm_set_global_value(
            IntPtr vm, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, ulong value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool vm_get_global_value(
            IntPtr vm, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, out ulong value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void vm_register_native(
            IntPtr vm,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            NativeFnDelegate function,
            int arity,
            IntPtr userdata,
            NativeFinalizerDelegate? finalizer);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr vm_new_native_handle(
            IntPtr vm,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string type_name,
            IntPtr data,
            NativeHandleFinalizerDelegate finalizer);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool vm_native_handle_set_method(
            IntPtr vm,
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            NativeFnDelegate function,
            int arity,
            IntPtr userdata,
            NativeFinalizerDelegate? finalizer);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr vm_native_handle_data(
            ulong value, [MarshalAs(UnmanagedType.LPUTF8Str)] string type_name);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ulong vm_native_handle_value(IntPtr handle);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr new_array(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr new_dict(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr copy_string(
            IntPtr vm, [MarshalAs(UnmanagedType.LPUTF8Str)] string chars, int length);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void mg_runtime_error_simple(
            IntPtr vm, [MarshalAs(UnmanagedType.LPUTF8Str)] string message);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void vm_clear_error(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr vm_last_error(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int vm_last_error_line(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int vm_frame_count(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern UIntPtr vm_bytes_allocated(IntPtr vm);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr vm_string_chars(ulong value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int vm_string_length(ulong value);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool vm_string_copy(
            IntPtr vm,
            ulong value,
            IntPtr buffer,
            UIntPtr capacity,
            out UIntPtr required);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr mg_get_native_userdata(IntPtr vm);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate ulong NativeFnDelegate(IntPtr vm, int arg_count, IntPtr args);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void NativeFinalizerDelegate(IntPtr data);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void NativeHandleFinalizerDelegate(IntPtr data);
    }
}

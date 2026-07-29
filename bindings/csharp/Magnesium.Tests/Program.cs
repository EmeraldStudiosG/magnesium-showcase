using System;
using System.Runtime.InteropServices;
using Magnesium;

class Program
{
    static int PassCount = 0;
    static int FailCount = 0;

    static void Run(string name, Action test)
    {
        Console.Write($"  {name} ... ");
        try
        {
            test();
            Console.WriteLine("OK");
            PassCount++;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"FAILED: {ex.Message}");
            FailCount++;
        }
    }

    static void Assert(bool cond, string msg = "assertion failed")
    {
        if (!cond) throw new Exception(msg);
    }

    static double Abs(double v) => Math.Abs(v);

    static void Main()
    {
        // Value tests
        Run("value_null", () => { var v = Value.Null; Assert(v.IsNull); Assert(!v.IsBool); Assert(!v.IsNumber); Assert(!v.IsInt); });
        Run("value_bool", () => { var t = Value.BoolVal(true); var f = Value.False; Assert(t.IsBool); Assert(t.AsBool); Assert(!f.AsBool); });
        Run("value_int", () => { var v = Value.IntVal(42); Assert(v.IsInt); Assert(!v.IsNumber); Assert(v.IsNumeric); Assert(v.AsInt == 42); Assert(v.AsNumeric == 42.0); });
        Run("value_float", () => { var v = Value.NumberVal(4.56); Assert(v.IsNumber); Assert(v.IsNumeric); Assert(!v.IsInt); Assert(Abs(v.AsNumber - 4.56) < 1e-10); });
        Run("value_auto_val", () => { var iv = Value.AutoVal(5.0); Assert(iv.IsInt); Assert(iv.AsInt == 5); var dv = Value.AutoVal(5.5); Assert(!dv.IsInt); Assert(dv.IsNumber); });
        Run("value_falsey", () => { Assert(Value.Null.IsFalsey); Assert(Value.False.IsFalsey); Assert(!Value.True.IsFalsey); Assert(!Value.IntVal(0).IsFalsey); });
        Run("value_try_as", () => { Assert(Value.BoolVal(true).TryAsBool() == true); Assert(Value.IntVal(42).TryAsInt() == 42); Assert(Value.Null.TryAsBool() == null); Assert(Value.Null.TryAsInt() == null); Assert(Value.IntVal(7).TryAsNumeric() == 7.0); Assert(Value.Null.TryAsNumeric() == null); });
        Run("value_equality", () => { Assert(Value.Null == Value.Null); Assert(Value.IntVal(1) == Value.IntVal(1)); Assert(Value.IntVal(1) != Value.IntVal(2)); Assert(Value.Null != Value.False); });
        Run("value_is_obj_excludes_tags", () => { Assert(!Value.Null.IsObj); Assert(!Value.True.IsObj); Assert(!Value.False.IsObj); Assert(!Value.IntVal(0).IsObj); });
        Run("value_bits_roundtrip", () => { var v = Value.IntVal(-7); var v2 = Value.FromBits(v.ToBits()); Assert(v2.AsInt == -7); });

        // Vm tests
        Run("vm_create_and_drop", () => { using var vm = new Vm(); });
        Run("interpret_hello", () => { using var vm = new Vm(); Assert(vm.Interpret("print(\"hello from C#\")").IsOk); });
        Run("interpret_arithmetic", () => { using var vm = new Vm(); Assert(vm.Interpret("let x = 2 + 3").IsOk); });
        Run("interpret_compile_error", () => { using var vm = new Vm(); Assert(vm.Interpret("let x = ").IsErr); });
        Run("interpret_rejects_nul", () => { using var vm = new Vm(); Assert(vm.Interpret("print\0bad").IsErr); });
        Run("interpret_named", () => { using var vm = new Vm(); Assert(vm.InterpretNamed("print(1)", "test").IsOk); });
        Run("push_pop", () => { using var vm = new Vm(); vm.Push(Value.IntVal(100)); Assert(vm.Pop().AsInt == 100); });
        Run("set_get_global", () => { using var vm = new Vm(); vm.SetGlobal("x", Value.IntVal(42)); var v = vm.GetGlobal("x"); Assert(v != null && v.Value.AsInt == 42); });
        Run("get_global_missing", () => { using var vm = new Vm(); Assert(vm.GetGlobal("nonexistent") == null); });
        Run("compile", () => { using var vm = new Vm(); Assert(vm.Compile("let x = 1 + 2") != null); });
        Run("compile_and_run", () => { using var vm = new Vm(); var fn = vm.Compile("let x = 1 + 2"); Assert(fn != null); Assert(vm.RunFunction(fn.Value).IsOk); });
        Run("extern_with_host_global", () => { using var vm = new Vm(); vm.SetGlobalNumber("HOST_SCORE", 42.0); Assert(vm.Interpret("!strict\nextern const HOST_SCORE: number\nlet s: number = HOST_SCORE\nprint(s)").IsOk); });
        Run("set_global_number_and_bool", () => { using var vm = new Vm(); vm.SetGlobalNumber("pi", 3.14); vm.SetGlobalBool("flag", true); Assert(vm.GetGlobal("pi")!.Value.IsNumeric); Assert(vm.GetGlobal("flag")!.Value.AsBool); });
        Run("new_array_value", () => { using var vm = new Vm(); Assert(vm.NewArrayValue().IsArray); });
        Run("new_dict_value", () => { using var vm = new Vm(); Assert(vm.NewDictValue().IsDict); });
        Run("clear_error", () => { using var vm = new Vm(); vm.Interpret("let x = \"a\" + 1"); vm.ClearError(); });
        Run("runtime_error_message", () => { using var vm = new Vm(); vm.Interpret("let x = \"a\" + 1"); Assert(vm.LastErrorLine > 0); });
        Run("interpret_error_message_snapshot", () => {
            using var vm = new Vm();
            var result = vm.Interpret("let interpret_failure = \"a\" + 1");
            Assert(result.IsErr);
            Assert(result.Error?.Kind == InterpretErrorKind.RuntimeError);
            string snapshot = result.Error?.RuntimeErrorMessage ?? "";
            Assert(snapshot.Length > 0);
            Assert(vm.Interpret("let interpret_recovery = 1").IsOk);
            Assert(result.Error?.RuntimeErrorMessage == snapshot);
        });
        Run("interpret_named_error_message_snapshot", () => {
            using var vm = new Vm();
            var result = vm.InterpretNamed(
                "let named_failure = \"a\" + 1", "named_snapshot");
            Assert(result.IsErr);
            string snapshot = result.Error?.RuntimeErrorMessage ?? "";
            Assert(snapshot.Length > 0);
            vm.ClearError();
            Assert(result.Error?.RuntimeErrorMessage == snapshot);
        });
        Run("run_function_error_message_snapshot", () => {
            using var vm = new Vm();
            var function = vm.Compile("let function_failure = \"a\" + 1");
            Assert(function != null);
            var result = vm.RunFunction(function.Value);
            Assert(result.IsErr);
            string snapshot = result.Error?.RuntimeErrorMessage ?? "";
            Assert(snapshot.Length > 0);
            Assert(vm.Interpret("let function_recovery = 1").IsOk);
            Assert(result.Error?.RuntimeErrorMessage == snapshot);
        });
        Run("vm_introspection_frame_count", () => {
            using var vm = new Vm();
            Assert(vm.FrameCount == 0);
        });
        Run("vm_introspection_bytes_allocated", () => {
            using var vm = new Vm();
            long before = vm.BytesAllocated;
            Assert(before >= 0);
            Assert(vm.Interpret("let x = \"hello\" + \" world\"").IsOk);
            long after = vm.BytesAllocated;
            Assert(after >= before);
        });
        Run("value_string_roundtrip", () => {
            using var vm = new Vm();
            vm.SetGlobalString("name", "hello world");
            var v = vm.GetGlobal("name");
            Assert(v != null);
            Assert(v.Value.IsString);
            Assert(v.Value.TryAsString() == "hello world");
        });
        Run("value_string_utf8_roundtrip", () => {
            using var vm = new Vm();
            const string expected = "\u0130stanbul \U0001F9EA";
            vm.SetGlobalString("\u015Fehir", expected);
            var v = vm.GetGlobal("\u015Fehir");
            Assert(v != null && v.Value.TryAsString() == expected);
        });
        Run("value_empty_string_roundtrip", () => {
            using var vm = new Vm();
            vm.SetGlobalString("empty", "");
            var v = vm.GetGlobal("empty");
            Assert(v != null && v.Value.TryAsString() == "");
        });
        Run("value_string_rejects_nul", () => {
            using var vm = new Vm();
            vm.SetGlobalString("bad", "a\0b");
            Assert(vm.GetGlobal("bad") == null);
        });
        Run("value_rope_string_roundtrip", () => {
            using var vm = new Vm();
            Assert(vm.Interpret("let @joined = \"hello \" + \"world\"").IsOk);
            var joined = vm.GetGlobal("joined");
            Assert(joined != null, "joined global is missing");
            string resolved = joined.Value.TryAsString(vm) ?? "<null>";
            Assert(resolved == "hello world",
                $"expected resolved rope, got {resolved}");
        });
        Run("gc_roots_are_not_lifo", () => {
            using var vm = new Vm();
            var first = vm.Root(vm.NewArrayValue());
            using var second = vm.Root(vm.NewArrayValue());
            first.Dispose();
            vm.CollectGarbage();
            Assert(second.Value.IsArray);
            second.Value = vm.NewDictValue();
            vm.CollectGarbage();
            Assert(second.Value.IsDict);
        });

        // Native tests
        Run("register_native_fn", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("double", 1, ctx => Value.AutoVal(ctx.ExpectNumeric(0) * 2.0));
            Assert(vm.Interpret("let x = double(21)\nprint(x)").IsOk);
        });

        Run("register_native_fn_multi_args", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("add_nums", 2, ctx => Value.AutoVal(ctx.ExpectNumeric(0) + ctx.ExpectNumeric(1)));
            Assert(vm.Interpret("let x = add_nums(3, 4)\nprint(x)").IsOk);
        });

        Run("native_context_string_arg", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("is_hello", 1, ctx => Value.BoolVal(ctx.ArgString(0) == "hello"));
            Assert(vm.Interpret("let ok = is_hello(\"hel\" + \"lo\")\nprint(ok)").IsOk);
        });

        Run("native_fn_bool_arg", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("invert", 1, ctx => Value.BoolVal(!ctx.ExpectBool(0)));
            Assert(vm.Interpret("let x = invert(true)\nprint(x)").IsOk);
        });

        Run("native_fn_int_arg", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("double_it", 1, ctx => { var n = ctx.ArgInt(0); return n.HasValue ? Value.IntVal(n.Value * 2) : Value.Null; });
            Assert(vm.Interpret("let x = double_it(21)\nprint(x)").IsOk);
        });

        Run("native_fn_numeric_arg", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("add_half", 1, ctx => { var n = ctx.ArgNumeric(0); return n.HasValue ? Value.AutoVal(n.Value + 0.5) : Value.Null; });
            Assert(vm.Interpret("let x = add_half(10)\nprint(x)").IsOk);
        });

        Run("native_fn_no_args", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("the_answer", 0, _ => Value.IntVal(42));
            Assert(vm.Interpret("let x = the_answer()\nprint(x)").IsOk);
        });

        Run("native_fn_access_vm", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("get_global_x", 0, ctx => ctx.Vm.GetGlobal("x") ?? Value.Null);
            vm.SetGlobal("x", Value.IntVal(77));
            Assert(vm.Interpret("let y = get_global_x()\nprint(y)").IsOk);
        });
        Run("native_delegate_survives_gc", () => {
            using var vm = new Vm();
            Native.NativeFnDelegate callback = (_, _, _) => Value.IntVal(42).ToRaw();
            vm.RegisterNative("answer_after_gc", callback, 0);
            callback = null!;
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            Assert(vm.Interpret("let answer = answer_after_gc()\nprint(answer)").IsOk);
        });
        Run("native_closure_survives_gc", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("closure_after_gc", 0, _ => Value.IntVal(7));
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            Assert(vm.Interpret("let answer = closure_after_gc()\nprint(answer)").IsOk);
        });
        Run("native_exception_is_contained", () => {
            using var vm = new Vm();
            vm.RegisterNativeFn("managed_failure", 0,
                _ => throw new InvalidOperationException("expected managed failure"));
            vm.Interpret("managed_failure()");
            Assert(vm.LastErrorMessage.Contains("expected managed failure"));
        });

        Run("native_handle_basic", () => {
            using var vm = new Vm();
            var obj = new CounterObj { Value = 42 };
            var handle = vm.NewNativeHandle<CounterObj>("Counter", obj);
            Assert(handle.IsNativeHandle);
            vm.SetGlobal("counter", handle);
            Assert(vm.Interpret("let x = counter\nprint(x)").IsOk);
            var data = vm.GetGlobal("counter");
            Assert(data != null);
            Assert(data.Value.IsNativeHandle);
        });
        Run("disposed_vm_throws", () => {
            var vm = new Vm();
            vm.Dispose();
            bool threw = false;
            try { vm.Interpret("print(1)"); }
            catch (ObjectDisposedException) { threw = true; }
            Assert(threw);
        });

        Console.WriteLine();
        int total = PassCount + FailCount;
        Console.WriteLine($"Results: {total} tests | {PassCount} passed | {FailCount} failed");
        Environment.Exit(FailCount);
    }
}

class FinalizableObj
{
    private readonly Action _onFinalize;
    public FinalizableObj(Action onFinalize) { _onFinalize = onFinalize; }
    ~FinalizableObj() { _onFinalize(); }
}

class CounterObj
{
    public int Value { get; set; }
}

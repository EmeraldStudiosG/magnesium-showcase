#include <mg/magnesium.hpp>
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <atomic>

static void test_vm_create_and_drop() {
    mg::Vm vm;
    // implicit drop at scope end
}

static void test_interpret_hello() {
    mg::Vm vm;
    auto result = vm.interpret("print(\"hello from C++\")");
    assert(result.is_ok());
}

static void test_value_null() {
    auto v = mg::Value::null();
    assert(v.is_null());
    assert(!v.is_bool());
    assert(!v.is_number());
    assert(!v.is_int());
}

static void test_value_bool() {
    auto t = mg::Value::bool_val(true);
    auto f = mg::Value::bool_val(false);
    assert(t.is_bool());
    assert(t.as_bool());
    assert(f.is_bool());
    assert(!f.as_bool());
}

static void test_value_int() {
    auto v = mg::Value::int_val(42);
    assert(v.is_int());
    assert(!v.is_number());
    assert(v.is_numeric());
    assert(v.as_int() == 42);
    assert(v.as_numeric() == 42.0);
}

static void test_value_float() {
    auto v = mg::Value::number_val(4.56);
    assert(v.is_number());
    assert(v.is_numeric());
    assert(!v.is_int());
    assert(std::abs(v.as_number() - 4.56) < 1e-10);
}

static void test_value_auto_val() {
    auto v = mg::Value::auto_val(5.0);
    assert(v.is_int());
    assert(v.as_int() == 5);

    auto v2 = mg::Value::auto_val(5.5);
    assert(!v2.is_int());
    assert(v2.is_number());
}

static void test_value_falsey() {
    assert(mg::Value::null().is_falsey());
    assert(mg::Value::bool_val(false).is_falsey());
    assert(!mg::Value::bool_val(true).is_falsey());
    assert(!mg::Value::int_val(0).is_falsey());
}

static void test_try_as_typed_methods() {
    assert(mg::Value::bool_val(true).try_as_bool() == true);
    assert(mg::Value::bool_val(false).try_as_bool().value() == false);
    assert(!mg::Value::null().try_as_bool().has_value());

    assert(mg::Value::int_val(42).try_as_int().value() == 42);
    assert(!mg::Value::number_val(3.14).try_as_int().has_value());
    assert(!mg::Value::null().try_as_int().has_value());

    assert(std::abs(mg::Value::number_val(2.718).try_as_number().value() - 2.718) < 1e-10);
    assert(!mg::Value::int_val(5).try_as_number().has_value());

    assert(mg::Value::int_val(7).try_as_numeric().value() == 7.0);
    assert(std::abs(mg::Value::number_val(1.5).try_as_numeric().value() - 1.5) < 1e-10);
    assert(!mg::Value::null().try_as_numeric().has_value());

    assert(mg::Value::int_val(42).expect_int() == 42);
}

static void test_expect_methods() {
    assert(mg::Value::bool_val(true).expect_bool() == true);
    assert(mg::Value::int_val(99).expect_int() == 99);
    assert(std::abs(mg::Value::number_val(3.14).expect_number() - 3.14) < 1e-10);
    assert(mg::Value::int_val(5).expect_numeric() == 5.0);
    assert(std::abs(mg::Value::number_val(2.5).expect_numeric() - 2.5) < 1e-10);
}

static void test_push_pop() {
    mg::Vm vm;
    vm.push(mg::Value::int_val(100));
    auto v = vm.pop();
    assert(v.as_int() == 100);
}

static void test_set_get_global() {
    mg::Vm vm;
    vm.set_global("x", mg::Value::int_val(42));
    auto val = vm.get_global("x");
    assert(val.has_value());
    assert(val->as_int() == 42);
}

static void test_get_global_missing() {
    mg::Vm vm;
    assert(!vm.get_global("nonexistent").has_value());
}

static void test_compile() {
    mg::Vm vm;
    auto func = vm.compile("let x = 1 + 2");
    assert(func.has_value());
}

static void test_compile_and_run() {
    mg::Vm vm;
    auto func = vm.compile("let x = 1 + 2");
    assert(func.has_value());
    auto result = vm.run_function(*func);
    assert(result.is_ok());
}

static void test_extern_declaration_with_host_global() {
    mg::Vm vm;
    vm.set_global_number("HOST_SCORE", 42.0);
    auto result = vm.interpret(
        "!strict\nextern const HOST_SCORE: number\nlet score: number = HOST_SCORE\nprint(score)");
    assert(result.is_ok());
}

static void test_interpret_arithmetic() {
    mg::Vm vm;
    auto result = vm.interpret("let x = 2 + 3");
    assert(result.is_ok());
}

static void test_interpret_compile_error() {
    mg::Vm vm;
    auto result = vm.interpret("let x = ");
    assert(result.is_err());
}

static void test_interpret_rejects_interior_nul() {
    mg::Vm vm;
    std::string src("print(\"ok\")\0extra", 15);
    auto result = vm.interpret(src);
    assert(result.is_err());
}

static void test_interpret_named() {
    mg::Vm vm;
    auto result = vm.interpret_named("print(\"test\")", "test_script");
    assert(result.is_ok());
}

static void test_bits_roundtrip() {
    auto v = mg::Value::int_val(-7);
    auto bits = v.to_bits();
    auto v2 = mg::Value::from_bits(bits);
    assert(v2.as_int() == -7);
}

static void test_is_obj_excludes_tags() {
    assert(!mg::Value::null().is_obj());
    assert(!mg::Value::bool_val(true).is_obj());
    assert(!mg::Value::bool_val(false).is_obj());
    assert(!mg::Value::int_val(0).is_obj());
}

static void test_value_equality() {
    assert(mg::Value::null() == mg::Value::null());
    assert(mg::Value::bool_val(true) == mg::Value::bool_val(true));
    assert(mg::Value::int_val(1) == mg::Value::int_val(1));
    assert(mg::Value::int_val(1) != mg::Value::int_val(2));
    assert(mg::Value::null() != mg::Value::bool_val(false));
}

static void test_safe_numeric_ffi_registration() {
    mg::Vm vm;
    vm.register_native("square",
        [](::VM* vm, int32_t arg_count, ::Value* args) -> ::Value {
            auto v = mg::Value::from_raw(args[0]);
            auto n = v.as_numeric();
            return mg::Value::number_val(n * n).to_raw();
        }, 1);

    vm.register_native("add",
        [](::VM* vm, int32_t arg_count, ::Value* args) -> ::Value {
            auto a = mg::Value::from_raw(args[0]).as_numeric();
            auto b = mg::Value::from_raw(args[1]).as_numeric();
            return mg::Value::number_val(a + b).to_raw();
        }, 2);

    auto square_val = vm.get_global("square");
    assert(square_val.has_value());
    assert(square_val->is_native());
    assert(square_val->is_callable());

    auto result = vm.interpret("let x = square(4)\nlet y = add(x, 2)");
    assert(result.is_ok());
}

static void test_runtime_errors_preserve_message() {
    mg::Vm vm;
    auto result = vm.interpret("let x = \"a\" + 1");
    assert(result.is_err());
    assert(vm.last_error_message().find("Operands") != std::string::npos || vm.last_error_line() > 0);
    assert(vm.last_error_line() > 0);
}

static void test_runtime_error_snapshot_outlives_vm() {
    std::optional<mg::MgError> snapshot;
    std::string expected_kind;
    std::string expected_message;
    std::string expected_file;
    std::string expected_function;
    std::string expected_hint;
    int expected_line = 0;

    {
        mg::Vm vm;
        auto result = vm.interpret("let snapshot_failure = \"a\" + 1");
        assert(result.is_err());
        assert(result.error().kind() == mg::InterpretErrorKind::RuntimeError);
        assert(result.error().mg_error().has_value());

        snapshot = *result.error().mg_error();
        expected_kind = snapshot->kind();
        expected_message = snapshot->message();
        expected_file = snapshot->file();
        expected_line = snapshot->line();
        expected_function = snapshot->function();
        expected_hint = snapshot->hint();
        assert(!expected_message.empty());

        vm.clear_error();
        for (int i = 0; i < 128; i++) {
            vm.set_global_string(
                "snapshot_churn_" + std::to_string(i),
                std::string(128, static_cast<char>('a' + (i % 26))));
        }
        mg::gc_collect(vm);
    }

    assert(snapshot.has_value());
    assert(snapshot->kind() == expected_kind);
    assert(snapshot->message() == expected_message);
    assert(snapshot->file() == expected_file);
    assert(snapshot->line() == expected_line);
    assert(snapshot->function() == expected_function);
    assert(snapshot->hint() == expected_hint);
}

static std::atomic<bool> finalizer_called{false};

struct Counter {
    int32_t value;
    ~Counter() { finalizer_called.store(true); }
};

static ::Value counter_get(::VM* vm, int32_t arg_count, ::Value* args) {
    if (arg_count != 1 || !args) return mg::Value::null().to_raw();
    auto self = mg::Value::from_raw(args[0]);
    auto* data = vm_native_handle_data(self.to_raw(), "Counter");
    if (!data) return mg::Value::null().to_raw();
    auto* counter = static_cast<Counter*>(data);
    return mg::Value::int_val(counter->value).to_raw();
}

static void test_native_handle_method_and_finalizer() {
    finalizer_called.store(false);

    {
        mg::Vm vm;
        auto handle = vm.new_native_handle_t<Counter>("Counter",
            std::make_unique<Counter>(Counter{41}));
        assert(handle.is_native_handle());

        vm.set_native_handle_method(handle, "get", counter_get, 1);
        vm.set_global("counter", handle);

        auto result = vm.interpret("let value = counter.get()\nprint(value)");
        assert(result.is_ok());
    }

    assert(finalizer_called.load());
}

static void test_native_handle_closure_method() {
    finalizer_called.store(false);

    {
        mg::Vm vm;
        auto handle = vm.new_native_handle_t<Counter>("Counter",
            std::make_unique<Counter>(Counter{0}));

        vm.set_native_handle_method_fn<Counter>(handle, "Counter", "increment", 1,
            [](Counter& c, mg::NativeContext&) -> mg::Value {
                c.value += 1;
                return mg::Value::int_val(c.value);
            });

        vm.set_native_handle_method_fn<Counter>(handle, "Counter", "get", 1,
            [](Counter& c, mg::NativeContext&) -> mg::Value {
                return mg::Value::int_val(c.value);
            });

        vm.set_global("counter", handle);

        auto result = vm.interpret(
            "counter.increment()\ncounter.increment()\nlet v = counter.get()\nprint(v)");
        assert(result.is_ok());
    }

    assert(finalizer_called.load());
}

static void test_register_native_fn() {
    mg::Vm vm;

    vm.register_native_fn("double", 1, [](mg::NativeContext& ctx) -> mg::Value {
        auto n = ctx.expect_numeric(0);
        return mg::Value::auto_val(n * 2.0);
    });

    auto result = vm.interpret("let x = double(21)\nprint(x)");
    assert(result.is_ok());
}

static void test_register_native_fn_multi_args() {
    mg::Vm vm;

    vm.register_native_fn("add_nums", 2, [](mg::NativeContext& ctx) -> mg::Value {
        auto a = ctx.expect_numeric(0);
        auto b = ctx.expect_numeric(1);
        return mg::Value::auto_val(a + b);
    });

    auto result = vm.interpret("let x = add_nums(3, 4)\nprint(x)");
    assert(result.is_ok());
}

static void test_native_context_string_arg() {
    mg::Vm vm;

    vm.register_native_fn("is_hello", 1, [](mg::NativeContext& ctx) -> mg::Value {
        auto s = ctx.arg_string(0);
        if (s && *s == "hello") return mg::Value::bool_val(true);
        return mg::Value::bool_val(false);
    });

    auto result = vm.interpret("let ok = is_hello(\"hello\")\nprint(ok)");
    assert(result.is_ok());
}

static void test_native_fn_bool_arg() {
    mg::Vm vm;
    vm.register_native_fn("invert", 1, [](mg::NativeContext& ctx) -> mg::Value {
        auto b = ctx.arg_bool(0);
        return mg::Value::bool_val(b ? false : true);
    });

    auto result = vm.interpret("let x = invert(true)\nprint(x)");
    assert(result.is_ok());
}

static void test_native_fn_int_arg() {
    mg::Vm vm;
    vm.register_native_fn("double_it", 1, [](mg::NativeContext& ctx) -> mg::Value {
        auto n = ctx.arg_int(0);
        if (n) return mg::Value::int_val(*n * 2);
        return mg::Value::null();
    });

    auto result = vm.interpret("let x = double_it(21)\nprint(x)");
    assert(result.is_ok());
}

static void test_native_fn_numeric_arg() {
    mg::Vm vm;
    vm.register_native_fn("add_half", 1, [](mg::NativeContext& ctx) -> mg::Value {
        auto n = ctx.arg_numeric(0);
        if (n) return mg::Value::auto_val(*n + 0.5);
        return mg::Value::null();
    });

    auto result = vm.interpret("let x = add_half(10)\nprint(x)");
    assert(result.is_ok());
}

static void test_native_fn_no_args() {
    mg::Vm vm;
    vm.register_native_fn("the_answer", 0, [](mg::NativeContext& ctx) -> mg::Value {
        return mg::Value::int_val(42);
    });

    auto result = vm.interpret("let x = the_answer()\nprint(x)");
    assert(result.is_ok());
}

static void test_native_fn_access_vm() {
    mg::Vm vm;
    vm.register_native_fn("get_global_x", 0, [](mg::NativeContext& ctx) -> mg::Value {
        auto v = ctx.vm().get_global("x");
        return v.value_or(mg::Value::null());
    });

    vm.set_global("x", mg::Value::int_val(77));
    auto result = vm.interpret("let y = get_global_x()\nprint(y)");
    assert(result.is_ok());
}

static void test_vm_move() {
    mg::Vm vm1;
    vm1.set_global("x", mg::Value::int_val(10));
    mg::Vm vm2 = std::move(vm1);
    auto v = vm2.get_global("x");
    assert(v.has_value());
    assert(v->as_int() == 10);
}

static void test_vm_move_rebinds_native_callbacks() {
    mg::Vm vm1;
    vm1.register_native_fn("answer", 0, [](mg::NativeContext&) {
        return mg::Value::int_val(42);
    });

    mg::Vm vm2 = std::move(vm1);
    auto result = vm2.interpret("let moved_answer = answer()\nprint(moved_answer)");
    assert(result.is_ok());
}

static void test_native_exception_is_contained() {
    mg::Vm vm;
    vm.register_native_fn("managed_failure", 0, [](mg::NativeContext&) -> mg::Value {
        throw std::runtime_error("expected C++ callback failure");
    });
    auto result = vm.interpret("managed_failure()");
    assert(result.is_err());
    assert(vm.last_error_message().find("expected C++ callback failure") != std::string::npos);
}

static void test_gc_roots_are_not_lifo() {
    mg::Vm vm;
    auto first = vm.new_array_value();
    auto second = vm.new_array_value();
    auto first_root = std::make_unique<mg::GcRoot>(vm, first);
    auto second_root = std::make_unique<mg::GcRoot>(vm, second);

    first_root.reset();
    mg::gc_collect(vm);
    assert(second_root->get().is_array());

    mg::Vm moved = std::move(vm);
    mg::gc_collect(moved);
    assert(second_root->get().is_array());
    second_root.reset();
}

static void test_gc_root_can_outlive_vm() {
    std::unique_ptr<mg::GcRoot> root;
    {
        mg::Vm vm;
        root = std::make_unique<mg::GcRoot>(vm, vm.new_array_value());
        assert(root->get().is_array());
    }

    assert(root->get().is_null());
    assert(!root->set(mg::Value::int_val(1)));
    root.reset();
}

static void test_gc_roots_follow_vm_move_assignment() {
    mg::Vm source;
    auto source_root = std::make_unique<mg::GcRoot>(
        source, source.new_array_value());

    mg::Vm destination;
    auto old_destination_root = std::make_unique<mg::GcRoot>(
        destination, destination.new_array_value());

    destination = std::move(source);
    mg::gc_collect(destination);

    assert(source_root->get().is_array());
    assert(old_destination_root->get().is_null());
    assert(!old_destination_root->set(mg::Value::int_val(1)));
}

static void test_collection_mutators_report_success() {
    mg::Vm vm;
    auto array_value = vm.new_array_value();
    mg::GcRoot array_root(vm, array_value);
    mg::MgArray array(reinterpret_cast<::ObjArray*>(array_value.as_obj()));
    array.push(vm, mg::Value::int_val(1));
    assert(array.set(vm, 0, mg::Value::int_val(2)));
    assert(array.get(0)->as_int() == 2);

    auto dict_value = vm.new_dict_value();
    mg::GcRoot dict_root(vm, dict_value);
    auto* key_raw = copy_string(vm.raw_mut(), "key", 3);
    mg::GcRoot key_root(vm, mg::Value::obj_val(key_raw));
    mg::MgDict dict(reinterpret_cast<::ObjDict*>(dict_value.as_obj()));
    mg::MgString key(key_raw);
    assert(dict.set(vm, key, mg::Value::int_val(1)));
    assert(dict.set(vm, key, mg::Value::int_val(2)));
    assert(dict.get(key)->as_int() == 2);
}

static void test_new_array_value() {
    mg::Vm vm;
    auto arr = vm.new_array_value();
    assert(arr.is_array());
}

static void test_new_dict_value() {
    mg::Vm vm;
    auto d = vm.new_dict_value();
    assert(d.is_dict());
}

static void test_set_global_number_and_bool() {
    mg::Vm vm;
    vm.set_global_number("pi", 3.14);
    vm.set_global_bool("flag", true);

    auto pi = vm.get_global("pi");
    assert(pi.has_value());
    assert(pi->is_numeric());

    auto flag = vm.get_global("flag");
    assert(flag.has_value());
    assert(flag->is_bool());
    assert(flag->as_bool());
}

static void test_set_global_string() {
    mg::Vm vm;
    vm.set_global_string("greeting", "hello");
    auto v = vm.get_global("greeting");
    assert(v.has_value());
    assert(v->is_string());
}

static void test_clear_error() {
    mg::Vm vm;
    vm.interpret("let x = \"a\" + 1");
    assert(!vm.last_error_message().empty() || vm.last_error_line() > 0);
    vm.clear_error();
}

static void test_interpret_result_ok() {
    mg::Vm vm;
    auto result = vm.interpret("let x = 1");
    assert(result.is_ok());
    assert(!result.is_err());
}

static void test_vm_introspection() {
    mg::Vm vm;
    vm.interpret("let x = 1");
    assert(vm.frame_count() >= 0);
    assert(vm.stack_top() >= 0);
    assert(vm.bytes_allocated() > 0);
}

using TestFn = void(*)();

struct TestEntry {
    const char* name;
    TestFn fn;
};

static TestEntry tests[] = {
    {"vm_create_and_drop", test_vm_create_and_drop},
    {"interpret_hello", test_interpret_hello},
    {"value_null", test_value_null},
    {"value_bool", test_value_bool},
    {"value_int", test_value_int},
    {"value_float", test_value_float},
    {"value_auto_val", test_value_auto_val},
    {"value_falsey", test_value_falsey},
    {"try_as_typed_methods", test_try_as_typed_methods},
    {"expect_methods", test_expect_methods},
    {"push_pop", test_push_pop},
    {"set_get_global", test_set_get_global},
    {"get_global_missing", test_get_global_missing},
    {"compile", test_compile},
    {"compile_and_run", test_compile_and_run},
    {"extern_declaration_with_host_global", test_extern_declaration_with_host_global},
    {"interpret_arithmetic", test_interpret_arithmetic},
    {"interpret_compile_error", test_interpret_compile_error},
    {"interpret_rejects_interior_nul", test_interpret_rejects_interior_nul},
    {"interpret_named", test_interpret_named},
    {"bits_roundtrip", test_bits_roundtrip},
    {"is_obj_excludes_tags", test_is_obj_excludes_tags},
    {"value_equality", test_value_equality},
    {"safe_numeric_ffi_registration", test_safe_numeric_ffi_registration},
    {"runtime_errors_preserve_message", test_runtime_errors_preserve_message},
    {"runtime_error_snapshot_outlives_vm", test_runtime_error_snapshot_outlives_vm},
    {"native_handle_method_and_finalizer", test_native_handle_method_and_finalizer},
    {"native_handle_closure_method", test_native_handle_closure_method},
    {"register_native_fn", test_register_native_fn},
    {"register_native_fn_multi_args", test_register_native_fn_multi_args},
    {"native_context_string_arg", test_native_context_string_arg},
    {"native_fn_bool_arg", test_native_fn_bool_arg},
    {"native_fn_int_arg", test_native_fn_int_arg},
    {"native_fn_numeric_arg", test_native_fn_numeric_arg},
    {"native_fn_no_args", test_native_fn_no_args},
    {"native_fn_access_vm", test_native_fn_access_vm},
    {"vm_move", test_vm_move},
    {"vm_move_rebinds_native_callbacks", test_vm_move_rebinds_native_callbacks},
    {"native_exception_is_contained", test_native_exception_is_contained},
    {"gc_roots_are_not_lifo", test_gc_roots_are_not_lifo},
    {"gc_root_can_outlive_vm", test_gc_root_can_outlive_vm},
    {"gc_roots_follow_vm_move_assignment", test_gc_roots_follow_vm_move_assignment},
    {"collection_mutators_report_success", test_collection_mutators_report_success},
    {"new_array_value", test_new_array_value},
    {"new_dict_value", test_new_dict_value},
    {"set_global_number_and_bool", test_set_global_number_and_bool},
    {"set_global_string", test_set_global_string},
    {"clear_error", test_clear_error},
    {"interpret_result_ok", test_interpret_result_ok},
    {"vm_introspection", test_vm_introspection},
};

int main() {
    int passed = 0;
    int failed = 0;
    int total = sizeof(tests) / sizeof(tests[0]);

    for (int i = 0; i < total; i++) {
        std::cout << "  " << tests[i].name << " ... ";
        try {
            tests[i].fn();
            std::cout << "OK" << std::endl;
            passed++;
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << std::endl;
            failed++;
        } catch (...) {
            std::cout << "FAILED: unknown exception" << std::endl;
            failed++;
        }
    }

    std::cout << std::endl;
    std::cout << "Results: " << total << " tests | " << passed << " passed | " << failed << " failed" << std::endl;
    return failed;
}

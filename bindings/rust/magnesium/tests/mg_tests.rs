use std::fs;
use std::path::Path;
use std::process::Command;

use magnesium::Vm;

fn repo_root() -> &'static Path {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .parent()
        .unwrap()
}

fn read_expected(test_name: &str) -> String {
    let path = repo_root()
        .join("tests/expected")
        .join(format!("{}.expected", test_name));
    fs::read_to_string(&path)
        .unwrap_or_else(|error| panic!("failed to read {}: {}", path.display(), error))
}

fn normalize_output(output: &str) -> String {
    output
        .replace("\r\n", "\n")
        .replace('\r', "\n")
        .trim_end()
        .to_string()
}

fn run_mg_binary(test_name: &str) -> (String, bool) {
    let binary = std::env::var_os("MAGNESIUM_BIN")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| {
            repo_root().join(if cfg!(windows) {
                "magnesium.exe"
            } else {
                "magnesium"
            })
        });
    let relative_path = format!("tests/{}.mg", test_name);
    let output = Command::new(binary)
        .args([&relative_path])
        .current_dir(repo_root())
        .output()
        .expect("failed to run magnesium binary");
    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
    let stderr = String::from_utf8_lossy(&output.stderr).to_string();
    let combined = if stderr.is_empty() {
        stdout
    } else if stdout.is_empty() {
        stderr
    } else {
        format!("{}\n{}", stdout, stderr.trim_end_matches('\n'))
    };
    let success = output.status.success();
    (normalize_output(&combined), success)
}

fn check_binary_test(test_name: &str) {
    let expected = normalize_output(&read_expected(test_name));
    let (actual, _) = run_mg_binary(test_name);
    assert_eq!(actual, expected, "output mismatch for {}", test_name);
}

macro_rules! mg_binary_test {
    ($($name:ident),* $(,)?) => {
        $(
            #[test]
            fn $name() {
                check_binary_test(stringify!($name));
            }
        )*
    };
}

mg_binary_test!(
    test_arrays,
    test_asan_smoke,
    test_closure_queue_stress,
    test_closures,
    test_collections_edge,
    test_const,
    test_control,
    test_coroutine,
    test_defer,
    test_dictiter,
    test_dict_lookup_comparison,
    test_dicts,
    test_dotcall,
    test_enum,
    test_error_arity,
    test_error_break,
    test_error_continue,
    test_error_field_set,
    test_error_fn_const,
    test_error_handling_full,
    test_error_ffi_type,
    test_errors_runtime,
    test_errors_syntax,
    test_error_type,
    test_error_undefined,
    test_ffi,
    test_fn,
    test_fold,
    test_for_field_prop,
    test_gc_stress,
    test_hof,
    test_import_cache_paths,
    test_import,
    test_import_missing,
    test_import_private,
    test_import_repeat,
    test_math,
    test_multiret,
    test_question_try,
    test_range_hardening,
    test_spawn_nested,
    test_std_error_question,
    test_std_fs_path_process,
    test_std,
    test_structs,
    test_task,
    test_vars,
    test_vm_isolation,
);

#[test]
fn test_native_fn_from_mg() {
    unsafe extern "C" fn rust_add(
        _vm: *mut magnesium_sys::VM,
        arg_count: libc::c_int,
        args: *mut magnesium_sys::Value,
    ) -> magnesium_sys::Value {
        if arg_count < 2 {
            return magnesium::Value::null().to_raw();
        }
        let a = magnesium::Value::from_raw(*args.offset(0)).as_number();
        let b = magnesium::Value::from_raw(*args.offset(1)).as_number();
        magnesium::Value::number_val(a + b).to_raw()
    }

    let mut vm = Vm::new();
    unsafe { vm.register_native("rust_add", Some(rust_add), 2) };
    let val = vm.get_global("rust_add");
    assert!(
        val.is_some(),
        "native function should be findable via get_global"
    );
}

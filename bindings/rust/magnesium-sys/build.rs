use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let vendored = env::var("CARGO_FEATURE_VENDORED").is_ok();
    let system = env::var("CARGO_FEATURE_SYSTEM").is_ok();

    if let Ok(repo_root) = env::var("MAGNESIUM_REPO_ROOT") {
        build_repo_root(PathBuf::from(&repo_root));
    } else if vendored {
        build_vendored();
    } else if system {
        build_system();
    } else {
        panic!(
            "magnesium-sys: no build mode selected.\n\
             Enable one of: `vendored` (default), `system`, or set MAGNESIUM_REPO_ROOT."
        );
    }
}

fn build_repo_root(repo_root: PathBuf) {
    assert!(
        repo_root.join("src/magnesium.h").exists(),
        "MAGNESIUM_REPO_ROOT={:?} does not contain src/magnesium.h",
        repo_root
    );

    let status = Command::new("make")
        .args(["-C", repo_root.to_str().unwrap(), "lib"])
        .status()
        .expect("failed to run `make lib` - is `make` installed?");
    if !status.success() {
        panic!("`make lib` failed");
    }

    println!("cargo:rustc-link-search=native={}", repo_root.display());
    println!("cargo:rustc-link-lib=magnesium");
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", repo_root.display());

    emit_rerun_if_changed(&repo_root);
}

fn build_vendored() {
    let repo_root = find_repo_root();
    let src_dir = repo_root.join("src");

    let lib_src = [
        "lexer.c",
        "parser.c",
        "compiler.c",
        "typecheck.c",
        "object.c",
        "gc.c",
        "vm.c",
        "serialize.c",
        "lsp.c",
        "mg_ffi_glue.c",
    ];

    let mut build = cc::Build::new();
    build
        .warnings(false)
        .opt_level(3)
        .flag("-std=c11")
        .include(&src_dir);

    if cfg!(target_os = "linux") {
        build.define("_GNU_SOURCE", None).flag("-pthread");
    } else if cfg!(target_os = "windows") {
        build.define("_CRT_SECURE_NO_WARNINGS", None);
    } else if cfg!(target_os = "macos") {
        build.flag("-pthread");
    }

    for file in &lib_src {
        let path = src_dir.join(file);
        assert!(
            path.exists(),
            "vendored source not found: {}",
            path.display()
        );
        build.file(&path);
    }

    build.compile("magnesium");

    if cfg!(unix) {
        println!("cargo:rustc-link-lib=m");
        println!("cargo:rustc-link-lib=dl");
        println!("cargo:rustc-link-lib=pthread");
    }
    if cfg!(target_os = "windows") {
        println!("cargo:rustc-link-lib=ws2_32");
    }

    emit_rerun_if_changed(&repo_root);
}

fn build_system() {
    let lib = pkg_config::probe_library("magnesium").expect(
        "magnesium-sys: `system` feature enabled but libmagnesium not found via pkg-config.\n\
             Install Magnesium first: `sudo make install` (from the Magnesium repo root).",
    );

    for path in &lib.include_paths {
        println!("cargo:include={}", path.display());
    }

    for path in &lib.link_paths {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", path.display());
    }
}

fn emit_rerun_if_changed(repo_root: &PathBuf) {
    println!(
        "cargo:rerun-if-changed={}",
        repo_root.join("src/magnesium.h").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        repo_root.join("src/mg_ffi_glue.c").display()
    );

    let src_dir = repo_root.join("src");
    if let Ok(entries) = std::fs::read_dir(&src_dir) {
        for entry in entries.flatten() {
            let p = entry.path();
            if p.extension().map(|e| e == "c").unwrap_or(false) {
                println!("cargo:rerun-if-changed={}", p.display());
            }
        }
    }
}

fn find_repo_root() -> PathBuf {
    let dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let repo_root = dir
        .parent()
        .and_then(|p| p.parent())
        .and_then(|p| p.parent())
        .expect("expected bindings/rust/magnesium-sys - three levels up from repo root");
    assert!(
        repo_root.join("src/magnesium.h").exists(),
        "repo root {:?} does not contain src/magnesium.h - cannot use vendored build without C source tree",
        repo_root
    );
    repo_root.to_path_buf()
}

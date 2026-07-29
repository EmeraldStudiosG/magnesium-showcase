use std::env;
use std::path::PathBuf;

fn main() {
    if env::var("CARGO_CFG_TARGET_FAMILY").as_deref() == Ok("unix") {
        if let Ok(r) = env::var("MAGNESIUM_REPO_ROOT") {
            let repo_root = PathBuf::from(r);
            println!("cargo:rustc-link-arg=-Wl,-rpath,{}", repo_root.display());
        }
    }
}

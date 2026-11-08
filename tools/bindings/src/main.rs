fn main() {
    let root = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let bindings = bindgen::Builder::default()
        .header(root.join("c/sqlite_api.h").to_str().unwrap())
        .clang_arg(format!("-I{}", root.join("vendor/sqlite").display()))
        .allowlist_type("sqlite3(_file|_vfs|_io_methods|_stmt|_api_routines)?")
        .allowlist_type("MvfsApi")
        .allowlist_function("mvfs_.*")
        .allowlist_var("SQLITE_.*")
        .default_macro_constant_type(bindgen::MacroTypeVariation::Signed)
        .derive_debug(false)
        .layout_tests(false)
        .generate_comments(false)
        .generate()
        .expect("generate SQLite ABI bindings");
    bindings
        .write_to_file(root.join("src/ffi_bindings.rs"))
        .unwrap();
}

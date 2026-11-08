use std::process::Command;

fn native_build(deployment: Option<&str>) -> cc::Build {
    let mut build = cc::Build::new();
    if let Some(version) = deployment {
        build.env("MACOSX_DEPLOYMENT_TARGET", version);
    }
    build
}

fn deployment_target() -> Option<String> {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        return None;
    }
    println!("cargo:rerun-if-env-changed=MACOSX_DEPLOYMENT_TARGET");
    let output = Command::new(std::env::var_os("RUSTC").expect("Cargo RUSTC"))
        .args(["--print", "deployment-target", "--target"])
        .arg(std::env::var_os("TARGET").expect("Cargo TARGET"))
        .output()
        .expect("query rustc deployment target");
    assert!(
        output.status.success(),
        "rustc deployment target query failed"
    );
    let output = String::from_utf8(output.stdout).expect("rustc deployment target is UTF-8");
    Some(
        output
            .trim()
            .strip_prefix("MACOSX_DEPLOYMENT_TARGET=")
            .expect("rustc macOS deployment target")
            .to_owned(),
    )
}

fn main() {
    let loadable = std::env::var_os("CARGO_FEATURE_LOADABLE").is_some();
    let linked = std::env::var_os("CARGO_FEATURE_STATIC").is_some();
    assert!(
        loadable != linked,
        "select exactly one of loadable or static"
    );
    let parity = std::env::var_os("CARGO_FEATURE_PARITY").is_some();
    let copy = std::env::var_os("CARGO_FEATURE_COPY").is_some();
    assert!(parity != copy, "select exactly one of parity or copy");
    // Cargo also builds the unused cdylib in static mode. Its SQLite symbols
    // belong to the eventual host, just as they do for the static archive.
    if linked && std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos") {
        println!("cargo:rustc-cdylib-link-arg=-Wl,-undefined,dynamic_lookup");
    }
    for file in [
        "c/sqlite_api.c",
        "c/sqlite_api.h",
        "vendor/sqlite/sqlite3.h",
        "vendor/sqlite/sqlite3ext.h",
    ] {
        println!("cargo:rerun-if-changed={file}");
    }
    // cc otherwise chooses the installed SDK version, which can be newer
    // than Rust's target. Query Rust so defaults and caller overrides agree.
    let deployment = deployment_target();
    let mut bridge = native_build(deployment.as_deref());
    bridge
        .file("c/sqlite_api.c")
        .include("vendor/sqlite")
        .include("c")
        .std("c11");
    if linked {
        bridge.define("MVFS_STATIC", None);
    }
    bridge.compile("monstervfs_sqlite_api");
    if !parity {
        return;
    }

    let mut fec = native_build(deployment.as_deref());
    fec.include("vendor/libfec")
        .std("c99")
        .flag_if_supported("-Wno-sign-compare");
    for file in ["init_rs_int.c", "encode_rs_int.c", "decode_rs_int.c"] {
        let path = format!("vendor/libfec/{file}");
        println!("cargo:rerun-if-changed={path}");
        fec.file(path);
    }
    for file in [
        "init_rs.h",
        "encode_rs.h",
        "decode_rs.h",
        "rs-common.h",
        "int.h",
    ] {
        println!("cargo:rerun-if-changed=vendor/libfec/{file}");
    }
    fec.compile("monstervfs_fec");
}

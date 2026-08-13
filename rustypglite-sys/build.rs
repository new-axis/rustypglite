/*
 * Build script for rustypglite-sys.
 *
 * Compiles the C shim (pg_shim.c) which manages the postgres process lifecycle.
 * PostgreSQL binaries are downloaded at runtime, not compiled from source.
 */

fn main() {
    println!("cargo:rerun-if-changed=shim/pg_shim.c");
    println!("cargo:rerun-if-changed=shim/pg_shim.h");

    cc::Build::new()
        .file("shim/pg_shim.c")
        .include("shim")
        .warnings(false)
        .flag_if_supported("-Wno-unused-parameter")
        .flag_if_supported("-Wno-sign-compare")
        .compile("rpgl_shim");

    // Link dl for dladdr() and dlopen() (libpq loading).
    //
    // LINUX ONLY. On macOS (and the other BSDs) dlopen/dladdr live in libSystem
    // and there is NO separate libdl, so emitting this makes the link fail
    // outright with "library not found for -ldl" — which is why this crate could
    // not be built on a Mac at all, before anything platform-specific in the
    // shim itself was ever reached.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux") {
        println!("cargo:rustc-link-lib=dylib=dl");
    }
}

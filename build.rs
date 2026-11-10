// The DLL exports exactly the five ffx entry points (AGENTS.md hard rule). The .def file is the
// single list; the `exports` test in tests/abi.rs checks the built image against it.
fn main() {
    println!("cargo:rerun-if-changed=exports.def");
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        let dir = std::env::var("CARGO_MANIFEST_DIR").expect("manifest dir");
        println!("cargo:rustc-cdylib-link-arg=/DEF:{dir}/exports.def");
    }
}

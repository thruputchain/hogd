extern crate bindgen;
extern crate cmake;

// Build directory should be set with `cargo --target-dir`
// Compiler should be selected by setting CC/CXX environment vars

fn main() {
    use std::env;
    use std::path::PathBuf;

    // Use cmake to configure and build the C++ library

    let source_dir = "../..";
    let mut cfg = cmake::Config::new(source_dir);
    cfg
        .define("CMAKE_BUILD_TYPE", "Release")
        // run cargo b -vv to see the full cmake output
        .define("CMAKE_VERBOSE_MAKEFILE", "ON")
        .define("CMAKE_OSX_SYSROOT", "")
        ;

    // important: only build a limit set of targets  If we do this wrong
    // we could easily wind up with some kind of nonsense recursive build.

    cfg.build_target("test_ffi");
    let dst = cfg.build();

    cfg.build_target("rs_test_contracts");
    let _ = cfg.build();

    println!("cargo:rerun-if-changed=../../hogd");
    println!("cargo:rerun-if-changed=../../x23");

    // Tell Rust where to find the built library
    println!(
        "cargo:rustc-link-search=native={}/build/hogd/test_ffi/",
        dst.display()
    );
    println!("cargo:rustc-link-lib=dylib=test_ffi");

    // No idea how to propagate this flag to dependent packages.
    println!(
        "cargo:rustc-link-arg=-Wl,-rpath,{}/build/hogd/test_ffi/",
        dst.display()
    );

    // Generate Rust bindings from the C header file
    let bindings = bindgen::Builder::default()
        .header("../../hogd/test_ffi/test_bindings.h")
        .generate()
        .expect("Unable to generate bindings");

    // Write the bindings to a Rust file
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");

    println!("out_path = {}", out_path.display());
}

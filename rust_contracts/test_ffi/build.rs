extern crate bindgen;
extern crate cmake;

// thank u chatgpt

fn main() {
    use std::env;
    use std::path::PathBuf;

    for (key, value) in std::env::vars() {
        println!("cargo:warning={}={}", key, value);
    }

    // Run CMake and build the C++ library
    let mut cmake_config = cmake::Config::new("../.."); // Points to C++ source directory
    let binding = cmake_config
        .define("CMAKE_BUILD_TYPE", "Release")
        // TODO(geoff): Using g++ here doesn't play well with cargo.
        // There's a --target=xyz variable that gets passed to gcc,
        // when building from a different workspace (only, which is bizarre. same workspace works fine).
        // This is a clang flag not a gcc flag, and so gcc errors out.
        .define("CMAKE_CXX_COMPILER", "clang++")
        .define("CMAKE_C_COMPILER", "clang")
        .define("CMAKE_VERBOSE_MAKEFILE", "ON") // run cargo b -vv to see the full cmake output
        .define("CMAKE_OSX_SYSROOT", "");

    // important: only build a limit set of targets  If we do this wrong
    // we could easily wind up with some kind of nonsense recursive build.
    let dst = binding.build_target("test_ffi").build();

    let _ = binding.build_target("rs_test_contracts").build();

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

extern crate cmake;
extern crate bindgen;

use std::env;
use std::path::PathBuf;

// thank u chatgpt

fn main() {
    // Run CMake and build the C++ library
    let dst = cmake::Config::new("../..") // Points to C++ source directory
        .define("CMAKE_BUILD_TYPE", "Release")
        .define("CMAKE_CXX_COMPILER", "g++")
        .define("CMAKE_C_COMPILER", "gcc")
        .define("CMAKE_VERBOSE_MAKEFILE", "ON") // run cargo b -vv to see the full cmake output
        .define("CMAKE_OSX_SYSROOT", "")
        .build_target("test_ffi") // important: only build the test_ffi option.  If we do this wrong
        // we could easily wind up with some kind of nonsense recursive build.
        .build();

    println!("cargo:rerun-if-changed=../../hogd");
    println!("cargo:rerun-if-changed=../../x23");

    // Tell Rust where to find the built library
    println!("cargo:rustc-link-search=native={}/build/hogd/test_ffi/", dst.display());
    println!("cargo:rustc-link-lib=static=test_ffi");

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
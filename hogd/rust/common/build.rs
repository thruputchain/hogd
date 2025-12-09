extern crate bindgen;

fn main() {
    use std::env;
    use std::path::PathBuf;

    // Generate Rust bindings from the C header file
    let bindings = bindgen::Builder::default()
        .header("../../hogexec/runtime_trampoline/ffi_trampolines.h")
        .generate()
        .expect("Unable to generate bindings");

    // Write the bindings to a Rust file
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");

    println!("out_path = {}", out_path.display());
}

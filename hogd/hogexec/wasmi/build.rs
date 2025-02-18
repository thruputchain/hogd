extern crate cbindgen;

use std::env;
use std::path::Path;
use std::fs::File;

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();

    for (key, value) in env::vars() {
        println!("cargo:warning={}={}", key, value);
    }


    let build_path = env::var("CORROSION_BUILD_DIR").unwrap_or_else(|_| ".".to_string());
    let output_path = Path::new(&build_path).join("bindings.h");

    let r = cbindgen::Builder::new()
        .with_crate(crate_dir) // Adjust as needed
        .with_config(cbindgen::Config::from_file("cbindgen.toml").unwrap())
        .generate()
        .expect("Unable to generate bindings").write_to_file(&output_path);
    
    println!("cargo:warning=Bindings written yes/no {}", r);

    println!("cargo:warning=Bindings written to {}", output_path.display());
}

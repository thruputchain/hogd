use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn convert_wat_to_wasm(
    manifest_dir: &Path,
    out_dir: &Path,
    wat_filename: &str,
    wasm_filename: &str,
) {
    let wat_path = manifest_dir.join("test_wat").join(wat_filename);
    let wasm_path = out_dir.join(wasm_filename);

    // Read the WAT file
    let wat_content = fs::read_to_string(&wat_path)
        .unwrap_or_else(|e| panic!("Failed to read {}: {}", wat_filename, e));

    // Convert WAT to WASM
    let wasm_bytes = wat::parse_str(&wat_content)
        .unwrap_or_else(|e| panic!("Failed to parse {}: {}", wat_filename, e));

    // Instrument with stack limiting
    let config = groundhog_wasm_meter::Config::default();
    let wasm_bytes = groundhog_wasm_meter::instrument(&wasm_bytes, &config)
        .unwrap_or_else(|e| {
            panic!("Failed to instrument {}: {}", wat_filename, e)
        });

    // Write the WASM file to the build output directory
    fs::write(&wasm_path, wasm_bytes)
        .unwrap_or_else(|e| panic!("Failed to write {}: {}", wasm_filename, e));

    // Tell cargo to rerun if the WAT file changes
    println!("cargo:rerun-if-changed={}", wat_path.display());
}

fn main() {
    // Get the manifest directory and output directory
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    // Tell cargo to rerun this build script if it changes
    println!("cargo:rerun-if-changed=build.rs");

    // Convert all test WAT files to WASM
    // Each conversion will register its own rerun-if-changed for the source WAT
    convert_wat_to_wasm(
        &manifest_dir,
        &out_dir,
        "test_fibonacci.wat",
        "fibonacci.wasm",
    );
    convert_wat_to_wasm(
        &manifest_dir,
        &out_dir,
        "test_simple_add.wat",
        "simple_add.wasm",
    );
    convert_wat_to_wasm(
        &manifest_dir,
        &out_dir,
        "test_oob_data_segment.wat",
        "oob_data_segment.wasm",
    );
    convert_wat_to_wasm(
        &manifest_dir,
        &out_dir,
        "test_oob_element_segment.wat",
        "oob_element_segment.wasm",
    );
}

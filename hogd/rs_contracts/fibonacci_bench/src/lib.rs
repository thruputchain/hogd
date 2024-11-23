#![no_std]

use rs_macros::public_method;

#[public_method(static_calldata_length = 4, static_return_length = 4)]
fn fibonacci_bench_root(depth: u32) -> u32 {
    const MODULUS: u32 = 10237849; // an arbitrary prime from https://oeis.org/A074665

    let mut a: u32 = 0;
    let mut b: u32 = 1;

    for _ in 0..depth {
        let c: u32 = (a + b) % MODULUS;
        a = b;
        b = c;
    }
    b
}

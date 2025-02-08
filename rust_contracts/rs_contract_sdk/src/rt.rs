#[cfg(target_arch = "wasm32")]
#[inline(always)]
pub fn trap() -> ! {
    core::arch::wasm32::unreachable()
}

#[cfg(all(target_arch = "wasm32", not(feature = "std")))]
#[panic_handler]
fn handle_panic(_: &core::panic::PanicInfo) -> ! {
    trap();
}

#[cfg(all(not(target_arch = "wasm32"), not(feature = "std")))]
#[panic_handler]
fn handle_panic(_: &core::panic::PanicInfo) -> ! {
    // empty panic handler that halts the program.
    // This should probably never be used -- either this library will be built in std
    // for testing or built targeting wasm32

    loop {}
}

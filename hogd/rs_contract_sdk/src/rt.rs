#[cfg(target_arch = "wasm32")]
#[inline(always)]
pub fn trap() -> ! {
    core::arch::wasm32::unreachable()
}

#[cfg(not(test))]
#[panic_handler]
fn handle_panic(_: &core::panic::PanicInfo) -> ! {
    trap();
}

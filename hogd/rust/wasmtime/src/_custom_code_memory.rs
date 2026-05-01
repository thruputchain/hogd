use wasmtime::{CustomCodeMemory, Error as WasmtimeError};

/// Custom code memory allocator that uses mprotect() on publish_executable()
/// and does nothing on unpublish_executable().
pub struct MprotectCodeMemory {
    page_size: usize,
}

impl MprotectCodeMemory {
    pub fn new() -> Self {
        let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) as usize };
        Self { page_size }
    }
}

impl CustomCodeMemory for MprotectCodeMemory {
    fn required_alignment(&self) -> usize {
        self.page_size
    }

    fn publish_executable(
        &self,
        ptr: *const u8,
        len: usize,
    ) -> Result<(), WasmtimeError> {
        // Wasmtime guarantees that ptr and ptr.offset(len) are aligned as per required_alignment()
        debug_assert_eq!((ptr as usize) % self.page_size, 0);
        debug_assert_eq!(((ptr as usize) + len) % self.page_size, 0);

        // We don't actually need to do anything here, as the memory is already set
        // as executable by the mmap in the ModuleCacheEntry constructor

        // Use mprotect to set memory to read and execute
        let result = unsafe {
            libc::mprotect(
                ptr as *mut libc::c_void,
                len,
                libc::PROT_READ | libc::PROT_EXEC,
            )
        };

        if result != 0 {
            let io_error = std::io::Error::last_os_error();
            return Err(WasmtimeError::new(io_error));
        }

        Ok(())
    }

    fn unpublish_executable(
        &self,
        ptr: *const u8,
        len: usize,
    ) -> Result<(), WasmtimeError> {
        // Wasmtime guarantees that ptr and ptr.offset(len) are aligned as per required_alignment()
        debug_assert_eq!((ptr as usize) % self.page_size, 0);
        debug_assert_eq!(((ptr as usize) + len) % self.page_size, 0);

        // Do nothing on unpublish_executable as requested
        Ok(())
    }
}

impl Default for MprotectCodeMemory {
    fn default() -> Self {
        Self::new()
    }
}

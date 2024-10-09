#![allow(non_snake_case)]

use crate::syscall_bindings;

pub fn Log(buffer: &[u8]) -> Result<(), i32> {
    syscall_bindings::raw_Log(buffer)
}

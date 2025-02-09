#![allow(non_snake_case)]

use crate::sys;
use crate::result::SysResult as Result;

pub fn Log(buffer: &[u8]) -> Result<()> {
    sys::raw_Log(buffer)
}

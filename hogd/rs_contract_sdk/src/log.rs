#![allow(non_snake_case)]

use crate::result::SysResult as Result;
use crate::sys;

pub fn Log(buffer: &[u8]) -> Result<()> {
    sys::raw_Log(buffer)
}

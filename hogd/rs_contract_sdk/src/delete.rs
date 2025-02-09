#![allow(non_snake_case)]

use crate::result::SysResult as Result;
use crate::sys;
use crate::types::StorageKey;

pub fn DeleteObject(key: &StorageKey) -> Result<()> {
    sys::raw_DeleteObject(key)?;
    Ok(())
}

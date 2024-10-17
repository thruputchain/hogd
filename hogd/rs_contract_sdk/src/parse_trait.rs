use crate::invoke::Return;
use crate::rt::{trap, unwrap_abort, unwrap_abort_option};
use rs_xdr::*;
use core::mem::MaybeUninit;

pub trait Parseable<'a> {
    fn parse_from(arg: &'a [u8]) -> (Self, usize)
    where
        Self: Sized;
}

pub trait Serializable {
    fn serialize_len(&self) -> usize;

    fn do_serialize(&self, slice: &mut [MaybeUninit<u8>]);
}

pub trait OutputReturnable {
    fn return_self(&self) -> !;
}

impl OutputReturnable for () {
    fn return_self(&self) -> ! {
        Return(&[])
    }
}

impl<'a> Parseable<'a> for u32 {
    fn parse_from(arg: &'a [u8]) -> (Self, usize) {
        let out: u32 = u32::from_le_bytes(unwrap_abort(unwrap_abort_option(arg.get(0..4)).try_into()));
        (out, 4)
    }
}

impl Serializable for u32 {
    fn serialize_len(&self) -> usize {
        4
    }

    fn do_serialize(&self, slice: &mut [MaybeUninit<u8>]) {
        debug_assert!(slice.len() == 4);
        let b = self.to_le_bytes();
        unsafe {
            core::ptr::copy_nonoverlapping(b.as_ptr(), MaybeUninit::slice_as_mut_ptr(slice), 4);
        }
    }
}

impl<'a> Parseable<'a> for i64 {
    fn parse_from(arg: &'a [u8]) -> (Self, usize) {
        let out: i64 = i64::from_le_bytes(unwrap_abort(unwrap_abort_option(arg.get(0..8)).try_into()));
        (out, 8)
    }
}

impl Serializable for i64 {
    fn serialize_len(&self) -> usize {
        8
    }

    fn do_serialize(&self, slice: &mut [MaybeUninit<u8>]) {
        debug_assert!(slice.len() == 4);
        let b = self.to_le_bytes();
        unsafe {
            core::ptr::copy_nonoverlapping(b.as_ptr(), MaybeUninit::slice_as_mut_ptr(slice), 8);
        }
    }
}

impl<'a> Parseable<'a> for StorageKey {
    fn parse_from(arg: &'a [u8]) -> (Self, usize) {
        //TODO(geoff): Could try something a bit fancier
        let out: StorageKey = unwrap_abort(unwrap_abort_option(arg.get(..32)).try_into());
        (out, 32)
    }
}

impl Serializable for &StorageKey {
    fn serialize_len(&self) -> usize {
        32
    }

    fn do_serialize(&self, slice: &mut [MaybeUninit<u8>]) {
        debug_assert!(slice.len() == 32);
        unsafe {
            core::ptr::copy_nonoverlapping(
                self.as_ptr(),
                MaybeUninit::slice_as_mut_ptr(slice),
                32,
            );
        }
    }
}

impl<'a> Parseable<'a> for &'a [u8] {
    fn parse_from(arg: &'a [u8]) -> (Self, usize) {
        (arg, arg.len())
    }
}

impl Serializable for &[u8] {
    fn serialize_len(&self) -> usize {
        self.len()
    }

    fn do_serialize(&self, slice: &mut [MaybeUninit<u8>]) {
        debug_assert!(slice.len() == self.len());
        unsafe {
            core::ptr::copy_nonoverlapping(
                self.as_ptr(),
                MaybeUninit::slice_as_mut_ptr(slice),
                self.len(),
            );
        }
    }
}

// Will fail if buf.len() != sum(args.serialize_len())
pub fn serialize_to_calldata_1<T1: Serializable>(buf: &mut [MaybeUninit<u8>], arg1: T1) {
    let mut offset: usize = 0;

    arg1.do_serialize(unwrap_abort_option(buf.get_mut(offset..(offset + arg1.serialize_len()))));
    offset += arg1.serialize_len();

    if offset != buf.len() {
        trap();
    }
}

pub fn serialize_to_calldata_2<T1: Serializable, T2: Serializable>(
    buf: &mut [MaybeUninit<u8>],
    arg1: T1,
    arg2: T2,
) {
    let mut offset: usize = 0;

    arg1.do_serialize(unwrap_abort_option(buf.get_mut(offset..(offset + arg1.serialize_len()))));
    offset += arg1.serialize_len();

    arg2.do_serialize(unwrap_abort_option(buf.get_mut(offset..(offset + arg2.serialize_len()))));
    offset += arg2.serialize_len();

    if offset != buf.len() {
        trap();
    }
}

pub fn serialize_to_calldata_3<
    T1: Serializable,
    T2: Serializable,
    T3: Serializable,
>(
    buf: &mut [MaybeUninit<u8>],
    arg1: T1,
    arg2: T2,
    arg3: T3,
) -> Result<(), i32> {
    let mut offset: usize = 0;

    arg1.do_serialize(unwrap_abort_option(buf.get_mut(offset..(offset + arg1.serialize_len()))));
    offset += arg1.serialize_len();

    arg2.do_serialize(unwrap_abort_option(buf.get_mut(offset..(offset + arg2.serialize_len()))));
    offset += arg2.serialize_len();

    arg3.do_serialize(unwrap_abort_option(buf.get_mut(offset..(offset + arg3.serialize_len()))));
    offset += arg3.serialize_len();

    if offset != buf.len() {
        return Err(1234);
    }
    Ok(())
}

pub fn parse_from_calldata_1<'a, T1: Parseable<'a>, R: Sized>(
    bytes: &'a [u8],
    main_func: fn(T1) -> Result<R, i32>,
) -> Result<R, i32> {
    let (arg1, arg1len) = T1::parse_from(bytes);

    if arg1len != bytes.len() {
        trap();
    }

    main_func(arg1)
}

pub fn parse_from_calldata_2<
    'a,
    T1: Parseable<'a>,
    T2: Parseable<'a>,
    R: OutputReturnable,
>(
    bytes: &'a [u8],
    main_func: fn(T1, T2) -> Result<R, i32>,
) -> Result<R, i32> {
    let mut offset: usize = 0;
    let (arg1, arg1len) = T1::parse_from(unwrap_abort_option(bytes.get(offset..)));
    offset += arg1len;

    let (arg2, arg2len) = T2::parse_from(unwrap_abort_option(bytes.get(offset..)));
    offset += arg2len;

    if offset != bytes.len() {
        trap();
    }

    main_func(arg1, arg2)
}

pub fn parse_from_calldata_3<
    'a,
    T1: Parseable<'a>,
    T2: Parseable<'a>,
    T3: Parseable<'a>,
    R: OutputReturnable,
>(
    bytes: &'a [u8],
    main_func: fn(T1, T2, T3) -> Result<R, i32>,
) -> Result<R, i32> {
    let mut offset: usize = 0;
    let (arg1, arg1len) = T1::parse_from(unwrap_abort_option(bytes.get(offset..)));
    offset += arg1len;

    let (arg2, arg2len) = T2::parse_from(unwrap_abort_option(bytes.get(offset..)));
    offset += arg2len;

    let (arg3, arg3len) = T3::parse_from(unwrap_abort_option(bytes.get(offset..)));
    offset += arg3len;

    if offset != bytes.len() {
        trap();
    }

    main_func(arg1, arg2, arg3)
}

pub fn parse_from_calldata_4<
    'a,
    T1: Parseable<'a>,
    T2: Parseable<'a>,
    T3: Parseable<'a>,
    T4: Parseable<'a>,
    R: OutputReturnable,
>(
    bytes: &'a [u8],
    main_func: fn(T1, T2, T3, T4) -> Result<R, i32>,
) -> Result<R, i32> {
    let mut offset: usize = 0;
    let (arg1, arg1len) = T1::parse_from((bytes.get(offset..)).ok_or(SyscallErrorCode::kCalldataParseError as i32)?);
    offset += arg1len;

    let (arg2, arg2len) = T2::parse_from((bytes.get(offset..)).ok_or(SyscallErrorCode::kCalldataParseError as i32)?);
    offset += arg2len;

    let (arg3, arg3len) = T3::parse_from((bytes.get(offset..)).ok_or(SyscallErrorCode::kCalldataParseError as i32)?);
    offset += arg3len;

    let (arg4, arg4len) = T4::parse_from((bytes.get(offset..)).ok_or(SyscallErrorCode::kCalldataParseError as i32)?);
    offset += arg4len;

    if offset != bytes.len() {
        return Err(SyscallErrorCode::kCalldataParseError as i32);
    }

    main_func(arg1, arg2, arg3, arg4)
}

#[cfg(test)]
mod tests {
    use super::*;

    extern crate alloc;
    use alloc::boxed::Box;
    use alloc::vec;

    #[test]
    fn check_serialize_2() {
        let x: u32 = 1;
        let y: u32 = 2;

        let mut slice: Box<[u8]> = vec![0; 8].into_boxed_slice();

        serialize_to_calldata_2(slice.as_mut(), x, y);

        let expect: Box<[u8]> = vec![1, 0, 0, 0, 2, 0, 0, 0].into_boxed_slice();

        assert_eq!(slice, expect);
    }

    #[test]
    fn check_serialize_vec() {
        let x: u32 = 1;
        let y: u32 = 2;

        let calldata: Box<[u8]> = vec![1, 2, 3, 4].into_boxed_slice();

        let mut slice: Box<[u8]> = vec![0; 12].into_boxed_slice();

        serialize_to_calldata_3(slice.as_mut(), x, y, calldata.as_ref());

        let expect: Box<[u8]> =
            vec![1, 0, 0, 0, 2, 0, 0, 0, 1, 2, 3, 4].into_boxed_slice();

        assert_eq!(slice, expect);
    }
}

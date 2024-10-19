use crate::invoke::{Exit, Return};
use core::mem::MaybeUninit;
use rs_xdr::*;

#[cfg(feature = "alloc")]
extern crate alloc;
#[cfg(feature = "alloc")]
use alloc::boxed::Box;

pub trait Parseable<'a> {
    fn parse_from(arg: &'a [u8]) -> (Self, usize)
    where
        Self: Sized;
}

pub trait ParseableOwned {
    fn parse_from(arg: &[u8]) -> Result<(Self, usize), i32>
    where
        Self: Sized;
}

pub trait Serializable {
    fn serialize_len(&self) -> usize;

    fn do_serialize(&self, slice: &mut [MaybeUninit<u8>])
        -> Result<usize, i32>;
}

pub trait OutputReturnable {
    fn return_self(&self) -> !;
}

#[cfg(feature = "alloc")]
impl ParseableOwned for Box<[u8]> {
    fn parse_from(arg: &[u8]) -> Result<(Self, usize), i32> {
        let mut out = Box::new_uninit_slice(arg.len());
        unsafe {
            core::ptr::copy_nonoverlapping(
                arg.as_ptr(),
                MaybeUninit::slice_as_mut_ptr(out.as_mut()),
                arg.len(),
            )
        };
        Ok((unsafe { out.assume_init() }, arg.len()))
    }
}

/*
Infuriating: the following does NOT work in rust, because they decided not to implement it
and stackoverflow commenters say its too much work
impl<T : GenericInt> ParseableOwned for T {
    fn parse_from(arg: &[u8]) -> Result<(Self, usize), i32> {
        const sz : usize = core::mem::size_of::<T>();
        let b = arg.get(0..sz).ok_or(SyscallErrorCode::kReturnParseError as i32)?
            .try_into().or_else(|_| {Err(SyscallErrorCode::kReturnParseError as i32)})?;
        let out = T::from_le_bytes(b);
        Ok((out, sz))
    }
}
while the following does work
impl<const N : usize> ParseableOwned for [u8; N] {
    fn parse_from(arg: &[u8]) -> Result<(Self, usize), i32> {
        let b = arg.get(0..N).ok_or(SyscallErrorCode::kReturnParseError as i32)?
            .try_into().or_else(|_| {Err(SyscallErrorCode::kReturnParseError as i32)})?;
        Ok((b, N))
    }
}
*/

// thank chatgpt for this macro
macro_rules! impl_generic_int {
    ($($t:ty)*) => ($(
        impl ParseableOwned for $t {
            fn parse_from(arg: &[u8]) -> Result<(Self, usize), i32> {
                const SZ : usize = core::mem::size_of::<$t>();
                let b = arg.get(0..SZ).ok_or(SyscallErrorCode::kReturnParseError as i32)?
                    .try_into().or_else(|_| {Err(SyscallErrorCode::kReturnParseError as i32)})?;
                let out = <$t>::from_le_bytes(b);
                Ok((out, SZ))
            }
        }

        impl OutputReturnable for $t {
            fn return_self(&self) -> ! {
                Return(&<$t>::to_le_bytes(*self));
            }
        }

        impl<'a> Parseable<'a> for $t {
            fn parse_from(arg: &'a [u8]) -> (Self, usize) {
                const SZ : usize = core::mem::size_of::<$t>();
                let out = match (match arg.get(0..SZ) {
                    Some(b) => b,
                    None => Exit(SyscallErrorCode::kCalldataParseError as i32),
                })
                .try_into()
                {
                    Ok(x) => <$t>::from_le_bytes(x),
                    Err(_) => Exit(SyscallErrorCode::kCalldataParseError as i32),
                };
                (out, SZ)
            }
        }

        impl Serializable for $t {
            fn serialize_len(&self) -> usize {
                core::mem::size_of::<$t>()
            }

            fn do_serialize(&self, slice: &mut [MaybeUninit<u8>])  -> Result<usize, i32> {
                const SZ : usize = core::mem::size_of::<$t>();
                debug_assert!(slice.len() == SZ);
                let b = self.to_le_bytes();
                unsafe {
                    core::ptr::copy_nonoverlapping(
                        b.as_ptr(),
                        MaybeUninit::slice_as_mut_ptr(slice),
                        SZ,
                    );
                }
                Ok(SZ)
            }
        }
    )*)
}

impl_generic_int! {i8 i16 i32 i64 u8 u16 u32 u64}

impl<const N: usize> ParseableOwned for [u8; N] {
    fn parse_from(arg: &[u8]) -> Result<(Self, usize), i32> {
        let b = arg
            .get(0..N)
            .ok_or(SyscallErrorCode::kReturnParseError as i32)?
            .try_into()
            .or_else(|_| Err(SyscallErrorCode::kReturnParseError as i32))?;
        Ok((b, N))
    }
}

impl ParseableOwned for () {
    fn parse_from(_: &[u8]) -> Result<(Self, usize), i32> {
        Ok(((), 0))
    }
}

impl<T1: ParseableOwned, T2: ParseableOwned> ParseableOwned for (T1, T2) {
    fn parse_from(arg: &[u8]) -> Result<(Self, usize), i32> {
        let mut offset: usize = 0;

        let (arg1, arg1len) = T1::parse_from(match arg.get(offset..) {
            Some(b) => b,
            None => {
                return Err(SyscallErrorCode::kReturnParseError as i32);
            }
        })?;

        offset += arg1len;

        let (arg2, arg2len) = T2::parse_from(match arg.get(offset..) {
            Some(b) => b,
            None => {
                return Err(SyscallErrorCode::kReturnParseError as i32);
            }
        })?;

        offset += arg2len;

        Ok(((arg1, arg2), offset))
    }
}

impl Serializable for () {
    fn serialize_len(&self) -> usize {
        0
    }
    fn do_serialize(&self, _: &mut [MaybeUninit<u8>]) -> Result<usize, i32> {
        Ok(0)
    }
}

impl OutputReturnable for () {
    fn return_self(&self) -> ! {
        Return(&[])
    }
}

impl OutputReturnable for &[u8] {
    fn return_self(&self) -> ! {
        Return(self)
    }
}

impl<T: OutputReturnable> OutputReturnable for Result<T, i32> {
    fn return_self(&self) -> ! {
        match self {
            Ok(x) => {
                x.return_self();
            }
            Err(code) => {
                Exit(*code);
            }
        }
    }
}

impl<T: OutputReturnable> OutputReturnable for Option<T> {
    fn return_self(&self) -> ! {
        match self {
            Some(x) => {
                x.return_self();
            }
            None => {
                Exit(-1);
            }
        }
    }
}

impl<'a> Parseable<'a> for &'a StorageKey {
    fn parse_from(arg: &'a [u8]) -> (Self, usize) {
        let out: &'a StorageKey = match (match arg.get(..32) {
            Some(x) => x,
            None => Exit(SyscallErrorCode::kCalldataParseError as i32),
        })
        .try_into()
        {
            Ok(x) => x,
            Err(_) => Exit(SyscallErrorCode::kCalldataParseError as i32),
        };
        (out, 32)
    }
}

impl Serializable for &StorageKey {
    fn serialize_len(&self) -> usize {
        32
    }

    fn do_serialize(
        &self,
        slice: &mut [MaybeUninit<u8>],
    ) -> Result<usize, i32> {
        debug_assert!(slice.len() == 32);
        unsafe {
            core::ptr::copy_nonoverlapping(
                self.as_ptr(),
                MaybeUninit::slice_as_mut_ptr(slice),
                32,
            );
        }
        Ok(32)
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

    fn do_serialize(
        &self,
        slice: &mut [MaybeUninit<u8>],
    ) -> Result<usize, i32> {
        debug_assert!(slice.len() == self.len());
        unsafe {
            core::ptr::copy_nonoverlapping(
                self.as_ptr(),
                MaybeUninit::slice_as_mut_ptr(slice),
                self.len(),
            );
        }
        Ok(slice.len())
    }
}

#[cfg(feature = "alloc")]
impl<'a> Parseable<'a> for Box<[u8]> {
    fn parse_from(arg: &'a [u8]) -> (Self, usize) {
        let mut out = Box::<[u8]>::new_uninit_slice(arg.len());
        unsafe {
            core::ptr::copy_nonoverlapping(
                arg.as_ptr(),
                MaybeUninit::slice_as_mut_ptr(out.as_mut()),
                arg.len(),
            )
        };

        (unsafe { out.assume_init() }, arg.len())
    }
}

pub fn compute_serialize_len_1<T1: Serializable>(arg1: T1) -> usize {
    arg1.serialize_len()
}

pub fn compute_serialize_len_2<T1: Serializable, T2: Serializable>(
    arg1: T1,
    arg2: T2,
) -> usize {
    arg1.serialize_len() + arg2.serialize_len()
}

pub fn compute_serialize_len_3<
    T1: Serializable,
    T2: Serializable,
    T3: Serializable,
>(
    arg1: T1,
    arg2: T2,
    arg3: T3,
) -> usize {
    arg1.serialize_len() + arg2.serialize_len() + arg3.serialize_len()
}

// parse_from_calldata happens inside the sdk, before any user code,
// so we just call exit(code) there instead of passing that error back up.

// Will fail if buf.len() != sum(args.serialize_len())
pub fn serialize_to_calldata_0(buf: &mut [MaybeUninit<u8>]) -> Result<(), i32> {
    if 0 != buf.len() {
        return Err(SyscallErrorCode::kCalldataParseError as i32);
    }
    Ok(())
}
pub fn serialize_to_calldata_1<T1: Serializable>(
    buf: &mut [MaybeUninit<u8>],
    arg1: T1,
) -> Result<(), i32> {
    let mut offset: usize = 0;

    offset += arg1.do_serialize(
        buf.get_mut(offset..(offset + arg1.serialize_len()))
            .ok_or(SyscallErrorCode::kCalldataParseError as i32)?,
    )?;

    if offset != buf.len() {
        return Err(SyscallErrorCode::kCalldataParseError as i32);
    }
    Ok(())
}

pub fn serialize_to_calldata_2<T1: Serializable, T2: Serializable>(
    buf: &mut [MaybeUninit<u8>],
    arg1: T1,
    arg2: T2,
) -> Result<(), i32> {
    let mut offset: usize = 0;

    offset += arg1.do_serialize(
        buf.get_mut(offset..(offset + arg1.serialize_len()))
            .ok_or(SyscallErrorCode::kCalldataParseError as i32)?,
    )?;

    offset += arg2.do_serialize(
        buf.get_mut(offset..(offset + arg2.serialize_len()))
            .ok_or(SyscallErrorCode::kCalldataParseError as i32)?,
    )?;

    if offset != buf.len() {
        return Err(SyscallErrorCode::kCalldataParseError as i32);
    }
    Ok(())
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

    offset += arg1.do_serialize(
        buf.get_mut(offset..(offset + arg1.serialize_len()))
            .ok_or(SyscallErrorCode::kCalldataParseError as i32)?,
    )?;

    offset += arg2.do_serialize(
        buf.get_mut(offset..(offset + arg2.serialize_len()))
            .ok_or(SyscallErrorCode::kCalldataParseError as i32)?,
    )?;

    offset += arg3.do_serialize(
        buf.get_mut(offset..(offset + arg3.serialize_len()))
            .ok_or(SyscallErrorCode::kCalldataParseError as i32)?,
    )?;

    if offset != buf.len() {
        return Err(SyscallErrorCode::kCalldataParseError as i32);
    }

    Ok(())
}

pub fn parse_from_calldata_0<'a, R: OutputReturnable>(
    bytes: &'a [u8],
    main_func: fn() -> R,
) -> R {
    if 0 != bytes.len() {
        Exit(SyscallErrorCode::kCalldataParseError as i32);
    }

    main_func()
}

pub fn parse_from_calldata_1<'a, T1: Parseable<'a>, R: OutputReturnable>(
    bytes: &'a [u8],
    main_func: fn(T1) -> R,
) -> R {
    let mut offset: usize = 0;

    let (arg1, arg1len) = T1::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });
    offset += arg1len;

    if offset != bytes.len() {
        Exit(SyscallErrorCode::kCalldataParseError as i32);
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
    main_func: fn(T1, T2) -> R,
) -> R {
    let mut offset: usize = 0;

    let (arg1, arg1len) = T1::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });

    offset += arg1len;

    let (arg2, arg2len) = T2::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });

    offset += arg2len;

    if offset != bytes.len() {
        Exit(SyscallErrorCode::kCalldataParseError as i32);
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
    main_func: fn(T1, T2, T3) -> R,
) -> R {
    let mut offset: usize = 0;

    let (arg1, arg1len) = T1::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });

    offset += arg1len;

    let (arg2, arg2len) = T2::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });

    offset += arg2len;

    let (arg3, arg3len) = T3::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });
    offset += arg3len;

    if offset != bytes.len() {
        Exit(SyscallErrorCode::kCalldataParseError as i32);
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
    main_func: fn(T1, T2, T3, T4) -> R,
) -> R {
    let mut offset: usize = 0;

    let (arg1, arg1len) = T1::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });

    offset += arg1len;

    let (arg2, arg2len) = T2::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });

    offset += arg2len;

    let (arg3, arg3len) = T3::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });
    offset += arg3len;

    let (arg4, arg4len) = T4::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });
    offset += arg4len;

    if offset != bytes.len() {
        Exit(SyscallErrorCode::kCalldataParseError as i32);
    }

    main_func(arg1, arg2, arg3, arg4)
}

pub fn parse_returns_from_calldata<'a, T: ParseableOwned>(
    bytes: &'a [u8],
) -> Result<T, i32> {
    let (arg, arglen) = T::parse_from(&bytes)?;

    if arglen != bytes.len() {
        return Err(SyscallErrorCode::kReturnParseError as i32);
    }

    Ok(arg)
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

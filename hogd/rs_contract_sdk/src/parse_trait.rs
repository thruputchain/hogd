use crate::invoke::Exit;
use core::mem::MaybeUninit;
use rs_xdr::*;

#[cfg(feature = "alloc")]
extern crate alloc;
#[cfg(feature = "alloc")]
use alloc::boxed::Box;

// TODO(geoff): The error codes in this module are a mess
// I should rework to SerializeError and DeserializeError
// The complicating factor is that it might be nice to allow cross-contract
// invocations to be able to distinguish error types between the following cases:
// Caller: serialize args
//   Callee: deserialize args
//   Callee: serialize return
// Caller: deserialize return
//
// So then the SDK needs 4 error types, essentially.  Which is obnoxious, and perhaps overkill.
// The caller should at least be able to know, upon trying to invoke another contract,
// whether the invocation happened or not (regardless of whether it is able to parse the returned buffer).

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

    fn do_serialize(&self, slice: &mut [MaybeUninit<u8>]) -> Option<usize>;
}

// This, or any comparable design,
// requires generic_const_exprs in order to be meaningfully useful, unfortunately.

trait SizedSerializable {
    const SZ: usize;

    fn do_sized_serialize(&self, slice: &mut [MaybeUninit<u8>]) -> Option<()>;

    /*fn sized_serialize(&self) -> Option<[u8; Self::SZ]> {
        let mut bytes_mu = [MaybeUninit::<u8>::uninit(); Self::SZ];
        self.do_sized_serialize(&mut bytes_mu)?;
        Some(unsafe { MaybeUninit::array_assume_init(bytes_mu) })
    }*/
}

// This does not immediately terminate the function.
// Should be called at the end of an invocation.
pub trait OutputReturnable {
    fn return_self(&self) -> Result<(), i32>;
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
sad: the following does NOT work in rust, because they decided not to implement it
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
            fn return_self(&self) -> Result<(), i32> {
                crate::syscall_bindings::raw_Return(&<$t>::to_le_bytes(*self))
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

        impl SizedSerializable for $t {
            const SZ : usize = core::mem::size_of::<$t>();

            //fn serialize_len(&self) -> usize {
            //    core::mem::size_of::<$t>()
            //}

            fn do_sized_serialize(&self, slice: &mut [MaybeUninit<u8>])  -> Option<()> {
                //const SZ : usize = core::mem::size_of::<$t>();
                debug_assert!(slice.len() == Self::SZ);
                let b = self.to_le_bytes();
                unsafe {
                    core::ptr::copy_nonoverlapping(
                        b.as_ptr(),
                        MaybeUninit::slice_as_mut_ptr(slice),
                        Self::SZ,
                    );
                }
                Some(())
            }
        }
    )*)
}

macro_rules! sized_serialize_to_serialize {
    ($($t:ty)*) => ($(
        impl Serializable for $t {
            fn serialize_len(&self) -> usize {
                <$t>::SZ
            }
            fn do_serialize(&self, buf : &mut [MaybeUninit<u8>]) -> Option<usize> {
                self.do_sized_serialize(buf)?;
                Some(<$t>::SZ)
            }
        }

    )*)
}

impl_generic_int! {i8 i16 i32 i64 i128 u8 u16 u32 u64 u128}
sized_serialize_to_serialize! {i8 i16 i32 i64 i128 u8 u16 u32 u64 u128}

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

impl<T: ParseableOwned> ParseableOwned for Result<T, i32> {
    fn parse_from(arg: &[u8]) -> Result<(Self, usize), i32> {
        let (res, len) = T::parse_from(arg)?;
        Ok((Ok(res), len))
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

impl SizedSerializable for () {
    const SZ: usize = 0;

    fn do_sized_serialize(&self, _: &mut [MaybeUninit<u8>]) -> Option<()> {
        Some(())
    }
}

sized_serialize_to_serialize! { () }

impl<T1: SizedSerializable, T2: SizedSerializable> SizedSerializable
    for (T1, T2)
{
    const SZ: usize = T1::SZ + T2::SZ;

    fn do_sized_serialize(&self, buf: &mut [MaybeUninit<u8>]) -> Option<()> {
        let mut offset: usize = 0;

        self.0.do_sized_serialize(buf.get_mut(offset..)?)?;
        offset += T1::SZ;

        self.1.do_sized_serialize(buf.get_mut(offset..)?)?;

        Some(())
    }
}

impl<T1: SizedSerializable, T2: SizedSerializable, T3: SizedSerializable>
    SizedSerializable for (T1, T2, T3)
{
    const SZ: usize = T1::SZ + T2::SZ + T3::SZ;

    fn do_sized_serialize(&self, buf: &mut [MaybeUninit<u8>]) -> Option<()> {
        let mut offset: usize = 0;

        self.0.do_sized_serialize(buf.get_mut(offset..)?)?;
        offset += T1::SZ;

        self.1.do_sized_serialize(buf.get_mut(offset..)?)?;
        offset += T2::SZ;

        self.2.do_sized_serialize(buf.get_mut(offset..)?)?;

        Some(())
    }
}

impl<
        T1: SizedSerializable,
        T2: SizedSerializable,
        T3: SizedSerializable,
        T4: SizedSerializable,
    > SizedSerializable for (T1, T2, T3, T4)
{
    const SZ: usize = T1::SZ + T2::SZ + T3::SZ + T4::SZ;

    fn do_sized_serialize(&self, buf: &mut [MaybeUninit<u8>]) -> Option<()> {
        let mut offset: usize = 0;

        self.0.do_sized_serialize(buf.get_mut(offset..)?)?;
        offset += T1::SZ;

        self.1.do_sized_serialize(buf.get_mut(offset..)?)?;
        offset += T2::SZ;

        self.2.do_sized_serialize(buf.get_mut(offset..)?)?;
        offset += T3::SZ;

        self.3.do_sized_serialize(buf.get_mut(offset..)?)?;

        Some(())
    }
}

impl<T1: Serializable, T2: Serializable> Serializable for (T1, T2) {
    fn serialize_len(&self) -> usize {
        self.0.serialize_len() + self.1.serialize_len()
    }

    fn do_serialize(&self, buf: &mut [MaybeUninit<u8>]) -> Option<usize> {
        let mut offset: usize = 0;

        offset += self.0.do_serialize(buf.get_mut(offset..)?)?;

        offset += self.1.do_serialize(buf.get_mut(offset..)?)?;

        Some(offset)
    }
}

impl<T1: Serializable, T2: Serializable, T3: Serializable> Serializable
    for (T1, T2, T3)
{
    fn serialize_len(&self) -> usize {
        self.0.serialize_len() + self.1.serialize_len() + self.2.serialize_len()
    }

    fn do_serialize(&self, buf: &mut [MaybeUninit<u8>]) -> Option<usize> {
        let mut offset: usize = 0;

        offset += self.0.do_serialize(buf.get_mut(offset..)?)?;

        offset += self.1.do_serialize(buf.get_mut(offset..)?)?;

        offset += self.2.do_serialize(buf.get_mut(offset..)?)?;

        Some(offset)
    }
}

impl<
        T1: Serializable,
        T2: Serializable,
        T3: Serializable,
        T4: Serializable,
    > Serializable for (T1, T2, T3, T4)
{
    fn serialize_len(&self) -> usize {
        self.0.serialize_len()
            + self.1.serialize_len()
            + self.2.serialize_len()
            + self.3.serialize_len()
    }

    fn do_serialize(&self, buf: &mut [MaybeUninit<u8>]) -> Option<usize> {
        let mut offset: usize = 0;

        offset += self.0.do_serialize(buf.get_mut(offset..)?)?;

        offset += self.1.do_serialize(buf.get_mut(offset..)?)?;

        offset += self.2.do_serialize(buf.get_mut(offset..)?)?;

        offset += self.3.do_serialize(buf.get_mut(offset..)?)?;

        Some(offset)
    }
}

impl OutputReturnable for () {
    fn return_self(&self) -> Result<(), i32> {
        crate::syscall_bindings::raw_Return(&[])
    }
}

impl OutputReturnable for &[u8] {
    fn return_self(&self) -> Result<(), i32> {
        crate::syscall_bindings::raw_Return(self)
    }
}

impl<T: OutputReturnable> OutputReturnable for Result<T, i32> {
    fn return_self(&self) -> Result<(), i32> {
        match self {
            Ok(x) => x.return_self(),
            Err(code) => Err(*code),
        }
    }
}

impl<T: OutputReturnable> OutputReturnable for Option<T> {
    fn return_self(&self) -> Result<(), i32> {
        match self {
            Some(x) => x.return_self(),
            None => Err(-1),
        }
    }
}

impl<'a, const N: usize> Parseable<'a> for &'a [u8; N] {
    fn parse_from(arg: &'a [u8]) -> (Self, usize) {
        let out: &'a [u8; N] = match (match arg.get(..N) {
            Some(x) => x,
            None => Exit(SyscallErrorCode::kCalldataParseError as i32),
        })
        .try_into()
        {
            Ok(x) => x,
            Err(_) => Exit(SyscallErrorCode::kCalldataParseError as i32),
        };
        (out, N)
    }
}

impl<const N: usize> SizedSerializable for &[u8; N] {
    const SZ: usize = N;

    fn do_sized_serialize(&self, slice: &mut [MaybeUninit<u8>]) -> Option<()> {
        debug_assert!(slice.len() == N);
        unsafe {
            core::ptr::copy_nonoverlapping(
                self.as_ptr(),
                MaybeUninit::slice_as_mut_ptr(slice),
                N,
            );
        }
        Some(())
    }
}

impl<const N: usize> Serializable for &[u8; N] {
    fn serialize_len(&self) -> usize {
        N
    }

    fn do_serialize(&self, slice: &mut [MaybeUninit<u8>]) -> Option<usize> {
        debug_assert!(slice.len() == N);
        unsafe {
            core::ptr::copy_nonoverlapping(
                self.as_ptr(),
                MaybeUninit::slice_as_mut_ptr(slice),
                N,
            );
        }
        Some(N)
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

    fn do_serialize(&self, slice: &mut [MaybeUninit<u8>]) -> Option<usize> {
        debug_assert!(slice.len() == self.len());
        unsafe {
            core::ptr::copy_nonoverlapping(
                self.as_ptr(),
                MaybeUninit::slice_as_mut_ptr(slice),
                self.len(),
            );
        }
        Some(slice.len())
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

// parse_from_calldata happens inside the sdk, before any user code,
// so we just call exit(code) there instead of passing that error back up.

// Will fail if buf.len() != sum(args.serialize_len())
pub fn serialize_to_calldata<T: Serializable>(
    buf: &mut [MaybeUninit<u8>],
    arg: T,
) -> Result<(), i32> {
    let len: usize = arg
        .do_serialize(buf)
        .ok_or(SyscallErrorCode::kCalldataParseError as i32)?;
    if len != buf.len() {
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

pub fn parse_combinator_1<'a, T1: Parseable<'a>, R>(
    bytes: &'a [u8],
    main_func: fn(T1) -> R,
) -> (R, usize) {
    let mut offset: usize = 0;

    let (arg1, arg1len) = T1::parse_from(match bytes.get(offset..) {
        Some(b) => b,
        None => Exit(SyscallErrorCode::kCalldataParseError as i32),
    });
    offset += arg1len;

    (main_func(arg1), offset)
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

pub fn parse_combinator_2<'a, T1: Parseable<'a>, T2: Parseable<'a>, R>(
    bytes: &'a [u8],
    main_func: fn(T1, T2) -> R,
) -> (R, usize) {
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

    (main_func(arg1, arg2), offset)
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

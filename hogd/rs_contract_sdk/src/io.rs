//! Support for serializing data into contract storage

use crate::result::{Error, Result};

#[cfg(any(feature = "alloc", feature = "std"))]
use alloc::vec::Vec;

pub struct Cursor<T> {
    inner: T,
    pos: usize
}

impl<T> Cursor<T> 
{
    pub fn new(inner: T) -> Self {
        Cursor { inner, pos: 0 }
    }

    pub fn position(&self) -> usize {
        self.pos
    }

    pub fn into_inner(self) -> T {
        self.inner
    }
}

pub trait Write {
    fn write(&mut self, input: &[u8]) -> Result<()>;
}

impl Write for Cursor<&mut [u8]> {
    /// Will panic if there is not sufficient space
    fn write(&mut self, input: &[u8]) -> Result<()> {
        let len = input.len();
        let end = self.pos + len;
        let write_buf = &mut self.inner[self.pos .. end];
        write_buf.copy_from_slice(input);   // XXX: replace panic with error
        self.pos = end;
        Ok(())
    }
}

impl<const N: usize> Write for Cursor<[u8; N]> {
    fn write(&mut self, input: &[u8]) -> Result<()> {
        let len = input.len();
        let end = self.pos + len;
        let write_buf = &mut self.inner[self.pos .. end];
        write_buf.copy_from_slice(input);   // XXX: replace panic with error
        self.pos = end;
        Ok(())
    }
}

#[cfg(any(feature = "alloc", feature = "std"))]
impl Write for Vec<u8> {
    fn write(&mut self, input: &[u8]) -> Result<()> {
        self.extend_from_slice(input);
        Ok(())
    }
}

pub trait Read {
    fn read(&mut self, output: &mut [u8]) -> Result<()>;
}

impl Read for Cursor<&[u8]> {
    fn read(&mut self, output: &mut [u8]) -> Result<()> {
        let len = output.len();
        let end = self.pos + len;
        output.copy_from_slice(&self.inner[self.pos..end]); // XXX: replace panic with error
        self.pos = end;
        Ok(())
    }
}

// ------------------ Serialize -----------------

pub trait Serialize {
    fn serialize<W: Write>(&self, writer: &mut W) -> Result<()>;
}

impl Serialize for () {
    fn serialize<W: Write>(&self, _writer: &mut W) -> Result<()> {
        Ok(())
    }
}

impl Serialize for [u8] {
    fn serialize<W: Write>(&self, writer: &mut W) -> Result<()>
    {
        writer.write(self)
    }
}

impl<const N: usize> Serialize for [u8; N]
{
    fn serialize<W: Write>(&self, writer: &mut W) -> Result<()>
    {
        writer.write(self.as_slice())
    }
}

impl<T> Serialize for [T]
    where T: Serialize
{
    fn serialize<W: Write>(&self, writer: &mut W) -> Result<()>
    {
        for v in self {
            v.serialize(writer)?;
        }
        Ok(())
    }
}

impl<T, const N: usize> Serialize for [T; N]
    where T: Serialize
{
    fn serialize<W: Write>(&self, writer: &mut W) -> Result<()>
    {
        for v in self {
            v.serialize(writer)?;
        }
        Ok(())
    }
}

#[derive(Debug, PartialEq)]
pub struct Pair<A, B>(pub A, pub B);

impl<A, B> Serialize for Pair<A, B>
    where A: Serialize, B: Serialize
{
    fn serialize<W: Write>(&self, writer: &mut W) -> Result<()> {
        self.0.serialize(writer)?;
        self.1.serialize(writer)
    }
}

impl<A, B> Serialize for (&A, &B)
    where A: Serialize, B: Serialize
{
    fn serialize<W: Write>(&self, writer: &mut W) -> Result<()> {
        self.0.serialize(writer)?;
        self.1.serialize(writer)
    }
}

#[cfg(any(feature = "alloc", feature = "std"))]
pub fn serialize_to_buf<V: Serialize>(v: &V) -> Result<Vec<u8>> {
    let mut buf = Vec::new();
    v.serialize(&mut buf)?;
    Ok(buf)
}

// ------------------- Deserialize --------------------

pub trait Deserialize: Sized {
    fn deserialize<R: Read>(reader: &mut R) -> Result<Self>;
}

impl<const N: usize> Deserialize for [u8; N]
{
    fn deserialize<R: Read>(reader: &mut R) -> Result<Self> {
        let mut buf = [0; N];
        reader.read(&mut buf)?;
        Ok(buf)
    }
}

pub fn deserialize<R, T>(reader: &mut R) -> Result<T>
    where R: Read, T: Deserialize
{
    Deserialize::deserialize(reader)
}

pub fn deserialize_from_slice<T>(buf: &[u8]) -> Result<T>
    where T: Deserialize
{
    let mut cur = Cursor::new(buf);
    let result = deserialize(&mut cur)?;
    if cur.position() != buf.len() {
        return Err(Error::IO("deserialize: trailing data"))
    }
    Ok(result)
}

use std::cell::RefCell;
use std::ffi::{c_int, c_uint, c_void};
use std::ptr::{NonNull, null_mut};

use crate::error::{Error, MISUSE, NOMEM, Result, zeros};

unsafe extern "C" {
    fn init_rs_int(
        symbol_bits: c_int,
        polynomial: c_int,
        first_root: c_int,
        root_step: c_int,
        roots: c_int,
        padding: c_int,
    ) -> *mut c_void;
    fn free_rs_int(codec: *mut c_void);
    fn encode_rs_int(codec: *mut c_void, data: *const c_uint, parity: *mut c_uint);
    fn decode_rs_int(
        codec: *mut c_void,
        data: *mut c_uint,
        positions: *mut c_int,
        erasures: c_int,
    ) -> c_int;
}

pub struct Fec {
    native: NonNull<c_void>,
    data_bytes: usize,
    parity_bytes: usize,
}

impl Fec {
    pub fn new(data_bytes: usize, parity_bytes: usize) -> Result<Self> {
        if data_bytes == 0
            || !data_bytes.is_multiple_of(2)
            || parity_bytes == 0
            || !parity_bytes.is_multiple_of(2)
            || parity_bytes > 1024
        {
            return Err(Error::new(MISUSE, "Reed-Solomon byte lengths are invalid"));
        }
        let symbols = data_bytes
            .checked_add(parity_bytes)
            .ok_or_else(|| Error::new(MISUSE, "Reed-Solomon length overflow"))?
            / 2;
        if symbols > 65_535 {
            return Err(Error::new(
                MISUSE,
                "Reed-Solomon codeword exceeds field size",
            ));
        }
        let padding = 65_535 - symbols;
        // Fixed parameters are valid; a null result here indicates allocation failure.
        let native = unsafe {
            init_rs_int(
                16,
                0x1100b,
                0,
                1,
                (parity_bytes / 2) as c_int,
                padding as c_int,
            )
        };
        let native = NonNull::new(native)
            .ok_or_else(|| Error::new(NOMEM, "Reed-Solomon initialization failed"))?;
        Ok(Self {
            native,
            data_bytes,
            parity_bytes,
        })
    }

    pub fn encode(&self, data: &[u8]) -> Result<Vec<u8>> {
        if data.len() != self.data_bytes {
            return Err(Error::new(MISUSE, "Reed-Solomon data length is invalid"));
        }
        let mut symbols = zeros::<u32>(data.len() / 2)?;
        for (symbol, bytes) in symbols.iter_mut().zip(data.chunks_exact(2)) {
            *symbol = u16::from_le_bytes([bytes[0], bytes[1]]) as u32;
        }
        let mut parity = zeros::<u32>(self.parity_bytes / 2)?;
        // The codec tables are read-only during encoding; scratch belongs to this call.
        unsafe { encode_rs_int(self.native.as_ptr(), symbols.as_ptr(), parity.as_mut_ptr()) };
        let mut bytes = zeros::<u8>(self.parity_bytes)?;
        for (out, symbol) in bytes.chunks_exact_mut(2).zip(parity) {
            out.copy_from_slice(&(symbol as u16).to_le_bytes());
        }
        Ok(bytes)
    }

    pub fn correct(&self, data: &mut [u8], parity: &mut [u8]) -> Result<usize> {
        if data.len() != self.data_bytes || parity.len() != self.parity_bytes {
            return Err(Error::new(
                MISUSE,
                "Reed-Solomon correction lengths are invalid",
            ));
        }
        let count = (self.data_bytes + self.parity_bytes) / 2;
        let mut symbols = zeros::<u32>(count)?;
        for (symbol, bytes) in symbols
            .iter_mut()
            .zip(data.chunks_exact(2).chain(parity.chunks_exact(2)))
        {
            *symbol = u16::from_le_bytes([bytes[0], bytes[1]]) as u32;
        }
        // No erasure locations are supplied; correction uses per-call data and stack scratch.
        let corrected =
            unsafe { decode_rs_int(self.native.as_ptr(), symbols.as_mut_ptr(), null_mut(), 0) };
        if corrected < 0 {
            return Err(Error::corrupt("Reed-Solomon correction failed"));
        }
        for (bytes, symbol) in data
            .chunks_exact_mut(2)
            .chain(parity.chunks_exact_mut(2))
            .zip(symbols)
        {
            bytes.copy_from_slice(&(symbol as u16).to_le_bytes());
        }
        Ok(corrected as usize)
    }
}

impl Drop for Fec {
    fn drop(&mut self) {
        // This owner alone frees the context after all operations have completed.
        unsafe { free_rs_int(self.native.as_ptr()) };
    }
}

// Each thread reuses at most four codec tables. No native pointer crosses threads.
thread_local! {
    static CACHE: RefCell<Vec<Fec>> = const { RefCell::new(Vec::new()) };
}

fn with_codec<T>(data: usize, parity: usize, task: impl FnOnce(&Fec) -> Result<T>) -> Result<T> {
    CACHE.with(|cell| {
        let mut cache = cell.borrow_mut();
        let index = match cache
            .iter()
            .position(|codec| codec.data_bytes == data && codec.parity_bytes == parity)
        {
            Some(index) => index,
            None => {
                let codec = Fec::new(data, parity)?;
                if cache.len() == 4 {
                    cache.remove(0);
                }
                cache
                    .try_reserve(1)
                    .map_err(|_| Error::new(NOMEM, "codec cache allocation failed"))?;
                cache.push(codec);
                cache.len() - 1
            }
        };
        task(&cache[index])
    })
}

pub fn encode(data: &[u8], parity_bytes: usize) -> Result<Vec<u8>> {
    with_codec(data.len(), parity_bytes, |codec| codec.encode(data))
}

pub fn correct(data: &mut [u8], parity: &mut [u8]) -> Result<usize> {
    with_codec(data.len(), parity.len(), |codec| {
        codec.correct(data, parity)
    })
}

#[cfg(test)]
#[path = "../tests/unit/fec.rs"]
mod tests;

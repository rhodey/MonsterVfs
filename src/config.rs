use crate::error::{Error, MISUSE, Result};

pub const BLOCK_SIZE: usize = 4096;
pub const ALIGNMENT: usize = 4096;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub struct Settings {
    pub parity_bytes: usize,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            parity_bytes: if cfg!(feature = "copy") { 0 } else { 32 },
        }
    }
}

impl Settings {
    pub fn new(parity_bytes: usize) -> Result<Self> {
        let settings = Self { parity_bytes };
        settings.validate()?;
        Ok(settings)
    }

    pub fn validate(self) -> Result<()> {
        if cfg!(feature = "copy") {
            if self.parity_bytes != 0 {
                return Err(Error::new(MISUSE, "this build has no parity bytes"));
            }
        } else if !(32..=1024).contains(&self.parity_bytes) || !self.parity_bytes.is_multiple_of(2)
        {
            return Err(Error::new(
                MISUSE,
                "parity bytes must be even and from 32 through 1024",
            ));
        }
        Ok(())
    }
}

#[cfg(all(test, feature = "parity"))]
#[path = "../tests/unit/config.rs"]
mod tests;

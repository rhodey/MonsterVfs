use std::collections::BTreeMap;

use crate::error::{Error, Result};
use crate::format::RESERVED_SIZE;
use crate::native::Physical;

const CHUNK: usize = 512;

/// Bytes as SQLite writes them: the end of every 4096-byte page stays zero.
pub fn page_bytes(mut bytes: Vec<u8>) -> Vec<u8> {
    for page in bytes.chunks_mut(4096) {
        page.iter_mut()
            .skip(4096 - RESERVED_SIZE)
            .for_each(|byte| *byte = 0);
    }
    bytes
}

pub fn filled(fill: u8, length: usize) -> Vec<u8> {
    page_bytes(vec![fill; length])
}

#[derive(Clone, Debug, Default)]
pub struct Image {
    pub length: u64,
    pub pages: BTreeMap<u64, [u8; CHUNK]>,
}

impl Image {
    pub fn bytes(&self, offset: u64, output: &mut [u8]) {
        output.fill(0);
        let mut done = 0;
        while done < output.len() {
            let at = offset + done as u64;
            let key = at / CHUNK as u64;
            let within = (at % CHUNK as u64) as usize;
            let count = (output.len() - done).min(CHUNK - within);
            if let Some(page) = self.pages.get(&key) {
                output[done..done + count].copy_from_slice(&page[within..within + count]);
            }
            done += count;
        }
    }

    pub fn write(&mut self, offset: u64, bytes: &[u8]) {
        let mut done = 0;
        while done < bytes.len() {
            let at = offset + done as u64;
            let key = at / CHUNK as u64;
            let within = (at % CHUNK as u64) as usize;
            let count = (bytes.len() - done).min(CHUNK - within);
            if bytes[done..done + count].iter().any(|&byte| byte != 0)
                || self.pages.contains_key(&key)
            {
                let page = self.pages.entry(key).or_insert([0; CHUNK]);
                page[within..within + count].copy_from_slice(&bytes[done..done + count]);
                if page.iter().all(|&byte| byte == 0) {
                    self.pages.remove(&key);
                }
            }
            done += count;
        }
        self.length = self.length.max(offset + bytes.len() as u64);
    }

    pub fn truncate(&mut self, length: u64) {
        if length < self.length {
            self.pages.retain(|&key, _| key * (CHUNK as u64) < length);
            let tail = (length % CHUNK as u64) as usize;
            if tail != 0
                && let Some(page) = self.pages.get_mut(&(length / CHUNK as u64))
            {
                page[tail..].fill(0);
            }
        }
        self.length = length;
    }

    pub fn flip(&mut self, offset: u64, bits: u8) {
        assert!(offset < self.length);
        let mut byte = [0];
        self.bytes(offset, &mut byte);
        byte[0] ^= bits;
        self.write(offset, &byte);
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Op {
    Read,
    Write,
    Size,
    Truncate,
    Sync,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Event {
    Read(u64, usize),
    Write(u64, usize),
    Size,
    Truncate(u64),
    Sync(i32),
}

#[derive(Clone, Debug)]
struct Fault {
    op: Op,
    skip: usize,
    partial: usize,
    code: i32,
}

#[derive(Clone, Debug, Default)]
pub struct Io {
    pub visible: Image,
    pub durable: Image,
    pub events: Vec<Event>,
    // Every read that touches this 4096-byte sector fails with this code.
    pub bad_sector: Option<(u64, i32)>,
    fault: Option<Fault>,
}

impl Io {
    pub fn fail(&mut self, op: Op, skip: usize, partial: usize, code: i32) {
        self.fault = Some(Fault {
            op,
            skip,
            partial,
            code,
        });
    }

    pub fn clear_fault(&mut self) {
        self.fault = None;
    }

    fn take_fault(&mut self, op: Op) -> Option<Fault> {
        let fault = self.fault.as_mut()?;
        if fault.op != op {
            return None;
        }
        if fault.skip != 0 {
            fault.skip -= 1;
            return None;
        }
        self.fault.take()
    }

    pub fn crash(&self) -> Self {
        Self {
            visible: self.durable.clone(),
            durable: self.durable.clone(),
            events: Vec::new(),
            bad_sector: None,
            fault: None,
        }
    }

    pub fn write_count(&self) -> usize {
        self.events
            .iter()
            .filter(|event| matches!(event, Event::Write(..)))
            .count()
    }
}

impl Physical for Io {
    fn read(&mut self, offset: u64, bytes: &mut [u8]) -> Result<()> {
        self.events.push(Event::Read(offset, bytes.len()));
        if let Some(fault) = self.take_fault(Op::Read) {
            let part = fault.partial.min(bytes.len());
            self.visible.bytes(offset, &mut bytes[..part]);
            return Err(Error::new(fault.code, "injected read failure"));
        }
        if let Some((sector, code)) = self.bad_sector
            && offset < sector + 4096
            && offset + bytes.len() as u64 > sector
        {
            return Err(Error::new(code, "injected bad sector"));
        }
        if offset
            .checked_add(bytes.len() as u64)
            .is_none_or(|end| end > self.visible.length)
        {
            return Err(Error::new(522, "unexpected physical short read"));
        }
        self.visible.bytes(offset, bytes);
        Ok(())
    }

    fn write(&mut self, offset: u64, bytes: &[u8]) -> Result<()> {
        self.events.push(Event::Write(offset, bytes.len()));
        if let Some(fault) = self.take_fault(Op::Write) {
            self.visible
                .write(offset, &bytes[..fault.partial.min(bytes.len())]);
            return Err(Error::new(fault.code, "injected write failure"));
        }
        self.visible.write(offset, bytes);
        Ok(())
    }

    fn size(&mut self) -> Result<u64> {
        self.events.push(Event::Size);
        if let Some(fault) = self.take_fault(Op::Size) {
            return Err(Error::new(fault.code, "injected size failure"));
        }
        Ok(self.visible.length)
    }

    fn truncate(&mut self, length: u64) -> Result<()> {
        self.events.push(Event::Truncate(length));
        if let Some(fault) = self.take_fault(Op::Truncate) {
            if fault.partial != 0 {
                self.visible.truncate(length);
            }
            return Err(Error::new(fault.code, "injected truncate failure"));
        }
        self.visible.truncate(length);
        Ok(())
    }

    fn sync(&mut self, flags: i32) -> Result<()> {
        self.events.push(Event::Sync(flags));
        if let Some(fault) = self.take_fault(Op::Sync) {
            if fault.partial != 0 {
                self.durable = self.visible.clone();
            }
            return Err(Error::new(fault.code, "injected sync failure"));
        }
        self.durable = self.visible.clone();
        Ok(())
    }
}

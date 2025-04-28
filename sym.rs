#!/usr/bin/env -S cargo -Zscript

//! ```cargo
//! [profile.dev]
//! opt-level=3
//!
//! [dependencies]
//! rayon = "1"
//! walkdir = "2"
//! ```

use std::fs;

#[derive(Debug)]
struct BSlugSymbol {
    name: &'static str,
    size: u32,
    offset: u32,
    data: &'static [u32],
}

#[derive(Debug, Copy, Clone)]
struct GameSection {
    offset: u32,
    start_addr: u32,
    end_addr: u32,
}

fn main() {
    #[rustfmt::skip]
    let mut sections = [
        GameSection { offset: 0x100,  start_addr: 0x80004000, end_addr: 0x80006460 },
        GameSection { offset: 0x2560,  start_addr: 0x800072c0, end_addr: 0x80244de0 },
        GameSection { offset: 0x240080,  start_addr: 0x80006460, end_addr: 0x80006a20 },
        GameSection { offset: 0x240640,  start_addr: 0x80006a20, end_addr: 0x800072c0 },
        GameSection { offset: 0x240ee0,  start_addr: 0x80244de0, end_addr: 0x80244ea0 },
        GameSection { offset: 0x240fa0,  start_addr: 0x80244ea0, end_addr: 0x80244ec0 },
        GameSection { offset: 0x240fc0,  start_addr: 0x80244ec0, end_addr: 0x80258580 },
        GameSection { offset: 0x254680,  start_addr: 0x80258580, end_addr: 0x802a4040 },
        GameSection { offset: 0x2a0140,  start_addr: 0x80384c00, end_addr: 0x80385fc0 },
        GameSection { offset: 0x2a1500,  start_addr: 0x80386fa0, end_addr: 0x80389140 },
    ];
    sections.sort_by_key(|v| v.offset);
    let file_off_to_addr = |file_offset: u32| {
        let GameSection {
            start_addr, offset, ..
        } = sections
            .iter()
            .copied()
            .filter(|&GameSection { offset, .. }| file_offset >= offset)
            .last()
            .expect("unmapped memory");

        start_addr + (file_offset - offset)
    };

    #[rustfmt::skip]
    let symbols = [
        // BSlugSymbol { name: "VIWaitForRetrace", size: 0x54, offset: 0x2c, data: &[0x7C1E0040, 0x4182FFF0, 0x7FE3FB78] },
        // BSlugSymbol { name: "iosAllocAligned", size: 0x204, offset: 0x40, data: &[0x381EFFFF, 0x7FC00039] }
        BSlugSymbol {name:"iosCreateHeap",size:0x130,offset:0x28,data:&[0x57A006FF, 0x408200E0, 0x3C80]},
        // BSlugSymbol { name: "strncpy", size: 0x44, offset: 0x0, data: &[0x3884FFFF, 0x38C3FFFF, 0x38A50001, 0x4800002C] },
        // BSlugSymbol { name: "__lcase", size: 0x100, offset: 0x58, data: &[0x78797A5B, 0x5C5D5E5F, 0x60616263] },
        // BSlugSymbol { name: "__ucase", size: 0x100, offset: 0x58, data: &[0x58595A5B, 0x5C5D5E5F, 0x60414243] },
        // BSlugSymbol { name: "OSUTF8to32", size: 0x110, offset: 0x0, data: &[0x88C30000, 0x2C060000, 0x41820008, 0x38630001] },
        // BSlugSymbol { name: "OSUTF32toANSI", size: 0x78, offset: 0x0, data: &[0x280300FF, 0x4081000C, 0x38600000, 0x4E800020] },
        // BSlugSymbol { name: "OSLockMutex", size: 0xdc, offset: 0x54, data: &[0x2C030000, 0x4082000C, 0x939E02F4, 0x48000008] },
        // BSlugSymbol { name: "OSUnlockMutex", size: 0xc8, offset: 0x50, data: &[0x2C040000, 0x4082000C, 0x90A302F8, 0x48000008] },
        // BSlugSymbol { name: "OSGetCurrentThread", size: 0xc, offset: 0x0, data: &[0x3C608000, 0x806300E4, 0x4E800020] },
        // BSlugSymbol { name: "OSGetTime", size: 0x18, offset: 0x0, data: &[0x7C6D42E6, 0x7C8C42E6] },
        // BSlugSymbol { name: "OSTicksToCalendarTime", size: 0x1c8, offset: 0x68, data: &[0x3C80431C, 0x57E31838] },
    ];
    let file = fs::read("mkw.dol").unwrap();
    'symbols: for symbol in symbols {
        let windows_len = 4 * symbol.data.len();
        for (chunk_off, chunk) in file.windows(windows_len).enumerate() {
            if chunk
                .chunks_exact(4)
                .map(|v| u32::from_be_bytes(v.try_into().unwrap()))
                .eq(symbol.data.iter().copied())
            {
                let file_offset = u32::try_from(chunk_off).unwrap() - symbol.offset;
                // println!(
                //     "{:?} found at {:x} (addr: {:x})",
                //     symbol,
                //     file_offset,
                //     file_off_to_addr(file_offset)
                // );
                /*
                PROVIDE(__lcase = 0x123);
                 */
                println!(
                    "PROVIDE({} = {:#x});",
                    symbol.name,
                    file_off_to_addr(file_offset)
                );
                // continue 'symbols;
            }
        }
        eprintln!("Symbol {} not found!", symbol.name);
    }
    println!("{}", file.len());
}

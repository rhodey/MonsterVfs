use super::*;

fn multiply(mut a: u32, mut b: u32) -> u32 {
    let mut result = 0;
    while b != 0 {
        if b & 1 != 0 {
            result ^= a;
        }
        b >>= 1;
        a <<= 1;
        if a & 0x10000 != 0 {
            a ^= 0x1100b;
        }
    }
    result
}

// Independent polynomial division, with no libfec lookup-table implementation.
fn reference(data: &[u8], roots: usize) -> Vec<u8> {
    let mut generator = vec![1u32];
    let mut root = 1;
    for _ in 0..roots {
        let mut next = vec![0; generator.len() + 1];
        for (i, &coefficient) in generator.iter().enumerate() {
            next[i] ^= coefficient;
            next[i + 1] ^= multiply(coefficient, root);
        }
        generator = next;
        root = multiply(root, 2);
    }
    let mut work: Vec<u32> = data
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]) as u32)
        .collect();
    let count = work.len();
    work.resize(count + roots, 0);
    for i in 0..count {
        let coefficient = work[i];
        for (j, &factor) in generator.iter().enumerate() {
            work[i + j] ^= multiply(coefficient, factor);
        }
    }
    work[count..]
        .iter()
        .flat_map(|&symbol| (symbol as u16).to_le_bytes())
        .collect()
}

#[test]
fn parity_matches_independent_polynomial_reference() {
    for (size, parity) in [(16, 32), (128, 32), (4128, 32), (64, 1024)] {
        let data: Vec<u8> = (0..size).map(|i| ((i * 73 + i / 7) & 255) as u8).collect();
        assert_eq!(encode(&data, parity).unwrap(), reference(&data, parity / 2));
    }
}

#[test]
fn corrects_unknown_errors_at_the_correction_limit() {
    let original: Vec<u8> = (0..4128).map(|i| (i * 37) as u8).collect();
    let original_parity = encode(&original, 32).unwrap();
    let mut data = original.clone();
    let mut parity = original_parity.clone();
    for i in 0..8 {
        data[2 * (i * 73 + 1)] ^= 0x5b;
    }
    assert_eq!(correct(&mut data, &mut parity).unwrap(), 8);
    assert_eq!(data, original);
    assert_eq!(parity, original_parity);
}

#[test]
fn protects_callers_from_invalid_lengths() {
    for (data, parity) in [
        (0, 32),
        (3, 32),
        (128, 0),
        (128, 33),
        (128, 1026),
        (usize::MAX, 32),
        (usize::MAX - 1, 32),
        (131_040, 32),
    ] {
        assert!(Fec::new(data, parity).is_err());
    }
    let fec = Fec::new(128, 32).unwrap();
    assert!(fec.encode(&[0; 126]).is_err());
    assert!(fec.correct(&mut [0; 126], &mut [0; 32]).is_err());
    assert!(fec.correct(&mut [0; 128], &mut [0; 30]).is_err());
}

#[test]
fn parity_errors_are_correctable_and_cache_is_bounded() {
    let original = vec![42u8; 128];
    let mut data = original.clone();
    let mut parity = encode(&data, 32).unwrap();
    assert_eq!(correct(&mut data, &mut parity).unwrap(), 0);
    assert_eq!(data, original);
    parity[0] ^= 1;
    data[0] ^= 2;
    assert_eq!(correct(&mut data, &mut parity).unwrap(), 2);
    assert_eq!(data, original);
    assert_eq!(parity, encode(&original, 32).unwrap());
    for size in (16..36).step_by(2) {
        encode(&vec![0; size], 32).unwrap();
    }
    CACHE.with(|cache| assert_eq!(cache.borrow().len(), 4));
}

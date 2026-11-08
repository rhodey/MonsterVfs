use super::*;

#[test]
fn valid_settings_and_defaults() {
    assert_eq!(Settings::new(32).unwrap(), Settings::default());
    for parity in (32..=1024).step_by(2) {
        assert_eq!(Settings::new(parity).unwrap().parity_bytes, parity);
    }
}

#[test]
fn invalid_settings_are_rejected() {
    for parity in [0, 30, 31, 33, 1025, usize::MAX] {
        assert_eq!(Settings::new(parity).unwrap_err().code, MISUSE);
    }
}

pub fn dbj2_hash(s: &str) -> u32 {
    let mut hash: u32 = 5381;
    for c in s.chars() {
        hash = (hash.wrapping_shl(5)).wrapping_add(hash).wrapping_add(c as u32);
    }
    hash
}

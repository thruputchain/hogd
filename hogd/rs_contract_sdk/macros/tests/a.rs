use macros::contract;

#[contract]
trait Silly {
    fn greet(self: &Self, x: u32, y: u64) -> u128;
    fn insult(self: &Self, x: u32, y: u64) -> u128;
}

struct Me;

impl Silly for Me {
    fn greet(&self, x: u32, y: u64) -> u128 {
        x as u128 + y as u128
    }
    fn insult(&self, x: u32, y: u64) -> u128 {
        x as u128 - y as u128
    }
}

#[test]
fn test_a() {
    let me = Me;

    println!("Hi! {} {}", me.greet(5, 10), me.insult(7, 3));
}

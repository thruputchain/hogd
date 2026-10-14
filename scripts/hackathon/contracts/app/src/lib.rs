#![no_std]
use sdk::*;

// The corpus builder replaces these constants before compiling each artifact.
const VARIANT: u64 = 1;
const FAMILY: u64 = 0;

#[contract(client = Client)]
pub trait HackathonApp {
    fn initialize();
    fn act(sequence: u64, peer: &ContractAddress, payload: &[u8]);
    fn receive(amount: u64);
}

pub struct App;
#[contract]
impl HackathonApp for App {
    fn initialize() {
        // Each caller gets a lazily created balance; the treasury models a
        // pre-minted supply, so random arrival ordering cannot overdraw it.
        non_negative_int_inc(&b"treasury".hash().into(), 1_000_000_000_000);
    }
    fn receive(amount: u64) {
        non_negative_int_inc(&b"received".hash().into(), amount as i128);
    }
    fn act(sequence: u64, peer: &ContractAddress, payload: &[u8]) {
        let key: StorageKey = (VARIANT, get_caller()).hash().into();
        match FAMILY {
            0 => {
                let amount = (VARIANT % 7 + 1) as i128;
                non_negative_int_inc(&b"treasury".hash().into(), -amount);
                non_negative_int_inc(&key, amount);
            }
            1 => non_negative_int_inc(&key, (VARIANT % 13 + 1) as i128),
            2 => {
                // Independent records keep same-user concurrent submissions
                // from becoming a benchmark of exclusive-write conflicts.
                let slot: StorageKey =
                    (VARIANT, get_caller(), sequence % 4096).hash().into();
                bytestring_set(&slot, payload);
            }
            3 => {
                let mut digest = (VARIANT, sequence, payload).hash();
                for _ in 0..(16 + VARIANT % 64) {
                    digest = digest.hash();
                }
                sdk::sys::raw_LogEvent(None, None, None, digest.as_ref())
                    .unwrap();
            }
            _ => {
                Client::new(peer).receive(VARIANT % 17 + 1);
                sdk::sys::raw_LogEvent(
                    None,
                    None,
                    None,
                    &sequence.to_le_bytes(),
                )
                .unwrap();
            }
        }
    }
}

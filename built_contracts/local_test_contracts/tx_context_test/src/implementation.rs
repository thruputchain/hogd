use crate::TxContextTest;
use sdk::sys;
use sdk::transaction::TransactionStatistic;
use sdk::*;

pub struct TxContextTestContract;

#[contract]
impl TxContextTest for TxContextTestContract {
    fn get_source_tx_hash() -> SHA256Hash {
        sdk::get_source_transaction_hash()
    }

    fn get_block_height() -> i64 {
        let mut height: BlockHeight = 0.into();
        sys::raw_GetBlockHeight(&mut height).unwrap();
        height.into_inner()
    }

    fn test_tx_stats_gas_remaining() {
        let mut remaining: u64 = 0;
        sys::raw_GetTransactionStatistics(
            TransactionStatistic::GasRemaining,
            &mut remaining,
        )
        .unwrap();
        assert!(remaining > 0);
    }

    fn internal_get_source_tx_hash() -> SHA256Hash {
        sdk::get_source_transaction_hash()
    }

    fn test_source_tx_hash_in_child(target: &ContractAddress) {
        let root_view = sdk::get_source_transaction_hash();
        let child_view = crate::Client::new(target).internal_get_source_tx_hash();
        assert!(root_view == child_view);
        assert!(root_view != SHA256Hash::from([0u8; 32]));
    }
}

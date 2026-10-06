//! Type-checks x23c's Rust output for check.x; see check.sh.

#[rustfmt::skip]
mod generated;
pub use generated::*;

use rpc_api::{ProgramError, RpcError};
use std::sync::Arc;

struct Server;

#[async_trait::async_trait]
impl check_check_v1 for Server {
    async fn ping(&self) -> Result<(), ProgramError> {
        Ok(())
    }

    async fn add(&self, a: i32, b: u64) -> Result<i32, ProgramError> {
        Ok(a + b as i32)
    }

    async fn submit(
        &self,
        _batch: TxBatch,
        _urgent: bool,
    ) -> Result<Outcome, ProgramError> {
        Ok(Outcome::REJECTED)
    }

    async fn credit(
        &self,
        _amount: Amount,
        tx: Tx,
    ) -> Result<Receipt, ProgramError> {
        Ok(Receipt {
            outcome: Outcome::REJECTED,
            tx: Some(tx),
        })
    }

    async fn verify(&self, _hash: Hash) -> Result<bool, ProgramError> {
        Ok(true)
    }
}

pub fn dispatcher() -> check_check_v1_Dispatcher {
    check_check_v1_Dispatcher::new(Box::new(Server))
}

// Clients take any argument that encodes like the wire type, so each of these
// shapes has to keep compiling: the owned value, slices of borrowed or shared
// elements, and pointers to it.
pub async fn call_sites(
    client: &check_check_v1_Client,
    batch: &TxBatch,
    tx: &mut Tx,
) -> Result<(), RpcError> {
    client.ping().await?;
    client.add(1, 2).await?;

    client.submit(batch, true).await?;
    let arcs: Vec<Arc<Tx>> = batch.iter().cloned().map(Arc::new).collect();
    client.submit(&arcs[..], false).await?;
    let refs: Vec<&Tx> = batch.iter().collect();
    client.submit(refs.as_slice(), false).await?;

    client.credit(&5, &*tx).await?;
    client.credit(&5, tx).await?;
    client.credit(&5, &Arc::new(tx.clone())).await?;
    client.credit(&5, &Box::new(tx.clone())).await?;

    client.verify(&Hash::default()).await?;
    Ok(())
}

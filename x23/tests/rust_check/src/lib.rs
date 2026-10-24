//! Type-checks x23c's Rust output for check.x; see check.sh.

#[rustfmt::skip]
mod generated;
pub use generated::*;

use rpc_api::{ProgramError, RpcError};
use std::sync::Arc;

// In-place decoding fixes the field's lifetime behind a mutable pointer.
// Generated borrowed newtypes must accept a reader that outlives that field.
#[derive(xdrs::ReadXDR, xdrs::WriteXDR)]
pub struct BorrowedHash<'a> {
    pub hash: &'a Hash,
}

pub struct Server;

// Handlers are plain `async fn`s; a concrete reply type refines the trait's
// `impl EncodesAs<_>`.
#[allow(refining_impl_trait)]
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

    async fn pending(&self) -> Result<TxBatch, ProgramError> {
        Ok(TxBatch::default())
    }
}

// Replies can be anything that encodes like the wire type: borrowed from the
// server or shared with it, instead of an owned copy.
struct SharedServer {
    receipt: Receipt,
    pending: Vec<Arc<Tx>>,
}

#[allow(refining_impl_trait)]
impl check_check_v1 for SharedServer {
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
    ) -> Result<&Outcome, ProgramError> {
        Ok(&self.receipt.outcome)
    }

    async fn credit(
        &self,
        _amount: Amount,
        _tx: Tx,
    ) -> Result<&Receipt, ProgramError> {
        Ok(&self.receipt)
    }

    async fn verify(&self, _hash: Hash) -> Result<bool, ProgramError> {
        Ok(true)
    }

    async fn pending(&self) -> Result<Arc<[Arc<Tx>]>, ProgramError> {
        Ok(self.pending.iter().cloned().collect())
    }
}

// A handler may also return its reply opaquely, as the trait declares it.
struct OpaqueServer {
    pending: Vec<Tx>,
}

impl check_check_v1 for OpaqueServer {
    async fn ping(&self) -> Result<impl xdrs::EncodesAs<()>, ProgramError> {
        Ok(())
    }

    async fn add(
        &self,
        a: i32,
        b: u64,
    ) -> Result<impl xdrs::EncodesAs<i32>, ProgramError> {
        Ok(a + b as i32)
    }

    async fn submit(
        &self,
        _batch: TxBatch,
        _urgent: bool,
    ) -> Result<impl xdrs::EncodesAs<Outcome>, ProgramError> {
        Ok(Outcome::REJECTED)
    }

    async fn credit(
        &self,
        _amount: Amount,
        tx: Tx,
    ) -> Result<impl xdrs::EncodesAs<Receipt>, ProgramError> {
        Ok(Receipt {
            outcome: Outcome::REJECTED,
            tx: Some(tx),
        })
    }

    async fn verify(
        &self,
        _hash: Hash,
    ) -> Result<impl xdrs::EncodesAs<bool>, ProgramError> {
        Ok(true)
    }

    async fn pending(
        &self,
    ) -> Result<impl xdrs::EncodesAs<TxBatch>, ProgramError> {
        Ok(&self.pending[..])
    }
}

pub fn dispatcher() -> check_check_v1_Dispatcher<Server> {
    check_check_v1_Dispatcher::new(Box::new(Server))
}

// Dispatchers are services the RPC server holds as trait objects.
pub fn handlers(receipt: Receipt) -> Vec<Box<dyn rpc_api::RpcHandler>> {
    vec![
        Box::new(dispatcher()),
        Box::new(check_check_v1_Dispatcher::new(Box::new(SharedServer {
            receipt,
            pending: Vec::new(),
        }))),
        Box::new(check_check_v1_Dispatcher::new(Box::new(OpaqueServer {
            pending: Vec::new(),
        }))),
    ]
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
    client.pending().await?;
    Ok(())
}

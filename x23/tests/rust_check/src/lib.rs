//! Type-checks x23c's Rust output for check.x; see check.sh.

#[rustfmt::skip]
mod generated;
pub use generated::*;

use thruput_rpc_api::{ProgramError, RpcError};
use std::sync::Arc;

// In-place decoding fixes the field's lifetime behind a mutable pointer.
// Generated borrowed newtypes must accept a reader that outlives that field.
#[derive(xdrs::ReadXDR, xdrs::WriteXDR)]
pub struct BorrowedHash<'a> {
    pub hash: &'a Hash,
}

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

// Generated client futures have to stay Send to be spawned.
fn _client_futures_are_send(client: &check_check_v1_Client, batch: &TxBatch, tx: &mut Tx) {
    fn send<T: Send>(_: T) {}
    send(call_sites(client, batch, tx));
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::future::Future;
    use std::pin::pin;
    use std::task::{Context, Poll, Waker};
    use thruput_rpc_api::{CallBuf, ProgVers, RpcHandler, RpcResponse, RpcSender};

    /// Runs each call through the dispatcher in place. It keeps the default
    /// `send_call_decoded`, so the client decodes an owned response.
    struct Loopback(check_check_v1_Dispatcher);

    #[async_trait::async_trait]
    impl RpcSender for Loopback {
        fn call_buf(&self) -> CallBuf {
            CallBuf::new()
        }

        async fn send_call(
            &self,
            progvers: ProgVers,
            proc: u32,
            call: CallBuf,
        ) -> Result<RpcResponse, RpcError> {
            assert_eq!(progvers, self.0.prog_vers());
            let reply = self.0.dispatch(proc, call.payload()).await?;
            Ok(RpcResponse::from_payload(reply))
        }
    }

    fn block_on<F: Future>(f: F) -> F::Output {
        let mut f = pin!(f);
        match f.as_mut().poll(&mut Context::from_waker(Waker::noop())) {
            Poll::Ready(out) => out,
            Poll::Pending => panic!("loopback calls never wait"),
        }
    }

    #[test]
    fn clients_decode_replies() {
        let client = check_check_v1_Client::new(Arc::new(Loopback(dispatcher())));
        block_on(async {
            client.ping().await.unwrap();
            assert_eq!(client.add(1, 2).await.unwrap(), 3);
            assert!(client.verify(&Hash::default()).await.unwrap());
            let tx = Tx {
                seq: 7,
                memo: Default::default(),
                note: Default::default(),
                hash: Hash::default(),
                digest: [1; 32],
                sig: [2; SIG_SIZE as usize],
            };
            let receipt = client.credit(&5, &tx).await.unwrap();
            assert_eq!(receipt.tx, Some(tx));
        });
    }
}

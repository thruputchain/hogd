//! Type-checks x23c's Rust output for check.x; see check.sh.

#[rustfmt::skip]
mod generated;
pub use generated::*;

// Generated from plain.x without the xdrs derives.
#[rustfmt::skip]
pub mod plain;

use std::sync::Arc;
use thruput_rpc_api::{ProgramError, RpcError};

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

    async fn scale(&self, t: T) -> Result<T, ProgramError> {
        Ok(t * 2)
    }

    async fn ratio(&self, f: f32, r: Reading) -> Result<f64, ProgramError> {
        Ok(f as f64 * r.sum.total)
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

    async fn scale(&self, t: T) -> Result<T, ProgramError> {
        Ok(t * 2)
    }

    async fn ratio(&self, f: f32, r: Reading) -> Result<f64, ProgramError> {
        Ok(f as f64 * r.sum.total)
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

    async fn scale(
        &self,
        t: T,
    ) -> Result<impl xdrs::EncodesAs<T>, ProgramError> {
        Ok(t * 2)
    }

    async fn ratio(
        &self,
        f: f32,
        r: Reading,
    ) -> Result<impl xdrs::EncodesAs<f64>, ProgramError> {
        Ok(f as f64 * r.sum.total)
    }
}

pub fn dispatcher() -> check_check_v1_Dispatcher<Server> {
    check_check_v1_Dispatcher::new(Box::new(Server))
}

// Dispatchers are services the RPC server holds as trait objects.
pub fn handlers(receipt: Receipt) -> Vec<Box<dyn thruput_rpc_api::RpcHandler>> {
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
    client.scale(&3).await?;
    client.ratio(0.5, &reading()).await?;
    Ok(())
}

pub fn reading() -> Reading {
    Reading {
        level: 1.5,
        sum: Reading_sum { total: 4.0 },
        last: Reading_last::TRUE { value: -0.0 },
    }
}

// Generated client futures have to stay Send to be spawned.
fn _client_futures_are_send(
    client: &check_check_v1_Client,
    batch: &TxBatch,
    tx: &mut Tx,
) {
    fn send<T: Send>(_: T) {}
    send(call_sites(client, batch, tx));
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::future::{Future, pending, poll_fn};
    use std::pin::pin;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::task::{Context, Poll, Waker};
    use thruput_rpc_api::{
        CallBuf, ProgVers, RpcHandler, RpcResponse, RpcSender,
    };

    type Decode<'a> = &'a mut (
                dyn for<'reply> FnMut(&'reply [u8]) -> Result<(), RpcError>
                    + Send
            );

    /// Runs each call through the dispatcher in place. It keeps the default
    /// `send_call_decoded`, so the client decodes an owned response.
    struct Loopback<T>(check_check_v1_Dispatcher<T>);

    #[async_trait::async_trait]
    impl<T: check_check_v1> RpcSender for Loopback<T> {
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

    /// Decodes the way a driver connection does: the call waits once, then
    /// `decode` runs on a reply the sender owns. `send_call` is off limits.
    struct Reader<T>(check_check_v1_Dispatcher<T>);

    #[async_trait::async_trait]
    impl<T: check_check_v1> RpcSender for Reader<T> {
        fn call_buf(&self) -> CallBuf {
            CallBuf::new()
        }

        async fn send_call(
            &self,
            _: ProgVers,
            _: u32,
            _: CallBuf,
        ) -> Result<RpcResponse, RpcError> {
            panic!("generated clients call send_call_decoded");
        }

        async unsafe fn send_call_decoded(
            &self,
            progvers: ProgVers,
            proc: u32,
            call: CallBuf,
            decode: Decode<'_>,
        ) -> Result<(), RpcError> {
            assert_eq!(progvers, self.0.prog_vers());
            let reply = self.0.dispatch(proc, call.payload()).await?;
            let mut waited = false;
            poll_fn(|_| {
                if std::mem::replace(&mut waited, true) {
                    Poll::Ready(())
                } else {
                    Poll::Pending
                }
            })
            .await;
            decode(&reply)
        }
    }

    /// Never answers, and records when the call future is dropped.
    struct Stalled(Arc<AtomicBool>);

    struct SetOnDrop(Arc<AtomicBool>);

    impl Drop for SetOnDrop {
        fn drop(&mut self) {
            self.0.store(true, Ordering::SeqCst);
        }
    }

    #[async_trait::async_trait]
    impl RpcSender for Stalled {
        fn call_buf(&self) -> CallBuf {
            CallBuf::new()
        }

        async fn send_call(
            &self,
            _: ProgVers,
            _: u32,
            _: CallBuf,
        ) -> Result<RpcResponse, RpcError> {
            panic!("generated clients call send_call_decoded");
        }

        async unsafe fn send_call_decoded(
            &self,
            _: ProgVers,
            _: u32,
            _: CallBuf,
            _: Decode<'_>,
        ) -> Result<(), RpcError> {
            let _dropped = SetOnDrop(self.0.clone());
            pending().await
        }
    }

    fn block_on<F: Future>(f: F) -> F::Output {
        let mut f = pin!(f);
        let mut cx = Context::from_waker(Waker::noop());
        loop {
            if let Poll::Ready(out) = f.as_mut().poll(&mut cx) {
                return out;
            }
        }
    }

    fn tx() -> Tx {
        Tx {
            seq: 7,
            memo: Default::default(),
            note: Default::default(),
            hash: Hash::default(),
            digest: [1; 32],
            sig: [2; SIG_SIZE as usize],
        }
    }

    fn round_trip(client: check_check_v1_Client) {
        block_on(async {
            client.ping().await.unwrap();
            assert_eq!(client.add(1, 2).await.unwrap(), 3);
            assert!(client.verify(&Hash::default()).await.unwrap());
            assert_eq!(client.scale(&3).await.unwrap(), 6);
            assert_eq!(client.ratio(0.5, &reading()).await.unwrap(), 2.0);
            let tx = tx();
            let receipt = client.credit(&5, &tx).await.unwrap();
            assert_eq!(receipt.tx, Some(tx));
        });
    }

    #[test]
    fn clients_decode_owned_replies() {
        round_trip(check_check_v1_Client::new(Arc::new(
            Loopback(dispatcher()),
        )));
    }

    #[test]
    fn clients_decode_where_the_sender_holds_the_reply() {
        round_trip(check_check_v1_Client::new(Arc::new(Reader(dispatcher()))));
    }

    #[test]
    fn borrowed_and_shared_replies_decode_like_owned_ones() {
        let receipt = Receipt {
            outcome: Outcome::REJECTED,
            tx: Some(tx()),
        };
        let batch = TxBatch::try_from(vec![tx(), tx()]).unwrap();
        let shared = check_check_v1_Client::new(Arc::new(Reader(
            check_check_v1_Dispatcher::new(Box::new(SharedServer {
                receipt: receipt.clone(),
                pending: batch.iter().cloned().map(Arc::new).collect(),
            })),
        )));
        let opaque = check_check_v1_Client::new(Arc::new(Loopback(
            check_check_v1_Dispatcher::new(Box::new(OpaqueServer {
                pending: batch.to_vec(),
            })),
        )));
        block_on(async {
            assert_eq!(shared.credit(&5, &tx()).await.unwrap(), receipt);
            assert_eq!(
                shared.submit(&batch, false).await.unwrap(),
                Outcome::REJECTED
            );
            assert_eq!(shared.pending().await.unwrap(), batch);
            assert_eq!(opaque.pending().await.unwrap(), batch);
        });
    }

    #[test]
    fn dropping_a_waiting_client_call_drops_the_call() {
        let dropped = Arc::new(AtomicBool::new(false));
        let client =
            check_check_v1_Client::new(Arc::new(Stalled(dropped.clone())));
        {
            let mut call = pin!(client.add(1, 2));
            let poll =
                call.as_mut().poll(&mut Context::from_waker(Waker::noop()));
            assert!(poll.is_pending());
            assert!(!dropped.load(Ordering::SeqCst));
        }
        assert!(dropped.load(Ordering::SeqCst));
    }

    // Without the xdrs derives, a default arm is a plain variant.
    #[test]
    fn default_arms_without_the_xdrs_derives() {
        let raw = plain::Reading::LOW { raw: 3 };
        assert_eq!(raw.l(), plain::Level::LOW);
        let other = plain::Count::Default { n: 7, other: 8 };
        assert_eq!(other.n(), 7);
        let sample = plain::Sample {
            reading: plain::Reading::Default { l: 5 },
            count: plain::Count::_1 { one: 1 },
        };
        assert_eq!(sample.reading.l(), plain::Level(5));
        assert_eq!(sample.count.n(), 1);
    }
}

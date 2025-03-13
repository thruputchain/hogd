use super::CoinContract;
use super::{Allowance, Balance};
use core::fmt;
use sdk::bytestring::BytestringGet;
use sdk::serialize;
use sdk::storage::{Keypath, KeypathSeed, Load, Map, NNInt};
use sdk::types::ContractAddress;
use serde::de::{DeserializeSeed, SeqAccess, Visitor};
use serde::Deserializer;

struct _KeypathSeed<T> {
    keypath: Keypath,
    marker: core::marker::PhantomData<T>,
}

impl<T> _KeypathSeed<T> {
    pub fn new(keypath: &Keypath) -> Self {
        Self {
            keypath: keypath.clone(),
            marker: core::marker::PhantomData,
        }
    }
}

impl<'de> DeserializeSeed<'de> for _KeypathSeed<CoinContract> {
    type Value = CoinContract;

    fn deserialize<D>(self, deserializer: D) -> Result<Self::Value, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct KeypathVisitor<'a> {
            keypath: &'a mut Keypath,
        }

        impl<'de> Visitor<'de> for KeypathVisitor<'_> {
            type Value = CoinContract;

            fn expecting(
                &self,
                formatter: &mut fmt::Formatter,
            ) -> fmt::Result {
                write!(formatter, "a CoinContract")
            }

            fn visit_seq<A>(self, mut seq: A) -> Result<CoinContract, A::Error>
            where
                A: SeqAccess<'de>,
            {
                self.keypath.enter_seq();
                let contract_owner = seq.next_element()?.unwrap();

                self.keypath.seq_next();
                let balances = {
                    let seed =
                        KeypathSeed::<Map<ContractAddress, Balance>>::new(
                            self.keypath,
                        );
                    seq.next_element_seed(seed)?.unwrap()
                };

                self.keypath.seq_next();
                let allowances = {
                    let seed = KeypathSeed::<
                        Map<ContractAddress, Map<ContractAddress, Allowance>>,
                    >::new(self.keypath);
                    seq.next_element_seed(seed)?.unwrap()
                };

                self.keypath.seq_next();
                let total_supply = {
                    let seed = KeypathSeed::<NNInt>::new(self.keypath);
                    seq.next_element_seed(seed)?.unwrap()
                };

                self.keypath.leave_seq();

                Ok(CoinContract {
                    contract_owner,
                    balances,
                    allowances,
                    total_supply,
                })
            }
        }

        let mut keypath = self.keypath.clone();
        let visitor = KeypathVisitor {
            keypath: &mut keypath,
        };
        deserializer.deserialize_tuple(4, visitor)
    }
}

impl Load for CoinContract {
    fn load_keypath(keypath: &Keypath) -> super::Result<Self> {
        let key = keypath.generate_key();
        let buf = BytestringGet(&key)?;

        let seed = _KeypathSeed::<CoinContract>::new(keypath);
        let value = serialize::from_bytes_seed(seed, &buf)?;

        Ok(value)
    }
}

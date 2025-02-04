//! Storage for a coin contract

use sdk::types::{ContractAddress, StorageKey};
use sdk::result::{Result, assert};
use sdk::io::{Read, Write, Serialize, Deserialize, Pair};
use sdk::storage::{NNInt, Map, FromKey, item_key};

/// SDK item
pub fn sender() -> Result<ContractAddress> {
    sdk::GetMsgSender().map_err(From::from)
}

pub type Allowance = NNInt;
pub type Balance = NNInt;

// #[derive(Serialize, Deserialize)]
#[derive(Debug, PartialEq)]
pub struct CoinStorage {
    pub owner: ContractAddress,
    // pub name: ByteArray,
    // pub symbol: ByteArray,
    pub total_supply: NNInt,
    pub balances: Map<ContractAddress, Balance>,
    pub allowances: Map<Pair<ContractAddress, ContractAddress>, Allowance>,
}

// TODO: auto-derive this
impl Serialize for CoinStorage {
    fn serialize<W: Write>(&self, writer: &mut W) -> Result<()> {
        self.owner.serialize(writer)?;
        self.total_supply.serialize(writer)?;
        self.balances.serialize(writer)?;
        self.allowances.serialize(writer)?;
        Ok(())
    }
}

// TODO: auto-derive this
impl Deserialize for CoinStorage {
    fn deserialize<R: Read>(reader: &mut R) -> Result<Self> {
        let owner = Deserialize::deserialize(reader)?;
        let total_supply = Deserialize::deserialize(reader)?;
        let balances = Deserialize::deserialize(reader)?;
        let allowances = Deserialize::deserialize(reader)?;
        Ok(CoinStorage {
            owner,
            total_supply,
            balances,
            allowances,
        })
    }
}

impl CoinStorage {
    pub fn new(storage_key: &StorageKey, owner: &ContractAddress) -> Result<Self> {
        let key = storage_key.clone();
        let owner = owner.clone();
        let total_supply = NNInt::from_key(item_key(&key, b"1"));
        let balances = Map::from_key(item_key(&key, b"2"));
        let allowances = Map::from_key(item_key(&key, b"3"));
        let storage = CoinStorage {
            owner,
            total_supply,
            balances,
            allowances,
        };

        Ok(storage)
    }

    pub fn balance_of(&self, addr: &ContractAddress) -> Result<Balance> {
        self.balances.get(addr)
    }

    pub fn allowance_for(&self, owner: &ContractAddress, spender: &ContractAddress)
        -> Result<Allowance>
    {
        let key = Pair(*owner, *spender); // XX
        self.allowances.get(&key)
    }

    pub fn assert_sender_is_owner(&self) -> Result<()> {
        let sender = sender()?;
        assert(sender == self.owner, "sender is owner")
    }
}

#[test]
fn serialize_storage() {
    // Make a storage structure
    let storage_key = StorageKey::default();
    let owner = StorageKey::default();
    let storage = CoinStorage::new(&storage_key, &owner).unwrap();

    // Write it to a buffer
    let mut cur = sdk::io::Cursor::new([0; 1000]);
    storage.serialize(&mut cur).unwrap();

    // Read it back from the buffer
    let buf = cur.into_inner();
    let mut cur = sdk::io::Cursor::new(buf.as_slice());
    let storage2 = CoinStorage::deserialize(&mut cur).unwrap();

    // Compare
    assert_eq!(storage, storage2);
}

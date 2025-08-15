// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

namespace hogd {

typedef opaque SCPHash[32];
typedef PublicKey SCPNodeID;

typedef unsigned int SCPCount;
typedef unsigned hyper SCPSlotIndex;

typedef opaque SCPValue<>;

struct SCPBallot {
  SCPCount counter; // n
  SCPValue value;   // x
};

enum SCPStatementType {
  SCP_ST_PREPARE = 0,
  SCP_ST_CONFIRM = 1,
  SCP_ST_EXTERNALIZE = 2,
  SCP_ST_NOMINATE = 3
};

struct SCPNomination {
  SCPHash quorumSetHash; // D
  SCPValue votes<>;      // X
  SCPValue accepted<>;   // Y
};

struct SCPStatement {
  SCPNodeID nodeID;       // v
  SCPSlotIndex slotIndex; // i

  union switch (SCPStatementType type)
  {
  case SCP_ST_PREPARE:
    struct {
      SCPHash quorumSetHash;    // D
      SCPBallot ballot;         // b
      SCPBallot *prepared;      // p
      SCPBallot *preparedPrime; // p'
      SCPCount nC;              // c.n
      SCPCount nH;              // h.n
    } prepare;
  case SCP_ST_CONFIRM:
    struct {
      SCPBallot ballot;      // b
      SCPCount nPrepared;    // p.n
      SCPCount nCommit;      // c.n
      SCPCount nH;           // h.n
      SCPHash quorumSetHash; // D
    } confirm;
  case SCP_ST_EXTERNALIZE:
    struct {
      SCPBallot commit;            // c
      SCPCount nH;                 // h.n
      SCPHash commitQuorumSetHash; // D used before EXTERNALIZE
    } externalize;
  case SCP_ST_NOMINATE:
    SCPNomination nominate;
  }
  pledges;
};

struct SCPEnvelope {
  SCPStatement statement;
  Signature signature;
};

// supports things like: A,B,C,(D,E,F),(G,H,(I,J,K,L))
// only allows 2 levels of nesting
struct SCPQuorumSet {
  SCPCount threshold;
  SCPNodeID validators<>;
  SCPQuorumSet innerSets<>;
};

} // namespace hogd

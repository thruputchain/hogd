#include <cassert>

#include "tests/compiler.hh"
#include "x23/serialize.h"

using namespace x23;

int
main()
{
  Reply reply(Status::OK);
  reply.record() = Record{.id = -42,
                          .count = 123,
                          .signed_big = -0x100000000LL,
                          .big = 0x100000000ULL,
                          .enabled = true,
                          .name = "example",
                          .payload = {0, 1, 255},
                          .digest = {1, 2, 3},
                          .status = Status::BUSY,
                          .fixed = {-1, 2},
                          .values = {3, 4, 5},
                          .next = Ptr(Record{})};
  reply.record().next->id = 99;
  Reply decoded;
  deserialize(&decoded, serialize(reply));
  assert(decoded == reply);
  assert(decoded.record().next.get() != reply.record().next.get());

  for (auto status : {Status::BUSY, Status::BAD}) {
    reply.status(status);
    reply.message() = "try again";
    deserialize(&decoded, serialize(reply));
    assert(decoded == reply);
    assert(decoded.status() == status);
  }

  reply.status(Status(42));
  auto bytes = serialize(reply);
  assert(bytes.size() == 4);
  deserialize(&decoded, bytes);
  assert(decoded == reply);

  // Unions named after the generator's own template parameters.
  U u;
  u.big(true);
  u.h() = std::int64_t(1) << 33;
  U u_decoded;
  deserialize(&u_decoded, serialize(u));
  assert(u_decoded == u);
  assert(get<2>(u_decoded) == u.h());

  I i;
  i.big(false);
  i.i() = 23;
  I i_decoded;
  deserialize(&i_decoded, serialize(i));
  assert(i_decoded == i);
  assert(get<1>(i_decoded) == 23);
}

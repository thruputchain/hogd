#include <array>
#include <cassert>
#include <format>
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "model_checker/async.h"
#include "model_checker/threadpool.h"
#include "model_checker/work_queue.h"
#include "tests/xdrtest.hh"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

constexpr uint32_t
padded_size(uint32_t N)
{
  return (N + 3) & ~3;
}

} // namespace

template<typename T> struct DeserializeBranch;

template<> struct DeserializeBranch<uint32_t> {

  static x23::Buf branch(model::RunnableActionSet &set, bool & /*unused*/)
  {

    auto choice = set.choice(3);
    switch (choice) {
    case 0:
      return {0, 0, 0, 0};
    case 1:
      return {0, 0, 0, 32};
    case 2:
      return {255, 255, 255, 255};
    default:
      panic("unreachable");
    }
  }
};

template<size_t MS> struct DeserializeBranch<x23::Vec<uint8_t, MS>> {

  static x23::Buf branch(model::RunnableActionSet &set, bool &exists_failure)
  {

    uint8_t is_good = set.choice(2);

    if (is_good == 1) {
      std::vector<uint32_t> possible_good_sizes = {0, 4, 8};

      auto choice = set.choice(possible_good_sizes.size());
      auto buf = x23::serialize(uint32_t{possible_good_sizes[choice]});

      buf.append_range(std::vector<uint8_t>(possible_good_sizes[choice], 0xBA));

      return buf;
    }
    assert(is_good == 0);
    exists_failure = true;

    std::vector<uint32_t> possible_bad_sizes = {2, 101};
    if (MS < std::numeric_limits<uint32_t>::max()) {
      possible_bad_sizes.push_back(
          padded_size(MS + 1)); // round up to the next multiple of 4
    }

    auto choice = set.choice(possible_bad_sizes.size());
    auto buf = x23::serialize(uint32_t{possible_bad_sizes[choice]});

    buf.append_range(
        std::vector<uint8_t>(padded_size(possible_bad_sizes[choice]),
                             0xAB)); // padded_size(N) bytes of garbage

    return buf;
  }
};

template<x23::is_xdr_type T, size_t MS>
struct DeserializeBranch<x23::Vec<T, MS>> {

  static x23::Buf branch(model::RunnableActionSet &set, bool &exists_failure)
  {

    uint8_t is_good = set.choice(2);

    if (is_good == 1) {
      std::vector<uint32_t> possible_good_sizes = {0, 1};

      auto choice = set.choice(possible_good_sizes.size());
      auto buf = x23::serialize(uint32_t{possible_good_sizes[choice]});

      for (uint32_t i = 0; i < possible_good_sizes[choice]; i++) {
        buf.append_range(DeserializeBranch<T>::branch(set, exists_failure));
      }

      return buf;
    }
    assert(is_good == 0);
    exists_failure = true;

    std::vector<uint32_t> possible_bad_sizes = {0, 100};
    if (MS != std::numeric_limits<uint32_t>::max()) {
      possible_bad_sizes.push_back(padded_size(MS + 1));
    }

    auto choice = set.choice(possible_bad_sizes.size());
    auto buf = x23::serialize(uint32_t{possible_bad_sizes[choice]});
    buf.append_range(std::vector<uint8_t>(4, 0xEF)); // 4 bytes of garbage

    return buf;
  }
};

template<size_t N> struct DeserializeBranch<std::array<uint8_t, N>> {
  constexpr static size_t kPaddedSize = (N + 3) & ~3;

  static x23::Buf branch(model::RunnableActionSet &set, bool &exists_failure)
  {

    auto is_good = set.choice(2);
    if (is_good == 1) {
      return x23::Buf(kPaddedSize, 0xFF);
    }
    assert(is_good == 0);
    exists_failure = true;

    if (N % 4 == 0) {
      return x23::Buf(N + 4, 0xFF);
    }

    return x23::Buf(N, 0xFF);
  }
};

template<x23::is_xdr_enum T> struct DeserializeBranch<T> {
  static x23::Buf branch(model::RunnableActionSet &set, bool &exists_failure)
  {
    // Enums serialize as uint32_t on the wire
    return DeserializeBranch<uint32_t>::branch(set, exists_failure);
  }
};

template<x23::is_xdr_struct T> struct DeserializeBranch<T> {

  static x23::Buf branch(model::RunnableActionSet &set, bool &exists_failure)
  {
    x23::Buf out;
    [&]<size_t... I>(std::index_sequence<I...>) {
      (out.append_range(DeserializeBranch<std::tuple_element_t<I, T>>::branch(
           set, exists_failure)),
       ...);
    }(std::make_index_sequence<std::tuple_size_v<T>>{});

    return out;
  }
};

// Required so the is_xdr_struct specialization above can recurse
// through a struct containing a union field — without this, the
// nested DeserializeBranch<UnionType> instantiation has no
// definition and the struct case fails to compile.  Emits only a
// valid encoding of the default-constructed value; the union's
// arms are not fuzz-explored at this position.
template<x23::is_xdr_union T> struct DeserializeBranch<T> {

  static x23::Buf branch(model::RunnableActionSet & /*set*/,
                         bool & /*exists_failure*/)
  {
    return x23::serialize(T{});
  }
};

namespace {
template<typename T>
void
run_deserialize_test(std::vector<uint8_t> const &choices = {})
{

  model::ThreadPool<bool> pool(4);
  std::shared_ptr<model::ExperimentBuilder<bool>> experiment =
      std::make_shared<model::ExperimentBuilder<bool>>(
          []() { return std::make_tuple(false); },
          [](model::WorkQueue &work_queue, bool &error) {
            auto actions =
                std::make_unique<model::RunnableActionSet>(work_queue);

            actions->add_action(
                [](model::RunnableActionSet &set, bool &error) -> model::Async {
                  co_await set.bg();
                  bool exists_failure = false;
                  bool found_failure = false;

                  auto data = DeserializeBranch<T>::branch(set, exists_failure);

                  T value;
                  try {
                    x23::deserialize(&value, x23::BufView(data));
                  } catch (const x23::XdrException &e) {
                    found_failure = true;
                    if (found_failure != exists_failure) {
                      warn("got unexpected error: {}", e.what());
                    }
                  }

                  if (exists_failure != found_failure) {
                    ADD_FAILURE() << "exists_failure != found_failure";
                    warn("bad example: deserialize {} (len = {}) found failure "
                         "{} expected failure {}",
                         encode_hex(std::as_bytes(std::span(data))),
                         data.size(), found_failure, exists_failure);
                    error = true;
                  }
                },
                error);

            return actions;
          },
          [](model::ActionResult res, bool &error) -> bool {
            return res == model::ActionResult::kOk && !error;
          });

  auto bad_path = pool.run(experiment, choices);
  EXPECT_FALSE(bad_path.has_value());
  if (bad_path.has_value()) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    ADD_FAILURE() << "Found bad path: " << model::show_path(bad_path.value());
  }
}

} // namespace

TEST(DeserializeTest, DeserializeUint32)
{
  run_deserialize_test<uint32_t>();
}

TEST(DeserializeTest, DeserializeOpaqueVec)
{
  run_deserialize_test<x23::Vec<uint8_t, 201>>();
}

TEST(DeserializeTest, DeserializeTxEventLog)
{
  // Editing serialize.h confirms that this test would have triggered
  // the bug fixed in #621
  run_deserialize_test<::TxEventLog>();
}

TEST(DeserializeTest, DeserializeHogdTxEventLog)
{
  run_deserialize_test<hogd::TxEventLog>();
}

} // namespace hogd


#include <memory>
#include <print>
#include <random>

#include "x23/heap.h"

using namespace x23;

auto rng = std::mt19937{std::random_device{}()};

void
print_heap(const auto &heap)
{
  for (const auto &i : heap.view())
    std::print(" {}", i);
  std::println("");
}

Heap<int>
random_heap(size_t sz = 20)
{
  auto dist = std::uniform_int_distribution{0, int(sz)};
  Heap<int> h;
  for (size_t i = 0; i < sz; ++i)
    h.insert(dist(rng));
  h.check();
  return h;
}

void
delete_test(auto &h)
{
  auto rng = std::mt19937{std::random_device{}()};
  print_heap(h);
  while (!h.empty()) {
    size_t i = std::uniform_int_distribution{0, int(h.size()) - 1}(rng);
    std::println("  removing element {}, which is {}", i, h.index(i));
    h.remove_index(i);
    print_heap(h);
    h.check();
  }
}

void
index_test(size_t sz = 20)
{
  auto dist = std::uniform_int_distribution{0, int(sz)};
  Heap<int> h;
  std::vector<std::unique_ptr<Heap<int>::HeapIndex>> is;
  for (size_t i = 0; i < sz; ++i) {
    is.emplace_back(std::make_unique<Heap<int>::HeapIndex>());
    h.insert(dist(rng), is.back().get());
    h.check();
  }
  print_heap(h);

  auto print_indices = [&is] {
    std::print(" indices: ");
    for (const auto &ip : is)
      std::print(" {}", ip->get());
    std::println("");
  };
  print_indices();

  for (size_t i = 0; i < sz; ++i) {
    std::println("  removing index entry {}, index {}, value {}", i,
                 is[i]->get(), h[*is[i]]);
    h.remove(*is[i]);
    print_indices();
    assert(is[i]->get() == -1);
    print_heap(h);
  }
  assert(h.empty());
}

int
main()
{
  auto h = random_heap();
  print_heap(h);
  while (!h.empty()) {
    std::print(" {}", h.poptop());
    h.check();
  }
  std::println("");

  h = random_heap();
  delete_test(h);

  index_test();

  return 0;
}

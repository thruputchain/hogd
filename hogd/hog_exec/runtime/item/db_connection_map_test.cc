#include <gtest/gtest.h>

#include <map>
#include <set>

#include "hogd/hog_exec/runtime/item/db_connection_map.h"
#include "x23/cpumap.h"
#include "x23/vec.h"

namespace hogd {
namespace {

// cpu -> bucket for ncpu CPUs with nllc LLCs of contiguous ids.  When smt,
// cpu c and c + ncpu/2 are siblings, as Linux numbers them on EPYC.
x23::CpuMap
llc_of(int ncpu, int nllc, bool smt)
{
  const int ncore = smt ? ncpu / 2 : ncpu;
  x23::Vec<int> m;
  for (int cpu = 0; cpu < ncpu; cpu++)
    m.push_back((cpu % ncore) / (ncore / nllc));
  return x23::CpuMap{m};
}

x23::CpuMap
core_of(int ncpu, bool smt)
{
  const int ncore = smt ? ncpu / 2 : ncpu;
  x23::Vec<int> m;
  for (int cpu = 0; cpu < ncpu; cpu++)
    m.push_back(cpu % ncore);
  return x23::CpuMap{m};
}

x23::Vec<int>
range(int first, int n, int step = 1)
{
  x23::Vec<int> v;
  for (int i = 0; i < n; i++)
    v.push_back(first + (i * step));
  return v;
}

// Every bucket in [0, nbuckets) has an allowed CPU, and no bucket has
// allowed CPUs from two LLCs.
void
expect_llc_local(const x23::Vec<int> &buckets, const x23::Vec<int> &allowed,
                 int nbuckets, const x23::CpuMap &llc)
{
  std::map<int, std::set<int>> llcs_of_bucket;
  for (int cpu : allowed)
    llcs_of_bucket[buckets[cpu]].insert(llc.bucket(cpu));
  ASSERT_EQ(static_cast<int>(llcs_of_bucket.size()), nbuckets);
  for (auto &[bucket, llcs] : llcs_of_bucket) {
    EXPECT_GE(bucket, 0);
    EXPECT_LT(bucket, nbuckets);
    EXPECT_EQ(llcs.size(), 1U) << "bucket " << bucket;
  }
}

TEST(DbConnectionBuckets, EpycWithSmtKeepsConnectionsInsideOneLlc)
{
  // 64 cores, 128 threads, 8 LLCs, 32 connections: cpu 0 and cpu 32 are on
  // different LLCs and must not share.
  const auto llc = llc_of(128, 8, true);
  const auto core = core_of(128, true);
  const auto allowed = range(0, 128);
  const auto b = db_connection_buckets(allowed, 32, llc, core);
  expect_llc_local(b, allowed, 32, llc);
  EXPECT_NE(b[0], b[32]);
  // SMT siblings share.
  EXPECT_EQ(b[0], b[64]);
  EXPECT_EQ(b[37], b[101]);
  // Four connections per LLC.
  std::map<int, std::set<int>> per_llc;
  for (int cpu : allowed)
    per_llc[llc.bucket(cpu)].insert(b[cpu]);
  for (auto &[l, conns] : per_llc)
    EXPECT_EQ(conns.size(), 4U) << "llc " << l;
}

TEST(DbConnectionBuckets, NoSmt)
{
  const auto llc = llc_of(64, 8, false);
  const auto core = core_of(64, false);
  const auto allowed = range(0, 64);
  const auto b = db_connection_buckets(allowed, 32, llc, core);
  expect_llc_local(b, allowed, 32, llc);
  EXPECT_EQ(b[0], b[1]);
  EXPECT_NE(b[1], b[2]);
}

TEST(DbConnectionBuckets, PinnedToOneLlcGivesOneConnectionPerCpu)
{
  const auto llc = llc_of(64, 8, false);
  const auto core = core_of(64, false);
  const auto allowed = range(8, 8);
  const auto b = db_connection_buckets(allowed, 8, llc, core);
  expect_llc_local(b, allowed, 8, llc);
  for (int cpu = 0; cpu < 8; cpu++)
    EXPECT_EQ(b[cpu], 0); // outside the mask
}

TEST(DbConnectionBuckets, OneCpuPerLlc)
{
  const auto llc = llc_of(64, 8, false);
  const auto core = core_of(64, false);
  const auto allowed = range(0, 8, 8);
  const auto b = db_connection_buckets(allowed, 8, llc, core);
  expect_llc_local(b, allowed, 8, llc);
}

TEST(DbConnectionBuckets, FewerConnectionsThanLlcsKeepsEachLlcWhole)
{
  const auto llc = llc_of(64, 8, false);
  const auto core = core_of(64, false);
  const auto allowed = range(0, 64);
  const auto b = db_connection_buckets(allowed, 3, llc, core);
  std::set<int> used;
  for (int cpu : allowed) {
    used.insert(b[cpu]);
    EXPECT_EQ(b[cpu], b[(cpu / 8) * 8]) << "cpu " << cpu;
  }
  EXPECT_EQ(used.size(), 3U);
}

TEST(DbConnectionBuckets, UnevenLlcsLeaveNoBucketEmpty)
{
  // One CPU on LLC 0, the rest on LLC 1, more connections than LLCs.
  x23::Vec<int> lm{0, 1, 1, 1, 1, 1, 1, 1};
  const x23::CpuMap llc{lm};
  const auto core = core_of(8, false);
  const auto allowed = range(0, 8);
  for (int n = 2; n <= 8; n++) {
    const auto b = db_connection_buckets(allowed, n, llc, core);
    expect_llc_local(b, allowed, n, llc);
  }
}

TEST(DbConnectionBuckets, SingleBucketLlcMap)
{
  const x23::CpuMap llc{x23::Vec<int>{0}};
  const auto core = core_of(16, false);
  const auto allowed = range(0, 16);
  const auto b = db_connection_buckets(allowed, 8, llc, core);
  std::set<int> used(b.begin(), b.end());
  EXPECT_EQ(used.size(), 8U);
}

} // namespace
} // namespace hogd

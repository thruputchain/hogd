// -*- C++ -*-

#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <exception>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <x23/async.h>
#include <x23/threadpool.h>

namespace x23 {

// Terminal failure state for a DAG run. A failure stops scheduling new nodes,
// but nodes that are already running are allowed to finish.
enum class DagFailureKind {
  None,
  Cancelled,
  Exception,
};

// Captures the first graph-level failure. For exceptions, exception contains
// the original exception_ptr from the failing node.
struct DagFailure {
  DagFailureKind kind{DagFailureKind::None};
  std::string node;
  std::exception_ptr exception;
};

// Timing and lifecycle information for one node. queued_at is set for every
// node when run() starts; started_at and finished_at are meaningful only when
// the corresponding flags are true.
struct DagNodeStats {
  std::string name;
  std::chrono::steady_clock::time_point queued_at;
  std::chrono::steady_clock::time_point started_at;
  std::chrono::steady_clock::time_point finished_at;
  bool started{false};
  bool finished{false};
};

// Opaque dependency list built by Dag::after(...).
struct DagDeps {
  std::vector<size_t> ids;
};

class Dag;
class DagCleanup;

template<typename T> class DagNode {
public:
  DagNode() = default;

  // Return the stored result. The node must have completed successfully.
  const T &get() const;
  T &get();
  // True once the node has completed successfully and its result is available.
  bool ready() const;
  // Stable insertion-order id used internally for dependency wiring.
  size_t id() const { return id_; }

private:
  friend class Dag;
  DagNode(Dag *dag, size_t id) : dag_(dag), id_(id) {}

  // Owning graph and insertion-order id. DagNode is a lightweight handle.
  Dag *dag_{nullptr};
  size_t id_{0};
};

template<> class DagNode<void> {
public:
  DagNode() = default;

  // True once the void node has completed successfully.
  bool ready() const;
  // Stable insertion-order id used internally for dependency wiring.
  size_t id() const { return id_; }

private:
  friend class Dag;
  DagNode(Dag *dag, size_t id) : dag_(dag), id_(id) {}

  // Owning graph and insertion-order id. DagNode is a lightweight handle.
  Dag *dag_{nullptr};
  size_t id_{0};
};

class DagCleanup {
public:
  DagCleanup() = default;

  void arm() const;
  void disarm() const;
  bool armed() const;

private:
  friend class Dag;
  DagCleanup(Dag *dag, size_t id) : dag_(dag), id_(id) {}

  Dag *dag_{nullptr};
  size_t id_{0};
};

// A small one-shot dependency graph for mixing CPU work and coroutine work.
//
// Example:
//
// ThreadPool pool;
// Dag dag("payments", pool);
//
// auto account = dag.cpu<Account>("load-account", [] {
//  return load_account_from_disk();
// });
//
// auto balance = dag.async<uint64_t>(
//   "fetch-balance", dag.after(account), [&]() -> Async<uint64_t> {
//    co_return co_await fetch_balance(account.get().id);
//   });
//
// auto receipt = dag.cpu<Receipt>("build-receipt", dag.after(account, balance),
//                [&] {
//                 return build_receipt(account.get(),
//                            balance.get());
//                });
//
// dag.run()();
// if (!dag.ok()) {
//  DagFailure failure = dag.failure();
//  // failure.node names the first failed node. If a node threw an exception,
//  // failure.kind is DagFailureKind::Exception and failure.exception has the
//  // exception.
// }
// else {
//  use_receipt(receipt.get());
// }
//
// Nodes are added before run() and the graph can only be run once. A node
// starts after all dependencies passed to after(...) have completed
// successfully. If any node throws or calls fail(...), no additional nodes are
// scheduled.
class Dag {
public:
  // name is retained for diagnostics; pool is used for cpu(...) nodes.
  explicit Dag(std::string_view name, ThreadPool &pool)
    : name_(name), pool_(pool)
  {}

  Dag(const Dag &) = delete;
  Dag &operator=(const Dag &) = delete;

  // Add a failure cleanup action. Cleanup actions run only if the graph fails,
  // after all normal nodes that were already running have drained. Callers arm
  // a cleanup after acquiring a resource and disarm it after the resource is
  // released or ownership is transferred.
  template<typename Fn> DagCleanup cleanup(std::string_view name, Fn &&fn)
  {
    std::lock_guard lock(mutex_);
    x23assert(!started_);

    const size_t id = cleanups_.size();
    auto cleanup = std::make_unique<CleanupBase>();
    cleanup->name = name;
    cleanup->run = make_cleanup_runner(std::forward<Fn>(fn));
    cleanups_.push_back(std::move(cleanup));
    return DagCleanup(this, id);
  }

  // Add a CPU-bound root node. The callable runs on pool_.bg() and either
  // returns T or, for T=void, returns nothing.
  template<typename T, typename Fn>
  DagNode<T> cpu(std::string_view name, Fn &&fn)
  {
    return cpu<T>(name, DagDeps{}, std::forward<Fn>(fn));
  }

  // Add a CPU-bound node with dependencies.
  template<typename T, typename Fn>
  DagNode<T> cpu(std::string_view name, DagDeps deps, Fn &&fn)
  {
    return add_node<T>(name, std::move(deps),
                       make_cpu_runner<T>(std::forward<Fn>(fn)));
  }

  // Add an async root node. The callable must return Async<T> or Async<void>.
  template<typename T, typename Fn>
  DagNode<T> async(std::string_view name, Fn &&fn)
  {
    return async<T>(name, DagDeps{}, std::forward<Fn>(fn));
  }

  // Add an async node with dependencies.
  template<typename T, typename Fn>
  DagNode<T> async(std::string_view name, DagDeps deps, Fn &&fn)
  {
    return add_node<T>(name, std::move(deps),
                       make_async_runner<T>(std::forward<Fn>(fn)));
  }

  // Build a dependency list from previously-added nodes.
  template<typename... Nodes> DagDeps after(const Nodes &...nodes)
  {
    DagDeps deps;
    deps.ids.reserve(sizeof...(nodes));
    (deps.ids.push_back(nodes.id()), ...);
    return deps;
  }

  // Start the graph and wait for completion. Completion means all runnable
  // nodes finished, or a failure occurred and all already-running nodes
  // drained. Exceptions from nodes are recorded in failure() rather than
  // rethrown.
  Async<void> run()
  {
    auto done_pair = make_async<void>();
    Async<void> done = std::move(done_pair.first);
    CoroCB<void> cb = done_pair.second;
    std::vector<size_t> to_start;
    std::vector<size_t> cleanup_ids;
    CoroCB<void> done_cb;
    {
      std::lock_guard lock(mutex_);
      x23assert(!started_);
      started_ = true;
      done_cb_ = cb;
      graph_started_at_ = std::chrono::steady_clock::now();
      for (size_t i = 0; i < nodes_.size(); i++)
        nodes_[i]->queued_at = graph_started_at_;
      mark_ready_roots_locked(to_start);
      done_cb = finish_cb_locked(cleanup_ids);
    }
    launch_nodes(to_start);
    if (!cleanup_ids.empty())
      run_cleanups(std::move(cleanup_ids));
    else if (done_cb)
      done_cb();
    co_await done;
  }

  // Record an explicit graph failure. The first failure wins.
  void fail(DagFailureKind kind, std::string_view node = {},
            std::exception_ptr exception = nullptr)
  {
    std::lock_guard lock(mutex_);
    fail_locked(kind, node, exception);
  }

  // True if no node has thrown and fail(...) has not been called.
  bool ok() const
  {
    std::lock_guard lock(mutex_);
    return failure_.kind == DagFailureKind::None;
  }

  // Return the first recorded graph failure, or kind None if the run succeeded.
  DagFailure failure() const
  {
    std::lock_guard lock(mutex_);
    return failure_;
  }
  std::vector<DagFailure> cleanup_failures() const
  {
    std::lock_guard lock(mutex_);
    return cleanup_failures_;
  }
  // Return a snapshot of per-node lifecycle timing and state.
  std::vector<DagNodeStats> stats() const
  {
    std::lock_guard lock(mutex_);
    std::vector<DagNodeStats> ret;
    ret.reserve(nodes_.size());
    for (const std::unique_ptr<NodeBase> &node : nodes_) {
      ret.push_back(DagNodeStats{
          .name = node->name,
          .queued_at = node->queued_at,
          .started_at = node->started_at,
          .finished_at = node->finished_at,
          .started = node->state != NodeState::Pending,
          .finished = node->state == NodeState::Done ||
                      node->state == NodeState::Failed,
      });
    }
    return ret;
  }

  // Return a Graphviz DOT rendering of the graph structure. Before run(), this
  // contains only graph structure. Once run() has started, node labels also
  // include lifecycle state and timing.
  std::string dot() const
  {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    std::string ret;
    ret += "digraph ";
    ret += dot_id(name_);
    ret += " {\n";
    ret += "  rankdir=LR;\n";
    ret += "  node [shape=box];\n";
    ret += "  dag_start [label=\"start\"];\n";
    ret += std::format("  dag_end [label=\"{}\"];\n",
                       dot_escape(dot_end_label_locked(now)));
    for (size_t id = 0; id < nodes_.size(); id++) {
      ret += std::format("  n{} [label=\"{}\"];\n", id,
                         dot_escape(dot_node_label_locked(id, now)));
    }
    for (const size_t root : roots_)
      ret += std::format("  dag_start -> n{};\n", root);
    for (size_t id = 0; id < nodes_.size(); id++) {
      for (const size_t dep : nodes_[id]->deps)
        ret += std::format("  n{} -> n{};\n", dep, id);
      if (nodes_[id]->successors.empty())
        ret += std::format("  n{} -> dag_end;\n", id);
    }
    if (nodes_.empty())
      ret += "  dag_start -> dag_end;\n";
    ret += "}\n";
    return ret;
  }

private:
  static std::string format_duration(std::chrono::steady_clock::duration d)
  {
    using namespace std::chrono;
    const auto us = duration_cast<microseconds>(d).count();
    if (us < 1'000)
      return std::format("{}us", us);

    const double ms = duration<double, std::milli>(d).count();
    if (ms < 1'000)
      return std::format("{:.3f}ms", ms);

    return std::format("{:.3f}s", duration<double>(d).count());
  }

  static std::string dot_escape(std::string_view s)
  {
    std::string ret;
    ret.reserve(s.size());
    for (const char c : s) {
      switch (c) {
      case '\\':
        ret += "\\\\";
        break;
      case '"':
        ret += "\\\"";
        break;
      case '\n':
        ret += "\\n";
        break;
      case '\r':
        ret += "\\r";
        break;
      case '\t':
        ret += "\\t";
        break;
      default:
        ret += c;
        break;
      }
    }
    return ret;
  }

  static std::string dot_id(std::string_view s)
  {
    std::string ret;
    ret.reserve(s.size());
    for (const char c : s) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_') {
        ret += c;
      }
      else {
        ret += '_';
      }
    }
    if (ret.empty() || (ret[0] >= '0' && ret[0] <= '9'))
      ret.insert(ret.begin(), '_');
    return ret;
  }

  // Internal lifecycle state for one node.
  enum class NodeState {
    Pending,
    Running,
    Done,
    Failed,
  };

  static std::string_view state_name(NodeState state)
  {
    switch (state) {
    case NodeState::Pending:
      return "pending";
    case NodeState::Running:
      return "running";
    case NodeState::Done:
      return "done";
    case NodeState::Failed:
      return "failed";
    }
    return "unknown";
  }

  // Type-erased storage base for typed node results.
  struct NodeStorageBase {
    virtual ~NodeStorageBase() = default;
  };

  // Result storage for non-void nodes. The value is populated only on success.
  template<typename T> struct NodeStorage : NodeStorageBase {
    std::optional<T> value;
  };

  // Marker storage for void nodes.
  struct VoidNodeStorage : NodeStorageBase {};

  // Internal graph node: dependency edges, runnable body, result storage, and
  // lifecycle timestamps.
  struct NodeBase {
    std::string name;
    std::vector<size_t> deps;
    std::vector<size_t> successors;
    std::function<Async<void>(size_t)> run;
    std::unique_ptr<NodeStorageBase> storage;
    NodeState state{NodeState::Pending};
    std::chrono::steady_clock::time_point queued_at;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point finished_at;
  };

  struct CleanupBase {
    std::string name;
    std::function<Async<void>()> run;
    bool armed{false};
    size_t arm_order{0};
  };

  // Wrap a synchronous callable as an Async<void> runner that first switches to
  // the thread pool and then stores the result.
  template<typename T, typename Fn>
  std::function<Async<void>(size_t)> make_cpu_runner(Fn &&fn)
  {
    return [this, fn = std::decay_t<Fn>(std::forward<Fn>(fn))](
               size_t id) mutable -> Async<void> {
      co_await pool_.bg();
      if constexpr (std::is_void_v<T>) {
        std::invoke(fn);
      }
      else {
        storage<T>(id).value.emplace(std::invoke(fn));
      }
      co_return;
    };
  }

  // Wrap an Async-returning callable as the common node runner type and store
  // the awaited result for non-void nodes.
  template<typename T, typename Fn>
  std::function<Async<void>(size_t)> make_async_runner(Fn &&fn)
  {
    if constexpr (std::is_void_v<T>) {
      return [fn = std::decay_t<Fn>(std::forward<Fn>(fn))](
                 size_t) mutable -> Async<void> {
        co_await std::invoke(fn);
        co_return;
      };
    }
    else {
      return [this, fn = std::decay_t<Fn>(std::forward<Fn>(fn))](
                 size_t id) mutable -> Async<void> {
        storage<T>(id).value.emplace(co_await std::invoke(fn));
        co_return;
      };
    }
  }

  template<typename Fn> std::function<Async<void>()> make_cleanup_runner(Fn &&fn)
  {
    return [fn = std::decay_t<Fn>(std::forward<Fn>(fn))]() mutable
               -> Async<void> {
      co_await std::invoke(fn);
      co_return;
    };
  }

  // Register a node before the graph starts and wire predecessor/successor
  // edges. Dependencies must refer to earlier nodes.
  template<typename T>
  DagNode<T> add_node(std::string_view name, DagDeps deps,
                      std::function<Async<void>(size_t)> runner)
  {
    std::lock_guard lock(mutex_);
    x23assert(!started_);

    const size_t id = nodes_.size();
    auto node = std::make_unique<NodeBase>();
    node->name = name;
    node->deps = std::move(deps.ids);
    node->run = std::move(runner);
    if constexpr (std::is_void_v<T>)
      node->storage = std::make_unique<VoidNodeStorage>();
    else
      node->storage = std::make_unique<NodeStorage<T>>();
    nodes_.push_back(std::move(node));
    if (nodes_[id]->deps.empty())
      roots_.push_back(id);
    for (const size_t dep : nodes_[id]->deps) {
      x23assert(dep < id);
      nodes_[dep]->successors.push_back(id);
    }
    return DagNode<T>(this, id);
  }

  void arm_cleanup(size_t id)
  {
    std::lock_guard lock(mutex_);
    x23assert(id < cleanups_.size());
    x23assert(!cleanup_started_);

    CleanupBase &cleanup = *cleanups_[id];
    if (cleanup.armed)
      return;
    cleanup.armed = true;
    cleanup.arm_order = next_cleanup_arm_order_++;
  }

  void disarm_cleanup(size_t id)
  {
    std::lock_guard lock(mutex_);
    x23assert(id < cleanups_.size());
    x23assert(!cleanup_started_);
    cleanups_[id]->armed = false;
  }

  bool cleanup_armed(size_t id) const
  {
    std::lock_guard lock(mutex_);
    x23assert(id < cleanups_.size());
    return cleanups_[id]->armed;
  }

  // Return typed storage for a node. The caller is responsible for requesting
  // the same T that was used when the node was added.
  template<typename T> NodeStorage<T> &storage(size_t id) const
  {
    return static_cast<NodeStorage<T> &>(*nodes_[id]->storage);
  }

  std::vector<size_t> armed_cleanups_locked()
  {
    std::vector<size_t> ret;
    ret.reserve(cleanups_.size());
    for (size_t i = 0; i < cleanups_.size(); i++) {
      if (cleanups_[i]->armed)
        ret.push_back(i);
    }
    std::sort(ret.begin(), ret.end(), [&](size_t a, size_t b) {
      return cleanups_[a]->arm_order > cleanups_[b]->arm_order;
    });
    return ret;
  }

  Detached run_cleanups(std::vector<size_t> cleanup_ids)
  {
    for (const size_t cleanup_id : cleanup_ids) {
      try {
        co_await cleanups_[cleanup_id]->run();
      } catch (...) {
        std::lock_guard lock(mutex_);
        cleanup_failures_.push_back(DagFailure{
            .kind = DagFailureKind::Exception,
            .node = cleanups_[cleanup_id]->name,
            .exception = std::current_exception(),
        });
      }
    }

    CoroCB<void> done_cb;
    {
      std::lock_guard lock(mutex_);
      cleanup_finished_ = true;
      graph_finished_at_ = std::chrono::steady_clock::now();
      done_cb = done_cb_;
    }
    if (done_cb)
      done_cb();
  }

  std::string
  dot_node_label_locked(size_t id,
                        std::chrono::steady_clock::time_point now) const
  {
    const NodeBase &node = *nodes_[id];
    std::string label = std::format("{}: {}", id, node.name);
    if (!started_)
      return label;

    label += std::format("\nstate: {}", state_name(node.state));
    switch (node.state) {
    case NodeState::Pending:
      if (finished_)
        label += "\nwait: not started";
      else
        label += std::format("\nwait: {} so far",
                             format_duration(now - node.queued_at));
      break;
    case NodeState::Running:
      label += std::format("\nwait: {}",
                           format_duration(node.started_at - node.queued_at));
      label += std::format("\nrun: {} so far",
                           format_duration(now - node.started_at));
      label += std::format("\ntotal: {} so far",
                           format_duration(now - node.queued_at));
      break;
    case NodeState::Done:
    case NodeState::Failed:
      label += std::format("\nwait: {}",
                           format_duration(node.started_at - node.queued_at));
      label += std::format("\nrun: {}",
                           format_duration(node.finished_at - node.started_at));
      label += std::format("\ntotal: {}",
                           format_duration(node.finished_at - node.queued_at));
      break;
    }
    return label;
  }

  std::string
  dot_end_label_locked(std::chrono::steady_clock::time_point now) const
  {
    std::string label = "end";
    if (!started_)
      return label;

    label += "\ntime: ";
    if (finished_)
      label += format_duration(graph_finished_at_ - graph_started_at_);
    else
      label +=
          std::format("{} so far", format_duration(now - graph_started_at_));
    return label;
  }

  // A node is ready to start when it is still pending and all dependencies
  // completed successfully.
  bool ready_locked(size_t id) const
  {
    const NodeBase &node = *nodes_[id];
    if (node.state != NodeState::Pending)
      return false;
    for (const size_t dep : node.deps)
      if (nodes_[dep]->state != NodeState::Done)
        return false;
    return true;
  }

  // Queue all root nodes that are ready at graph start.
  void mark_ready_roots_locked(std::vector<size_t> &to_start)
  {
    if (failure_.kind != DagFailureKind::None)
      return;

    for (const size_t id : roots_) {
      if (!ready_locked(id))
        continue;
      mark_node_running_locked(id);
      to_start.push_back(id);
    }
  }

  // Queue successors of a completed node whose dependencies are now satisfied.
  // Once the graph has failed, no new successors are scheduled.
  void mark_ready_successors_locked(size_t id, std::vector<size_t> &to_start)
  {
    if (failure_.kind != DagFailureKind::None)
      return;

    for (const size_t successor : nodes_[id]->successors) {
      if (!ready_locked(successor))
        continue;
      mark_node_running_locked(successor);
      to_start.push_back(successor);
    }
  }

  // Transition a pending node to running and update the active-node count.
  void mark_node_running_locked(size_t id)
  {
    NodeBase &node = *nodes_[id];
    node.state = NodeState::Running;
    node.started_at = std::chrono::steady_clock::now();
    running_++;
  }

  // Launch a batch of nodes outside the mutex.
  void launch_nodes(const std::vector<size_t> &ids)
  {
    for (const size_t id : ids)
      launch_node(id);
  }

  // Start one node coroutine.
  void launch_node(size_t id) { run_node(id); }

  // Run one node, convert any exception into a graph failure, and then finish
  // the node.
  Detached run_node(size_t id)
  {
    bool failed = false;
    try {
      co_await nodes_[id]->run(id);
    } catch (...) {
      failed = true;
      std::lock_guard lock(mutex_);
      fail_locked(DagFailureKind::Exception, nodes_[id]->name,
                  std::current_exception());
    }
    finish_node(id, failed);
  }

  // Mark a node done or failed, schedule newly-ready successors, and complete
  // run() if this was the last running node.
  void finish_node(size_t id, bool failed)
  {
    std::vector<size_t> to_start;
    std::vector<size_t> cleanup_ids;
    CoroCB<void> done_cb;
    {
      std::lock_guard lock(mutex_);
      NodeBase &node = *nodes_[id];
      x23assert(node.state == NodeState::Running);
      node.state = failed ? NodeState::Failed : NodeState::Done;
      node.finished_at = std::chrono::steady_clock::now();
      x23assert(running_ > 0);
      running_--;
      mark_ready_successors_locked(id, to_start);
      done_cb = finish_cb_locked(cleanup_ids);
    }
    launch_nodes(to_start);
    if (!cleanup_ids.empty())
      run_cleanups(std::move(cleanup_ids));
    else if (done_cb)
      done_cb();
  }

  // Record the first failure only. Later failures from already-running nodes
  // are ignored.
  void fail_locked(DagFailureKind kind, std::string_view node,
                   std::exception_ptr exception)
  {
    if (failure_.kind != DagFailureKind::None)
      return;
    failure_ = DagFailure{
        .kind = kind,
        .node = std::string(node),
        .exception = exception,
    };
  }

  // Return the run() completion callback if the graph is now terminal.
  CoroCB<void> finish_cb_locked(std::vector<size_t> &cleanup_ids)
  {
    if (!started_ || finished_ || running_ != 0)
      return {};

    if (failure_.kind == DagFailureKind::None) {
      for (size_t i = 0; i < nodes_.size(); i++)
        if (ready_locked(i))
          return {};
    }

    if (failure_.kind == DagFailureKind::None) {
      finished_ = true;
      graph_finished_at_ = std::chrono::steady_clock::now();
      return done_cb_;
    }

    cleanup_ids = armed_cleanups_locked();
    if (cleanup_ids.empty()) {
      finished_ = true;
      cleanup_started_ = true;
      cleanup_finished_ = true;
      graph_finished_at_ = std::chrono::steady_clock::now();
      return done_cb_;
    }

    finished_ = true;
    cleanup_started_ = true;
    return {};
  }

  // Diagnostic graph name.
  std::string name_;
  // Thread pool used by CPU nodes.
  ThreadPool &pool_;
  // Protects graph structure, node state, failure_, and run lifecycle fields.
  mutable std::mutex mutex_;
  // Nodes in insertion order. Node ids are indexes into this vector.
  std::vector<std::unique_ptr<NodeBase>> nodes_;
  std::vector<std::unique_ptr<CleanupBase>> cleanups_;
  // Node ids with no dependencies.
  std::vector<size_t> roots_;
  // First graph-level failure, if any.
  DagFailure failure_;
  std::vector<DagFailure> cleanup_failures_;
  // Callback used to resume run() when the graph reaches a terminal state.
  CoroCB<void> done_cb_;
  // Wall-clock lifecycle of the whole graph run.
  std::chrono::steady_clock::time_point graph_started_at_;
  std::chrono::steady_clock::time_point graph_finished_at_;
  // Count of currently-running nodes.
  size_t running_{0};
  size_t next_cleanup_arm_order_{0};
  // True after run() has been called.
  bool started_{false};
  // True after run() has completed or is ready to complete.
  bool finished_{false};
  bool cleanup_started_{false};
  bool cleanup_finished_{false};

  template<typename T> friend class DagNode;
  friend class DagNode<void>;
  friend class DagCleanup;
};

template<typename T>
const T &
DagNode<T>::get() const
{
  x23assert(ready());
  return dag_->storage<T>(id_).value.value();
}

template<typename T>
T &
DagNode<T>::get()
{
  x23assert(ready());
  return dag_->storage<T>(id_).value.value();
}

template<typename T>
bool
DagNode<T>::ready() const
{
  std::lock_guard lock(dag_->mutex_);
  return dag_->nodes_[id_]->state == Dag::NodeState::Done;
}

inline bool
DagNode<void>::ready() const
{
  std::lock_guard lock(dag_->mutex_);
  return dag_->nodes_[id_]->state == Dag::NodeState::Done;
}

inline void
DagCleanup::arm() const
{
  x23assert(dag_ != nullptr);
  dag_->arm_cleanup(id_);
}

inline void
DagCleanup::disarm() const
{
  x23assert(dag_ != nullptr);
  dag_->disarm_cleanup(id_);
}

inline bool
DagCleanup::armed() const
{
  x23assert(dag_ != nullptr);
  return dag_->cleanup_armed(id_);
}

} // namespace x23

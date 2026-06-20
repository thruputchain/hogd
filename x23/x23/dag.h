// -*- C++ -*-

#pragma once

#include <chrono>
#include <concepts>
#include <exception>
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

enum class DagFailureKind {
  None,
  Cancelled,
  Exception,
};

struct DagFailure {
  DagFailureKind kind{DagFailureKind::None};
  std::string node;
  std::exception_ptr exception;
};

struct DagNodeStats {
  std::string name;
  std::chrono::steady_clock::time_point queued_at;
  std::chrono::steady_clock::time_point started_at;
  std::chrono::steady_clock::time_point finished_at;
  bool started{false};
  bool finished{false};
};

struct DagDeps {
  std::vector<size_t> ids;
};

class Dag;

template<typename T> class DagNode {
public:
  DagNode() = default;

  const T &get() const;
  T &get();
  bool ready() const;
  size_t id() const { return id_; }

private:
  friend class Dag;
  DagNode(Dag *dag, size_t id) : dag_(dag), id_(id) {}

  Dag *dag_{nullptr};
  size_t id_{0};
};

template<> class DagNode<void> {
public:
  DagNode() = default;

  bool ready() const;
  size_t id() const { return id_; }

private:
  friend class Dag;
  DagNode(Dag *dag, size_t id) : dag_(dag), id_(id) {}

  Dag *dag_{nullptr};
  size_t id_{0};
};

class Dag {
public:
  explicit Dag(std::string_view name, ThreadPool &pool)
    : name_(name), pool_(pool)
  {}

  Dag(const Dag &) = delete;
  Dag &operator=(const Dag &) = delete;

  template<typename T, typename Fn>
  DagNode<T> cpu(std::string_view name, Fn &&fn)
  {
    return cpu<T>(name, DagDeps{}, std::forward<Fn>(fn));
  }

  template<typename T, typename Fn>
  DagNode<T> cpu(std::string_view name, DagDeps deps, Fn &&fn)
  {
    return add_node<T>(name, std::move(deps),
                       make_cpu_runner<T>(std::forward<Fn>(fn)));
  }

  template<typename T, typename Fn>
  DagNode<T> async(std::string_view name, Fn &&fn)
  {
    return async<T>(name, DagDeps{}, std::forward<Fn>(fn));
  }

  template<typename T, typename Fn>
  DagNode<T> async(std::string_view name, DagDeps deps, Fn &&fn)
  {
    return add_node<T>(name, std::move(deps),
                       make_async_runner<T>(std::forward<Fn>(fn)));
  }

  template<typename... Nodes> DagDeps after(const Nodes &...nodes)
  {
    DagDeps deps;
    deps.ids.reserve(sizeof...(nodes));
    (deps.ids.push_back(nodes.id()), ...);
    return deps;
  }

  Async<void> run()
  {
    auto done_pair = make_async<void>();
    Async<void> done = std::move(done_pair.first);
    CoroCB<void> cb = done_pair.second;
    std::vector<size_t> to_start;
    CoroCB<void> done_cb;
    {
      std::lock_guard lock(mutex_);
      x23assert(!started_);
      started_ = true;
      done_cb_ = cb;
      for (size_t i = 0; i < nodes_.size(); i++)
        nodes_[i]->queued_at = std::chrono::steady_clock::now();
      mark_ready_nodes_locked(to_start);
      done_cb = finish_cb_locked();
    }
    launch_nodes(to_start);
    if (done_cb)
      done_cb();
    co_await done;
  }

  void fail(DagFailureKind kind, std::string_view node = {},
            std::exception_ptr exception = nullptr)
  {
    std::lock_guard lock(mutex_);
    fail_locked(kind, node, exception);
  }

  bool ok() const
  {
    std::lock_guard lock(mutex_);
    return failure_.kind == DagFailureKind::None;
  }

  const DagFailure &failure() const { return failure_; }
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

private:
  enum class NodeState {
    Pending,
    Running,
    Done,
    Failed,
  };

  struct NodeStorageBase {
    virtual ~NodeStorageBase() = default;
  };

  template<typename T> struct NodeStorage : NodeStorageBase {
    std::optional<T> value;
  };

  struct VoidNodeStorage : NodeStorageBase {};

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

  template<typename T, typename Fn>
  std::function<Async<void>(size_t)> make_cpu_runner(Fn &&fn)
  {
    auto fnp = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
    return [this, fnp](size_t id) -> Async<void> {
      co_await pool_.bg();
      if constexpr (std::is_void_v<T>) {
        std::invoke(*fnp);
      }
      else {
        storage<T>(id).value.emplace(std::invoke(*fnp));
      }
      co_return;
    };
  }

  template<typename T, typename Fn>
  std::function<Async<void>(size_t)> make_async_runner(Fn &&fn)
  {
    auto fnp = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
    return [this, fnp](size_t id) -> Async<void> {
      if constexpr (std::is_void_v<T>) {
        co_await std::invoke(*fnp);
      }
      else {
        storage<T>(id).value.emplace(co_await std::invoke(*fnp));
      }
      co_return;
    };
  }

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
    for (const size_t dep : nodes_[id]->deps) {
      x23assert(dep < id);
      nodes_[dep]->successors.push_back(id);
    }
    return DagNode<T>(this, id);
  }

  template<typename T> NodeStorage<T> &storage(size_t id) const
  {
    return static_cast<NodeStorage<T> &>(*nodes_[id]->storage);
  }

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

  void mark_ready_nodes_locked(std::vector<size_t> &to_start)
  {
    if (failure_.kind != DagFailureKind::None)
      return;

    for (size_t i = 0; i < nodes_.size(); i++) {
      if (!ready_locked(i))
        continue;
      mark_node_running_locked(i);
      to_start.push_back(i);
    }
  }

  void mark_node_running_locked(size_t id)
  {
    NodeBase &node = *nodes_[id];
    node.state = NodeState::Running;
    node.started_at = std::chrono::steady_clock::now();
    running_++;
  }

  void launch_nodes(const std::vector<size_t> &ids)
  {
    for (const size_t id : ids)
      launch_node(id);
  }

  void launch_node(size_t id) { run_node(id); }

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

  void finish_node(size_t id, bool failed)
  {
    std::vector<size_t> to_start;
    CoroCB<void> done_cb;
    {
      std::lock_guard lock(mutex_);
      NodeBase &node = *nodes_[id];
      x23assert(node.state == NodeState::Running);
      node.state = failed ? NodeState::Failed : NodeState::Done;
      node.finished_at = std::chrono::steady_clock::now();
      x23assert(running_ > 0);
      running_--;
      mark_ready_nodes_locked(to_start);
      done_cb = finish_cb_locked();
    }
    launch_nodes(to_start);
    if (done_cb)
      done_cb();
  }

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

  CoroCB<void> finish_cb_locked()
  {
    if (!started_ || finished_ || running_ != 0)
      return {};

    if (failure_.kind == DagFailureKind::None) {
      for (size_t i = 0; i < nodes_.size(); i++)
        if (ready_locked(i))
          return {};
    }

    finished_ = true;
    return done_cb_;
  }

  std::string name_;
  ThreadPool &pool_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<NodeBase>> nodes_;
  DagFailure failure_;
  CoroCB<void> done_cb_;
  size_t running_{0};
  bool started_{false};
  bool finished_{false};

  template<typename T> friend class DagNode;
  friend class DagNode<void>;
};

template<typename T>
const T &
DagNode<T>::get() const
{
  return dag_->storage<T>(id_).value.value();
}

template<typename T>
T &
DagNode<T>::get()
{
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

} // namespace x23

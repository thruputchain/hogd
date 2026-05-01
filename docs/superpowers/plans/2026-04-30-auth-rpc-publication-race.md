# Auth RPC Adapter Publication Race Fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the data race where RPC handler threads read `auth_server_` before the main thread writes it, by splitting `RpcServer::start_server` into bind + accept phases.

**Architecture:** Add `bind_server` (returns port + `UniqueFd`) and `start_accepting` (takes `UniqueFd`, begins dispatching) to `RpcServer`. Refactor both `start_server` overloads to call the new pair. Restructure `auth_init()` to bind first, do the intro handshake, set backend pointers, then start accepting.

**Tech Stack:** C++23, x23 async/RPC framework, GTest

---

### Task 1: Add `bind_server` and `start_accepting` to `RpcServer`

**Files:**
- Modify: `hogd/base/rpc_server.h:63-98` (replace both `start_server` overloads)

- [ ] **Step 1: Add `bind_server(uint16_t)`, `bind_server(HostPort)`, and `start_accepting(UniqueFd)`**

Add three new public methods before the existing `start_server` methods. Then refactor both `start_server` overloads to call through.

Replace the entire public method block (lines 63–98) with:

```cpp
  std::pair<uint16_t, x23::UniqueFd> bind_server(uint16_t port)
  {
    CHECK(!bound_);
    bound_ = true;
    int backlog = 5 + cp_.pool_.nthreads_;
    auto lfd = x23::tcplisten(port, backlog);
    if (port == 0)
      port = x23::get_port(x23::local_addr(lfd));
    port_ = port;
    return {port_, std::move(lfd)};
  }

  std::pair<uint16_t, x23::UniqueFd> bind_server(x23::HostPort bind_addr)
  {
    CHECK(!bound_);
    bound_ = true;
    std::expected<sockaddr_storage, x23::GaiError> addr =
        bind_addr.to_addr(/*dns=*/false);
    CHECK(addr.has_value());
    int backlog = 5 + cp_.pool_.nthreads_;
    auto lfd = x23::tcplisten(*addr, backlog);
    port_ = x23::get_port(x23::local_addr(lfd));
    return {port_, std::move(lfd)};
  }

  void start_accepting(x23::UniqueFd lfd)
  {
    CHECK_EQ(listen_fd_.load(std::memory_order_relaxed), x23::kInvalidFd);
    listen_fd_ = lfd.get();
    x23::start_listening(cp_, lfd,
                         [self = x23::RefPtr(*this),
                          _d = x23::Defer([this] { eof(); })](auto c) {
                           c->max_message_size = self->max_message_size_;
                           new Conn{*self, c};
                         });
  }

  uint16_t start_server(uint16_t port)
  {
    auto [p, lfd] = bind_server(port);
    start_accepting(std::move(lfd));
    return p;
  }

  uint16_t start_server(x23::HostPort bind_addr)
  {
    auto [p, lfd] = bind_server(bind_addr);
    start_accepting(std::move(lfd));
    return p;
  }
```

- [ ] **Step 2: Build `auth_test` to verify RpcServer compiles**

Run: `./scripts/build.sh -c clang -t Debug -d ~/build/hogd-race-fix auth_test`
Expected: compiles successfully

- [ ] **Step 3: Run existing auth tests to verify no regression**

Run: `ctest --test-dir ~/build/hogd-race-fix -V --output-on-failure -R AuthServerTest`
Expected: all tests pass

- [ ] **Step 4: Commit**

```bash
git add hogd/base/rpc_server.h
git commit -m "refactor(rpc): split start_server into bind_server + start_accepting

Decompose both start_server overloads so callers can bind to get a port
without starting the accept loop.  Existing start_server calls are
unchanged — they call both halves.

Part of #1656."
```

### Task 2: Restructure `auth_init()` to use two-phase startup

**Files:**
- Modify: `hogd/auth/auth.cc:42-76`

- [ ] **Step 1: Replace `start_server` calls with `bind_server` / `start_accepting`**

Replace the body of `auth_init` (lines 42–78) with the two-phase flow. The key change: `bind_server` to get ports, do everything else, then `start_accepting` last.

```cpp
  RpcServer<AuthDBRpc> auth_db_rpc;
  auth_db_rpc->max_message_size_ = hogd::LoadgenRpc::kRpcMaxMessageSize;
  auto [actual_db_port, db_lfd] =
      auth_db_rpc.bind_server(make_bind_host_port(bind_ip, db_port));
  CHECK(actual_db_port == db_port || db_port == 0);
  info("PORT auth.db={}", actual_db_port);

  RpcServer<AuthMempoolRpc> auth_mempool_rpc;
  auth_mempool_rpc->max_message_size_ = hogd::LoadgenRpc::kRpcMaxMessageSize;
  auto [actual_mempool_port, mempool_lfd] =
      auth_mempool_rpc.bind_server(make_bind_host_port(bind_ip, mempool_port));
  CHECK(actual_mempool_port == mempool_port || mempool_port == 0);
  info("PORT auth.mempool={}", actual_mempool_port);

  const std::string auth_db_address =
      format_advertised_address(bind_ip, actual_db_port);
  const std::string auth_mempool_address =
      format_advertised_address(bind_ip, actual_mempool_port);

  AuthCard auth_card{.protocol = version::kHogdProtocol(),
                     .upid = {},
                     .address_for_db = auth_db_address.c_str(),
                     .address_for_mempool = auth_mempool_address.c_str()};
  IntroAuthResult intro = mempool_client.mempoolauth_intro_auth(auth_card)();
  if (intro.shards().empty()) {
    throw std::runtime_error("No db shards registered with mempool");
  }
  ShardCard &shard = intro.shards()[0];
  const std::string address_for_auth = shard.address_for_auth.c_str();
  dbauth_client_t dbauth_client = make_dbauth_client(address_for_auth)();
  AuthServer auth_server(std::move(dbauth_client));

  auth_server.set_mempool_client(std::move(mempool_client));
  auth_db_rpc->impl()->set_auth_server(auth_server);
  auth_mempool_rpc->impl()->set_auth_server(auth_server);

  auth_db_rpc.start_accepting(std::move(db_lfd));
  auth_mempool_rpc.start_accepting(std::move(mempool_lfd));

  auth_server.subscribe_to_auth_data_changes(auth_card)();
```

- [ ] **Step 2: Build `auth_exec` to verify the binary compiles**

Run: `./scripts/build.sh -c clang -t Debug -d ~/build/hogd-race-fix auth_exec`
Expected: compiles successfully

- [ ] **Step 3: Commit**

```bash
git add hogd/auth/auth.cc
git commit -m "fix(auth): eliminate auth_server_ publication race (#1656)

Use bind_server to get ports without starting the accept loop, then
start_accepting only after set_auth_server has been called.  Handlers
cannot fire before the backend pointer exists."
```

### Task 3: Build broader targets and run full test suite

- [ ] **Step 1: Build all test targets that use RpcServer**

Run: `./scripts/build.sh -c clang -t Debug -d ~/build/hogd-race-fix auth_test`
Expected: compiles successfully (auth_test exercises RpcServer via `db_server.start_server(0)`)

- [ ] **Step 2: Run full auth test suite**

Run: `ctest --test-dir ~/build/hogd-race-fix -V --output-on-failure -R AuthServerTest`
Expected: all tests pass

- [ ] **Step 3: Build hogexec tests (heaviest RpcServer user)**

Run: `./scripts/build.sh -c clang -t Debug -d ~/build/hogd-race-fix executor_test`
Expected: compiles successfully (confirms start_server(0) call sites still work)

# Auth RPC Adapter Publication Race Fix

**Date**: 2026-04-30
**Issue**: #1656
**Severity**: High

## Problem

`AuthDBRpc` and `AuthMempoolRpc` store their `AuthServer*` backend via a plain pointer write (`set_auth_server`) after `RpcServer::start_server()` has already begun dispatching handlers on the x23 thread pool. The handler-thread loads of `auth_server_` race with the main-thread store — undefined behavior under the C++ memory model.

The root cause is a startup ordering constraint: `start_server()` must be called first to get port numbers for the `AuthCard` intro handshake, but `start_server()` immediately begins accepting connections and dispatching handlers that read `auth_server_`.

## Approach

Split `RpcServer::start_server()` into two phases — `bind_server` (bind + listen, return port and fd) and `start_accepting` (begin dispatching handlers). `start_server` becomes a convenience that calls both. No existing call sites change.

In `auth_init()`, use the two-phase API: bind to get ports, do the intro handshake, construct `AuthServer`, bind it to the adapters, then start accepting. Handlers cannot fire before the backend exists.

## Changes

### `hogd/base/rpc_server.h` — Add `bind_server` / `start_accepting`

New public methods on `RpcServer`:

```cpp
std::pair<uint16_t, x23::UniqueFd> bind_server(x23::HostPort bind_addr)
{
    CHECK_EQ(listen_fd_.load(std::memory_order_relaxed), x23::kInvalidFd);
    auto addr = bind_addr.to_addr(false);
    CHECK(addr.has_value());
    int backlog = 5 + cp_.pool_.nthreads_;
    auto lfd = x23::tcplisten(*addr, backlog);
    listen_fd_ = lfd.get();
    port_ = x23::get_port(x23::local_addr(lfd));
    return {port_, std::move(lfd)};
}

void start_accepting(x23::UniqueFd lfd)
{
    x23::start_listening(cp_, lfd,
        [self = x23::RefPtr(*this),
         _d = x23::Defer([this] { eof(); })](auto c) {
            c->max_message_size = self->max_message_size_;
            new Conn{*self, c};
        });
}
```

Refactor existing `start_server(HostPort)` to call both:

```cpp
uint16_t start_server(x23::HostPort bind_addr)
{
    auto [port, lfd] = bind_server(bind_addr);
    start_accepting(std::move(lfd));
    return port;
}
```

The `uint16_t` port overload of `start_server` gets the same treatment if it uses `tcplisten` directly — or it can construct a `HostPort` and delegate. Either way, existing callers are unchanged.

### `hogd/auth/auth.cc` — Restructure `auth_init()` startup order

Before (racy):
1. `start_server` both RPC servers (handlers can now run)
2. Intro handshake (needs ports)
3. Construct `AuthServer` (needs intro result)
4. `set_auth_server` on both adapters

After (race-free):
1. `bind_server` both RPC servers (get ports, no handlers yet)
2. Intro handshake (needs ports)
3. Construct `AuthServer` (needs intro result)
4. `set_auth_server` on both adapters
5. `start_accepting` on both servers (handlers can now run)

The `UniqueFd` for each server is held on the stack between steps 1 and 5.

### `hogd/auth/auth_server.h` — No structural changes needed

`set_auth_server` stays as-is. The race is eliminated by startup ordering, not by changing the pointer type. Handlers are guaranteed to see a non-null, fully-constructed `AuthServer*` because `start_accepting` happens-after `set_auth_server` on the same thread, and handler dispatch happens-after `start_accepting` (the coroutine machinery provides the edge).

## What about the `uint16_t` port overload?

`start_server(uint16_t port)` also exists. Two options:
1. Give it its own `bind_server(uint16_t)` + same `start_accepting` pattern.
2. Leave it as-is since it's not used in the racy auth startup path.

Recommend (1) for consistency — both overloads decompose the same way.

## Testing

The PoC from #1656 exercises the race via a `std::jthread` + relaxed-order `std::atomic<bool>` gate. After the fix, the handler cannot run before `start_accepting`, so the race window is structurally eliminated. TSan should report clean on the existing test suite.

A targeted test could verify the two-phase startup by calling `bind_server`, checking the port is nonzero, then calling `start_accepting` and verifying a client can connect.

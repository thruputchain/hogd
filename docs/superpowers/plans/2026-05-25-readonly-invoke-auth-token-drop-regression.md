# Read-Only Invoke Auth Token Drop Regression Tests

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add regression tests proving that forwarding auth tokens into a read-only invoke and not returning them is WAI (tokens are dropped, equivalent to voluntary discard).

**Architecture:** Two tests using existing native contract test infrastructure. Test 1 (proposal mode) uses `SyscallAuthTokenTest` fixture with three contracts: caller mints+forwards token via read-only invoke, child ignores it, caller verifies the token is gone. Test 2 (viewing mode) uses `ViewTest` fixture with the same scenario driven through `run_view()`.

**Tech Stack:** C++, GTest, existing `NativeContractTestFramework` and `ViewTest` fixtures

---

### Task 1: Proposal mode test — `ReadOnlyInvokeDropsUnreturnedForwardedAuthTokens`

**Files:**
- Modify: `hogd/hogexec/runtime/syscall_auth_token_test.cc` (append before closing `NOLINTEND` markers)

- [ ] **Step 1: Write the test**

Insert before line 1719 (`// NOLINTEND(bugprone-unchecked-optional-access)`):

```cpp
TEST_F(SyscallAuthTokenTest, ReadOnlyInvokeDropsUnreturnedForwardedAuthTokens)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: should never be reached.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(
      0, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
        ADD_FAILURE() << "Consumer should never be invoked";
        return {};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Read-only child: receives the token but does not consume or return it.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints a token, forwards it to the read-only child, then tries
  // to forward the same token to the consumer (should fail).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Mint a token targeting the child.
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // Forward the token to the child via a read-only invoke.
        std::array<uint32_t, 1> forwarded{token_id};
        RuntimeMemoryLength out_ret_len = 0, out_auth_len = 0;
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, 0, UINT64_MAX, Calldata{},
            std::to_underlying(InvokeFlag::ReadOnly),
            std::span<uint32_t const>{forwarded}, out_ret_len, out_auth_len);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeNone);

        // The token was dropped by the read-only child. Attempting to
        // forward it again must fail — the caller no longer owns it.
        auto reuse_res = ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, 0, UINT64_MAX, Calldata{}, 0,
            std::span<uint32_t const>{forwarded}, out_ret_len, out_auth_len);
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  contract_map.emplace(2, std::move(consumer_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);
  auto consumer_xdr = make_native_contract(2);
  write_contract(consumer_xdr);
  write_deployment(kConsumerAddress, consumer_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}
```

- [ ] **Step 2: Build and run the test**

Run:
```bash
cmake --build ./build -j16 --target runtime_test
ctest --test-dir ./build --output-on-failure --timeout 120 -R "^SyscallAuthTokenTest\.ReadOnlyInvokeDropsUnreturnedForwardedAuthTokens$"
```
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add hogd/hogexec/runtime/syscall_auth_token_test.cc
git commit -m "test: read-only invoke drops unreturned forwarded auth tokens (proposal mode)

Regression test for #1765. Demonstrates that forwarding auth tokens into a
read-only invoke and not returning them is WAI — the tokens are dropped,
equivalent to the caller voluntarily discarding them."
```

---

### Task 2: Viewing mode test — `ViewReadOnlyInvokeDropsUnreturnedForwardedAuthTokens`

**Files:**
- Modify: `hogd/hogexec/runtime/view/view_test.cc` (append before closing `NOLINTEND` markers)

- [ ] **Step 1: Write the test**

Insert before line 1573 (`// NOLINTEND(bugprone-unchecked-optional-access)`):

```cpp
TEST_F(ViewTest, ViewReadOnlyInvokeDropsUnreturnedForwardedAuthTokens)
{
  static constexpr ContractAddress kOuterAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kChildAddr =
      NativeContractTestFramework::make_contract_address(11);
  static constexpr ContractAddress kConsumerAddr =
      NativeContractTestFramework::make_contract_address(12);
  static constexpr ContractAddress kParentAddr =
      NativeContractTestFramework::make_contract_address(20);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;

  // Method 0 (read-only child at kChildAddr): does nothing, token is dropped.
  fns.emplace(0,
              [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
                return {};
              });

  // Method 2 (consumer at kConsumerAddr): should never be reached.
  fns.emplace(2,
              [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
                ADD_FAILURE() << "Consumer should never be invoked";
                return {};
              });

  // Method 1 (outer at kOuterAddr): forwards the token to read-only child,
  // then tries to reuse it.
  fns.emplace(
      1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Token id 0 is the first (and only) forwarded token from the query.
        std::array<uint32_t, 1> forwarded{0};
        RuntimeMemoryLength out_ret_len = 0, out_auth_len = 0;

        // Forward token to read-only child.
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddr, 0, UINT64_MAX, Calldata{},
            std::to_underlying(InvokeFlag::ReadOnly),
            std::span<uint32_t const>{forwarded}, out_ret_len, out_auth_len);
        if (!invoke_res.has_value()) {
          ADD_FAILURE() << "read-only invoke syscall failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeNone);

        // Token is now gone. Attempting to forward it again must fail.
        auto reuse_res = ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddr, 2, UINT64_MAX, Calldata{}, 0,
            std::span<uint32_t const>{forwarded}, out_ret_len, out_auth_len);
        if (!reuse_res.has_value()) {
          ADD_FAILURE() << "reuse invoke syscall failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        EXPECT_EQ(reuse_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
      });

  NativeViewSetup setup(std::move(fns));

  deploy_native(kOuterAddr);
  deploy_native(kChildAddr);
  deploy_native(kConsumerAddr);

  Invocation invocation{
      .invoked_address = kOuterAddr,
      .entrypoint = entrypoint_from_method_id(1),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto query = make_query_with_parent(invocation, kParentAddr);
  query.forwarded_tokens.push_back(
      AuthToken{.mint_address = kParentAddr,
                .expiry_height = 1'000'000,
                .root_node = {.consumer_address = kChildAddr,
                              .payload = {},
                              .child_nodes = {}}});

  auto result = run_view(query, *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);
}
```

- [ ] **Step 2: Build and run the test**

Run:
```bash
cmake --build ./build -j16 --target view_test
ctest --test-dir ./build --output-on-failure --timeout 120 -R "^ViewTest\.ViewReadOnlyInvokeDropsUnreturnedForwardedAuthTokens$"
```
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add hogd/hogexec/runtime/view/view_test.cc
git commit -m "test: read-only invoke drops unreturned forwarded auth tokens (viewing mode)

Regression test for #1765. Exercises the same WAI behavior through the
view transaction path — forwarding tokens into a read-only child invoke
drops them, and the caller can no longer reuse them."
```

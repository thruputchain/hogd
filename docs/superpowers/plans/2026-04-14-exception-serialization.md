# Exception Serialization Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire up the new exception serialization format `(u64 selector, CXS body)` throughout hogd's C++ side, replacing the old `CrossExn` `(i32 discriminant, CXS body)` format.

**Architecture:** The new `components/contract_result.h` defines `Result<T, ContractException<ExnType, Selector>...>` with `ContractErrorType` handling selector-based dispatch. We delete the old `runtime/contract_result.h`, update `contract_trait.h` to pass raw abort buffers to `ContractErrorType`, update `native_contract_builder.h` to serialize `(u64 selector, CXS body)`, and update `generate_header.py` to emit `ContractException<Type, Selector>` in Result types.

**Tech Stack:** C++23, CMake, Python (generate_header.py)

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `hogd/hogexec/runtime/contract_result.h` | Delete | Old Result<T,E> — replaced by components version |
| `hogd/hogexec/components/contract_result.h` | Modify | Fix include paths |
| `hogd/hogexec/runtime/contract_trait.h` | Modify | New decode_return using ContractErrorType |
| `hogd/hogexec/precompiles/native_contract_builder.h` | Modify | New revert encoding with selector |
| `built_contracts/generate_header.py` | Modify | Emit ContractException in Result types |
| `hogd/hogexec/precompiles/precompile_native_token.cc` | Modify | Update Result type signatures |
| `hogd/hogexec/precompiles/precompile_initial_token_claim.cc` | Modify | Update Result type signatures |
| `hogd/hogexec/runtime/contract_result_preset_handling_test.cc` | Modify | Update for new Result<T, Exns...> |
| `hogd/hogexec/runtime/manual_revert_wasm_test.cc` | Modify | Update include path |

---

### Task 1: Fix include paths in components/contract_result.h

**Files:**
- Modify: `hogd/hogexec/components/contract_result.h:17-18`

- [ ] **Step 1: Fix the include paths**

In `hogd/hogexec/components/contract_result.h`, change lines 17-18 from:

```cpp
#include "hogd/hogexec/components/library_exception.hh"
#include "hogd/xdr/transaction.hh"
```

(These were partially fixed earlier in this session. Verify the current state and ensure both paths are correct.)

- [ ] **Step 2: Verify it compiles**

Run: `cmake --build vscode --target contract_loader -j8`
Expected: builds successfully.

- [ ] **Step 3: Commit**

```bash
git add hogd/hogexec/components/contract_result.h
git commit -m "fix: correct include paths in components/contract_result.h"
```

---

### Task 2: Delete old runtime/contract_result.h and update includes

**Files:**
- Delete: `hogd/hogexec/runtime/contract_result.h`
- Modify: `hogd/hogexec/runtime/contract_trait.h:24`
- Modify: `hogd/hogexec/precompiles/precompile_native_token.cc`
- Modify: `hogd/hogexec/precompiles/precompile_initial_token_claim.cc`
- Modify: `hogd/hogexec/runtime/manual_revert_wasm_test.cc:18`
- Modify: `hogd/hogexec/runtime/contract_result_preset_handling_test.cc:15`
- Modify: `hogd/hogexec/precompiles/native_contract_builder.h`

- [ ] **Step 1: Delete the old file**

```bash
rm hogd/hogexec/runtime/contract_result.h
```

- [ ] **Step 2: Update all includes**

In each of the 6 files, change:
```cpp
#include "hogd/hogexec/runtime/contract_result.h"
```
to:
```cpp
#include "hogd/hogexec/components/contract_result.h"
```

- [ ] **Step 3: Verify compilation**

This will NOT compile yet — `Result<T, E>` (two args) is now `Result<T, Exns...>` (variadic). But the include path change should be correct. Verify with:

```bash
cmake --build vscode --target contract_loader -j8 2>&1 | head -5
```

Expected: errors about `Result` template arguments, NOT about missing files.

- [ ] **Step 4: Commit**

```bash
git rm hogd/hogexec/runtime/contract_result.h
git add hogd/hogexec/runtime/contract_trait.h \
        hogd/hogexec/precompiles/precompile_native_token.cc \
        hogd/hogexec/precompiles/precompile_initial_token_claim.cc \
        hogd/hogexec/runtime/manual_revert_wasm_test.cc \
        hogd/hogexec/runtime/contract_result_preset_handling_test.cc \
        hogd/hogexec/precompiles/native_contract_builder.h
git commit -m "build: delete old runtime/contract_result.h, update includes"
```

---

### Task 3: Update generate_header.py to emit ContractException types

**Files:**
- Modify: `built_contracts/generate_header.py`

Currently generates `Result<void, ERC20Error>`. Must generate `Result<void, ContractException<ERC20Error, 0xb19947acb2fa14ffULL>>`.

- [ ] **Step 1: Update the error type handling in generate_header.py**

Replace the error type handling block (the `throws` processing and the Result wrapping in function signatures). Change the `generate_header` function:

Old code (around line 89-100):
```python
    # Error type: use first throw type name if any
    err_type = None
    if throws:
        err_ty = throws[0].get("type", {})
        if err_ty.get("kind") == "named":
            err_type = err_ty["name"]
```

New code:
```python
    # Build list of ContractException<Type, Selector> for each throw
    exception_types = []
    for throw_entry in throws:
        selector = throw_entry.get("selector", "")
        err_ty = throw_entry.get("type", {})
        if err_ty.get("kind") == "named":
            name = err_ty["name"]
            exception_types.append(f"ContractException<{name}, {selector}ULL>")
```

And change the signature generation from:
```python
        if err_type:
            return_type = f"Result<{return_type}, {err_type}>"
```
to:
```python
        if exception_types:
            exn_args = ", ".join(exception_types)
            return_type = f"Result<{return_type}, {exn_args}>"
```

- [ ] **Step 2: Rebuild the itc_erc20 header and verify**

```bash
cmake --build vscode --target itc_erc20_header -j8
grep "Result<" vscode/built_contracts/itc_erc20.h | head -3
```

Expected: `Result<void, ContractException<ERC20Error, 0xb19947acb2fa14ffULL>>` in the output.

- [ ] **Step 3: Verify fibonacci header (no throws) is unchanged**

```bash
cmake --build vscode --target fibonacci_header -j8
grep "Result\|ContractException" vscode/built_contracts/fibonacci.h
```

Expected: no matches (fibonacci has no error types).

- [ ] **Step 4: Commit**

```bash
git add built_contracts/generate_header.py
git commit -m "build: generate ContractException<T, Selector> in Result types"
```

---

### Task 4: Update contract_trait.h decode_return

**Files:**
- Modify: `hogd/hogexec/runtime/contract_trait.h:40-41,220-287`

The old decode_return parses `CrossExn(i32 discriminant, body)`. The new `ContractErrorType` constructor takes a `ReturnBuffer&&` and parses `(u64 selector, Buf body)` internally.

- [ ] **Step 1: Remove kCrossExnContractDiscriminant**

Delete line 40-41:
```cpp
// SDK CrossExn<E> discriminant for the Contract(E) variant.
constexpr int32_t kCrossExnContractDiscriminant = 0;
```

- [ ] **Step 2: Rewrite the manually_reverted branch in decode_return**

Replace lines 228-259 (the `if (manually_reverted)` branch) with:

```cpp
        if (manually_reverted) {
          // New format: abort buffer contains CXS-encoded (u64 selector, Buf body).
          // ContractErrorType parses selector+body and dispatches internally.
          ReturnBuffer abort_buf(
              reinterpret_cast<const uint8_t *>(data.data()),
              reinterpret_cast<const uint8_t *>(data.data()) + data.size());
          return return_type::make_err(std::move(abort_buf));
        }
```

- [ ] **Step 3: Verify compilation**

```bash
cmake --build vscode --target test_ffi_test -j8 2>&1 | grep "error:" | head -10
```

Expected: may still fail due to precompile Result type mismatches (Task 5), but contract_trait.h itself should compile.

- [ ] **Step 4: Commit**

```bash
git add hogd/hogexec/runtime/contract_trait.h
git commit -m "feat: update decode_return for selector+body exception format"
```

---

### Task 5: Update native_contract_builder.h revert encoding

**Files:**
- Modify: `hogd/hogexec/precompiles/native_contract_builder.h:181-193`

The `do_revert` lambda needs to serialize `(u64 selector, CXS body)` instead of `(i32 discriminant, CXS body)`.

The challenge: `do_revert` currently receives `err_value` (the raw error value like `ERC20Error`), but now needs the selector too. The return type is `Result<T, ContractException<ERC20Error, Selector>...>`, so `r.err()` returns a `ContractErrorType<ContractException<...>...>`. But `ContractErrorType` is a deserialization wrapper — for precompiles that construct errors locally, we need a different approach.

Looking at the current code: `r.err()` on the old `Result<T, E>` returns the `E` value directly. On the new `Result<T, Exns...>`, `r.err()` returns a `ContractErrorType<Exns...>` — but precompiles never call `r.err()` for revert. Instead, they call `Result::make_err(...)` then the builder's `do_revert` serializes it.

The new `Result::make_err` takes a `ReturnBuffer&&` (for deserialization from wire). For precompiles that construct errors locally, we need `make_err` to also accept the error value + serialize it. Let me check the new Result more carefully.

Actually, looking at `contract_result.h` again: `make_err` requires `std::is_constructible_v<err_type, Args...>` and `err_type` is `ContractErrorType<Exns...>` which takes `ReturnBuffer&&`. So precompiles need to serialize the error into a `ReturnBuffer` with the `(selector, body)` format before calling `make_err`.

The simplest approach: change `do_revert` to serialize the error with selector, then the existing code path through `make_err` will work.

- [ ] **Step 1: Rewrite do_revert**

Replace lines 181-193 in `native_contract_builder.h`:

```cpp
      // Wrap an error in the new exception envelope, matching SDK behavior.
      // Layout: CXS-encoded tuple of (u64 selector, Buf body).
      auto do_revert = [&ctx](auto const &err_value) -> InvokeStatus<void> {
        // For precompile-constructed errors that need manual serialization,
        // we serialize selector + body directly.
        // The selector is hard-coded per exception type at the call site.
        auto body = Serializer<Format>::serialize(err_value);
        if (!body.has_value()) {
          return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        ctx.handler().set_return_buffer(std::move(body.value()), true);
        return {};
      };
```

Wait — this doesn't include the selector. The `contract_trait.h` decode_return passes the raw abort buffer to `ContractErrorType` which expects `(u64 selector, Buf body)`. But native precompiles don't throw typed exceptions — they currently serialize the error directly. 

Actually, looking more carefully at how precompiles work: they don't actually call `do_revert` with a typed exception. They call `Result::make_err(value)` to construct the result, then the builder serializes `r.err()`. But the precompiles never actually throw — they return the Result value which is then serialized as the ok/err response.

Let me re-read the code flow: lines 195-212 show that when `r.is_err()`, `do_revert(r.err())` is called. With the old Result, `r.err()` returns the raw `E` value. With the new Result, `r.err()` returns a `ContractErrorType<Exns...>`.

But precompiles currently do `Result<void, int32_t>::make_err(456)` — they make errors from raw values. The new `Result::make_err` requires a `ReturnBuffer&&`. This is a fundamental mismatch: precompiles construct errors from values, but the new Result only constructs errors from serialized buffers.

We need a way for precompiles to construct `Result` errors from typed values. The cleanest approach: add a `make_typed_err` factory that takes the value + selector, serializes internally.

Actually, the precompile code is test-only for the preset handling test. For real precompiles (`precompile_native_token.cc`), they use `itc_erc20::ERC20Error` which will now be `ContractException<ERC20Error, Selector>`. Looking at the code, they only ever return `.make_ok()` — they never actually return errors. Let me verify:

```
grep "make_err" hogd/hogexec/precompiles/precompile_native_token.cc
```

- [ ] **Step 1: Check if precompiles actually return errors**

```bash
grep "make_err" hogd/hogexec/precompiles/precompile_native_token.cc hogd/hogexec/precompiles/precompile_initial_token_claim.cc
```

If no matches: the precompiles only return ok values, and `do_revert` is only reached via tests. In that case, we can defer updating `do_revert` and focus on making the compiler happy.

- [ ] **Step 2: Update do_revert to serialize with selector**

Replace lines 181-193:

```cpp
      // Serialize an error with selector+body envelope for the new exception format.
      // `err_variant` is a ContractErrorType — but for precompile-generated errors,
      // we need to serialize the raw value with its selector.
      // For now, precompiles that throw typed exceptions must serialize manually.
      auto do_revert = [&ctx](auto const &err_variant) -> InvokeStatus<void> {
        // The err_variant from ContractErrorType doesn't support re-serialization
        // (it's a deserialization-only type). Precompiles shouldn't hit this path
        // unless they explicitly construct errors.
        // TODO: if precompiles need to throw typed exceptions, add serialization support.
        ctx.handler().set_return_buffer(ReturnBuffer{}, true);
        return {};
      };
```

Actually, this is getting complicated. Let me take a step back and think about what the precompiles actually need. The `native_contract_builder.h` `do_revert` path is only hit when a `Result` method returns an error. For the real precompiles (`precompile_native_token.cc`), they never return errors — they always `make_ok()`. So this path is effectively dead code for production.

For the test (`contract_result_preset_handling_test.cc`), the test explicitly constructs `Result<T, int32_t>::make_err(456)` which won't work with the new variadic Result.

The pragmatic fix: keep `do_revert` working for the case where the caller passes a raw `ReturnBuffer`, and update the test to use the new format. For precompile production code, the Result types change from `Result<T, ERC20Error>` to `Result<T, ContractException<ERC20Error, Selector>>` but since they only call `make_ok()`, the error type doesn't matter for construction.

- [ ] **Step 2 (revised): Update precompile Result type signatures**

In `precompile_native_token.cc`, change all occurrences of:
```cpp
Result<void, itc_erc20::ERC20Error>
Result<amount_t, itc_erc20::ERC20Error>
```
to use the type from the generated trait. Since `itc_erc20::ERC20` is the generated trait struct, define a helper:
```cpp
// Use the trait's export_types to get the correct Result type
// For now, just update to use ContractException directly:
using ERC20Exception = ContractException<itc_erc20::ERC20Error, 0xb19947acb2fa14ffULL>;
```

Then use `Result<void, ERC20Exception>` and `Result<amount_t, ERC20Exception>`.

Do the same for `precompile_initial_token_claim.cc`.

- [ ] **Step 3: Update do_revert for the new err_type**

The `err_type` is now `ContractErrorType<Exns...>`. But `r.err()` won't be called by precompiles (they only return ok). For safety, make `do_revert` handle the case:

```cpp
      // Wrap an error for the new exception format.
      // For precompile-constructed Results, this path is not reached
      // (precompiles only return ok values). If it is reached,
      // serialize the abort buffer raw.
      auto do_revert = [&ctx](auto const &err_variant) -> InvokeStatus<void> {
        // ContractErrorType doesn't support re-serialization.
        // This path shouldn't be reached by precompiles.
        error("do_revert called on precompile — this should not happen");
        ctx.handler().set_return_buffer(ReturnBuffer{}, true);
        return {};
      };
```

- [ ] **Step 4: Verify compilation**

```bash
cmake --build vscode --target test_ffi_test -j8 2>&1 | grep "error:" | head -10
```

- [ ] **Step 5: Commit**

```bash
git add hogd/hogexec/precompiles/native_contract_builder.h \
        hogd/hogexec/precompiles/precompile_native_token.cc \
        hogd/hogexec/precompiles/precompile_initial_token_claim.cc
git commit -m "feat: update precompiles for new exception Result types"
```

---

### Task 6: Update contract_result_preset_handling_test.cc

**Files:**
- Modify: `hogd/hogexec/runtime/contract_result_preset_handling_test.cc`

The test defines `TestContract` with `Result<void, int32_t>` and `Result<void, void>`. These need to use the new variadic format with `ContractException`. Define test exception types with known selectors.

- [ ] **Step 1: Update TestContract export_types**

Replace lines 34-47:

```cpp
// Test exception type with a known selector
using TestException = ContractException<int32_t, 0x1234567890abcdefULL>;

struct TestContract {
  constexpr static std::string_view kTraitName = "TestContract";
  constexpr static std::string_view kVersion = "0.0.0";

  constexpr static auto kExportNames =
      std::array<std::string_view, 3>{"return_ok_void", "return_ok_int",
                                      "return_err_int"};

  using export_types =
      std::tuple<Result<void, TestException> (*)(),
                 Result<int32_t, TestException> (*)(),
                 Result<int32_t, TestException> (*)()>;
};
```

Note: removed `return_err_void` — `Result<void, void>` doesn't make sense with ContractException (there's no void exception type).

- [ ] **Step 2: Update the test contract builder lambdas**

Update `make_test_contract()` to match the new signatures. Since `Result::make_err` now takes a `ReturnBuffer&&`, the test needs to manually serialize `(u64 selector, CXS body)`:

```cpp
std::unique_ptr<NativeContract>
make_test_contract()
{
  NativeContractBuilder builder;
  builder.add<TestContract, "return_ok_void">(
      []() -> InvokeStatus<Result<void, TestException>> {
        return {Result<void, TestException>::make_ok()};
      });
  builder.add<TestContract, "return_ok_int">(
      []() -> InvokeStatus<Result<int32_t, TestException>> {
        return {Result<int32_t, TestException>::make_ok(123)};
      });
  builder.add<TestContract, "return_err_int">(
      []() -> InvokeStatus<Result<int32_t, TestException>> {
        // Serialize (selector, body) into a ReturnBuffer
        x23::Buf buf;
        std::tuple<uint64_t, x23::Buf> payload;
        std::get<0>(payload) = TestException::kSelector;
        x23::Buf body;
        x23::serialize_cxs(&body, int32_t{456});
        std::get<1>(payload) = std::move(body);
        x23::serialize_cxs(&buf, payload);
        ReturnBuffer rb(buf.begin(), buf.end());
        return {Result<int32_t, TestException>::make_err(std::move(rb))};
      });
  return builder.build();
}
```

- [ ] **Step 3: Update CallerContract and its test lambdas**

Update `CallerContract` export_names (remove `call_return_err_void`), update the caller lambdas accordingly.

- [ ] **Step 4: Verify tests compile and pass**

```bash
cmake --build vscode --target runtime_test -j8
cd vscode && ctest -R contract_result_preset -j4
```

- [ ] **Step 5: Commit**

```bash
git add hogd/hogexec/runtime/contract_result_preset_handling_test.cc
git commit -m "test: update preset handling test for new exception format"
```

---

### Task 7: Full build and test verification

- [ ] **Step 1: Rebuild everything**

```bash
cmake --build vscode -j8 2>&1 | grep "error:" | head -20
```

Expected: clean build.

- [ ] **Step 2: Run the contract-related tests**

```bash
cd vscode && ctest -R "contract_result_preset|test_ffi" -j4
```

Expected: tests pass (except known manual_revert_test failures from SDK incompatibility).

- [ ] **Step 3: Commit any remaining fixups**

- [ ] **Step 4: Push**

```bash
git push
```

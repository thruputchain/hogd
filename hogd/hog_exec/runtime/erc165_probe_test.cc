#include <gtest/gtest.h>

// End-to-end rv32 coverage for ERC165 on the native token, driven through the
// erc165_probe contract (local_test_contracts): an RV32 contract asks the
// genesis-deployed native token what it implements through ERC165Client, the
// way real callers do -- read-only, with any failed call reading as
// "unsupported". erc20_permit_parity_test.cc checks the native token's
// answers byte for byte, but from a native driver making plain invokes.

#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include "built_contracts/erc165_probe.h"
#include "built_contracts/itc_erc20.h"
#include "built_contracts/itc_erc20_permit.h"
#include "contract.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/network_config_params.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace {

template<is_contract_trait Trait>
InterfaceId
interface_id_of()
{
  return sdk_interface_id_derivation(Trait::kTraitName, Trait::kVersion);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
class Erc165ProbeTest : public ::testing::Test,
                        public NativeContractTestFramework {
public:
  Erc165ProbeTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal)
  {}

protected:
  using probe_meta = ContractTraitMeta<erc165_probe::Erc165Probe>;

  // supports' answers: ERC20, ERC20Permit, ERC165, and the probe's own
  // interface, which no target implements.
  using Support = std::tuple<bool, bool, bool, bool>;

  void SetUp() override
  {
    deploy(kProbeAddress, probe_meta::load_rv32());
    // RV32 controls: the reference the native token is held to, which
    // implements all three interfaces, and an ERC20 without ERC165.
    deploy(kPermitReferenceAddress,
           ContractTraitMeta<itc_erc20_permit::ERC20>::load_rv32());
    deploy(kNoErc165Address, ContractTraitMeta<itc_erc20::ERC20>::load_rv32());

    runtime_context_ = make_hog_exec_context();
  }

  void deploy(ContractAddress const &address,
              std::shared_ptr<Contract> const &contract)
  {
    ASSERT_TRUE(contract);
    write_contract(contract);
    write_deployment(address, contract);
    setup_grant_storage_token(address, 1'000'000'000);
  }

  // Runs one probe method as the root of a transaction; nullopt (after a
  // test failure) if the probe itself did not return.
  template<StringLiteral Name, typename... Args>
  std::optional<probe_meta::return_t<Name>> probe(Args &&...args)
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation = client_.make_invocation<Name>(
                        kDefaultMaxTxGas, std::forward<Args>(args)...),
                    .uniquifier = make_new_uniquifier()},
        *runtime_context_);
    EXPECT_TRUE(handler.root_invoke()().is_root_success());

    auto output = handler.get_most_recent_frame_output();
    EXPECT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
    if (output.status_code() != InvocationFrameStatusCode::Return) {
      return std::nullopt;
    }
    auto decoded = probe_meta::decode_return<Name>(
        std::as_bytes(std::span(output.return_buffer().data(),
                                output.return_buffer().size())),
        false);
    EXPECT_TRUE(decoded.has_value());
    return decoded;
  }

  // What one read-only supports_interface call on `target` answered, or
  // nullopt when the call failed.
  std::optional<bool> probe_interface(ContractAddress const &target,
                                      InterfaceId const &id)
  {
    auto answer = probe<u8"probe_interface">(target, id);
    if (!answer.has_value() || !*answer) {
      return std::nullopt;
    }
    return **answer;
  }

  constexpr static ContractAddress kProbeAddress = make_contract_address(0);
  constexpr static ContractAddress kPermitReferenceAddress =
      make_contract_address(1);
  constexpr static ContractAddress kNoErc165Address = make_contract_address(2);

  ContractTraitClient<erc165_probe::Erc165Probe> client_ =
      ContractTraitClient<erc165_probe::Erc165Probe>(kProbeAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;
};

// ERC165Client::supports sees the native token implement ERC20, ERC20Permit
// and ERC165, exactly as it sees the RV32 reference. Since supports reads any
// failed call as all-false, the true answers also show the read-only
// supports_interfaces call into the precompile succeeded.
TEST_F(Erc165ProbeTest, SupportsSeesTheNativeTokenInterfaces)
{
  Support const expected{true, true, true, false};
  EXPECT_EQ(probe<u8"supports">(kNativeTokenContractAddress), expected);
  EXPECT_EQ(probe<u8"supports">(kPermitReferenceAddress), expected);
}

// A single read-only supports_interface call into the native token returns
// rather than fails, for an interface it implements and for one it does not.
TEST_F(Erc165ProbeTest, ReadOnlySupportsInterfaceReturnsOnTheNativeToken)
{
  auto const erc20 = interface_id_of<itc_erc20_permit::ERC20>();
  auto const probe_iface = interface_id_of<erc165_probe::Erc165Probe>();

  EXPECT_EQ(probe_interface(kNativeTokenContractAddress, erc20), true);
  EXPECT_EQ(probe_interface(kNativeTokenContractAddress, probe_iface), false);
  EXPECT_EQ(probe_interface(kPermitReferenceAddress, erc20), true);
  EXPECT_EQ(probe_interface(kPermitReferenceAddress, probe_iface), false);
}

// Control: a contract without ERC165 rejects the call, which supports reads
// as all-false and probe_interface reports as a failed call -- the outcome
// the native token cases above rule out.
TEST_F(Erc165ProbeTest, ContractWithoutErc165ReadsAsUnsupported)
{
  EXPECT_EQ(probe<u8"supports">(kNoErc165Address),
            (Support{false, false, false, false}));
  EXPECT_EQ(probe_interface(kNoErc165Address,
                            interface_id_of<itc_erc20::ERC20>()),
            std::nullopt);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd

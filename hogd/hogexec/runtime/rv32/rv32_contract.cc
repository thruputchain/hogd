#include "hogd/hogexec/runtime/rv32/rv32_contract.h"

#include "hogd/hogexec/runtime/rv32/artifact.h"

namespace hogd {

std::optional<Rv32Contract>
Rv32Contract::create(ContractBlob const &contract_blob)
{

  auto artifact = rv32::parse_threaded_artifact(contract_blob.data);
  if (!artifact.has_value()) {
    return std::nullopt;
  }

  auto pre_instantiation = rv32::ThreadedMachinePreInstantiation::create(
      artifact->code, artifact->initial_memory, artifact->memory_bytes, false);
  if (!pre_instantiation.has_value()) {
    return std::nullopt;
  }

  return std::make_optional<Rv32Contract>(std::move(*pre_instantiation));
}

Rv32Contract::Rv32Contract(
    rv32::ThreadedMachinePreInstantiation pre_instantiation)
  : pre_instantiation_(std::move(pre_instantiation))
{}

rv32::ThreadedMachine
Rv32Contract::instantiate(uint64_t gas) const
{
  return rv32::ThreadedMachine(pre_instantiation_, gas);
}

std::pair<InvokeStatus<void>, GasAmount>
Rv32Runtime::invoke_internal(MethodID method_id, uint64_t gas_limit)
{
  rv32::ThreadedMachine machine = contract_.instantiate(gas_limit);
  machine.seed_method_id(method_id);

  throw std::runtime_error("Not implemented");
}

} // namespace hogd

cmake_minimum_required(VERSION 3.28)
file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/client.x" [=[
struct Tx {
  unsigned hyper seq;
  opaque memo<16>;
};

typedef Tx TxBatch<4>;
typedef unsigned hyper Amount;

program sample {
  version sample_v1 {
    void ping(void) = 0;
    int add(int, unsigned hyper) = 1;
    bool submit(TxBatch, bool) = 2;
    void credit(Amount, Tx) = 3;
  } = 1;
} = 0x20000002;
]=])

execute_process(
  COMMAND "${X23C}" --rust -o "${TEST_DIR}/client.rs" "${TEST_DIR}/client.x"
  RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "x23c failed: ${error}")
endif()
file(READ "${TEST_DIR}/client.rs" rust)

function(require_text expected)
  string(FIND "${rust}" "${expected}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "Missing expected output: ${expected}")
  endif()
endfunction()

# Scalars stay by value; everything else, including a typedef of a scalar, is
# borrowed as anything that encodes like it.
require_text("pub async fn ping(&self) ->")
require_text("call.encode(&()).")
require_text("pub async fn add(&self, arg0 : i32, arg1 : u64) ->")
require_text("call.encode(&(arg0, arg1)).")
require_text("pub async fn submit(&self, arg0 : &(impl xdrs::EncodesAs<TxBatch> + ?Sized), arg1 : bool) ->")
require_text("call.encode(&(arg0, arg1)).")
require_text("pub async fn credit(&self, arg0 : &(impl xdrs::EncodesAs<Amount> + ?Sized), arg1 : &(impl xdrs::EncodesAs<Tx> + ?Sized)) ->")

# The server trait keeps owned arguments.
require_text("async fn submit(&self, arg0 : TxBatch, arg1 : bool) -> Result<bool, ::rpc_api::ProgramError>;")

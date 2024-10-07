%.wasm : Makefile $(wildcard $</src/*.rs)
	cargo rustc --target wasm32-unknown-unknown --release -p $(basename $@) \
	-- -C link-args=-zstack-size=64000

clean:
	cargo clean

.PHONY: clean rs_contracts

rs_contracts : basic.wasm basic_alloc.wasm test_calldata.wasm

# rustup target install wasm32-unknown-unknown

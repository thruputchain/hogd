build:
	cd ../ && make erc20.wasm

test:
	cargo +nightly test \
		--features "guest,std" \
		--target-dir "../../build/rs_contracts" \

test-client:
	cargo +nightly test \
		--features "client,std" \
		--target-dir "../../build/rs_contracts" \

expand:
	cargo +nightly expand \
		--features "client,std" \
		--target-dir "../../build/rs_contracts" \

tree:
	# Show the dependency graph
	cargo +nightly tree \
		--features "guest,alloc" \
		--target wasm32-unknown-unknown \
		-Zbuild-std=alloc,core \
		-Zbuild-std-features=panic_immediate_abort

.PHONY: build test test-client expand tree

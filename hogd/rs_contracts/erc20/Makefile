build:
	cd ../ && make erc20.wasm

test:
	cd ../ && make test_erc20

test-client:
	cargo +nightly test \
		--no-default-features \
		--features "client,std" \

expand:
	cargo +nightly expand \
		--no-default-features \
		--features "guest,client,alloc" \
		--target wasm32-unknown-unknown

tree:
	# Show the dependency graph
	cargo +nightly tree \
		--no-default-features \
		--features "guest,alloc,client,mock" \
		--target wasm32-unknown-unknown \
		-Zbuild-std=alloc,core \
		-Zbuild-std-features=panic_immediate_abort

.PHONY: build test test-client expand tree

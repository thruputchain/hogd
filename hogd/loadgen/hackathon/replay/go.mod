module github.com/thruputchain/hogd/hackathon

go 1.24.4

require (
	github.com/mr-tron/base58 v1.2.0
	github.com/thruputchain/lib-go v0.0.0-20261018054045-e56684cd4d86
	github.com/thruputchain/thruput-protocol-go v0.0.0-20261018052825-8f1017e9496b
	github.com/xdrpp/goxdr v0.0.0-20260507185951-17b997ee737d
)

require (
	github.com/mattn/go-colorable v0.1.13 // indirect
	github.com/mattn/go-isatty v0.0.20 // indirect
	github.com/rs/zerolog v1.34.0 // indirect
	golang.org/x/sys v0.37.0 // indirect
)

replace github.com/thruputchain/lib-go => ../../../../groundhog-go/lib-go

replace github.com/thruputchain/thruput-protocol-go => ../../../../groundhog-go/protocol-go

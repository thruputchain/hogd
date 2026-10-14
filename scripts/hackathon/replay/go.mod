module github.com/groundhogchain/hogd/hackathon

go 1.24.4

require (
	github.com/groundhogchain/lib-go v0.0.0-20260512044649-33b87d4adadb
	github.com/groundhogchain/protocol-go v0.0.0-20260929230109-5833a26ffc0e
	github.com/xdrpp/goxdr v0.0.0-20260507185951-17b997ee737d
)

require (
	github.com/mattn/go-colorable v0.1.13 // indirect
	github.com/mattn/go-isatty v0.0.20 // indirect
	github.com/rs/zerolog v1.34.0 // indirect
	golang.org/x/sys v0.37.0 // indirect
)

replace github.com/groundhogchain/lib-go => ../../../groundhog-go/lib-go

replace github.com/groundhogchain/protocol-go => ../../../groundhog-go/protocol-go

# Makefile for OpenDMR Go bindings
#
# This Makefile helps prepare the Go package for distribution by copying
# the necessary C library files into the Go package directory.

.PHONY: all clean prepare test

# Default target
all: prepare

# Prepare Go package by copying necessary files
prepare:
	@echo "Preparing Go package..."
	@cp ../opendmr.h .
	@cp ../libopendmr.a .
	@echo "✓ Go package ready for distribution"

# Clean copied files
clean:
	@echo "Cleaning Go package..."
	@rm -f opendmr.h libopendmr.a
	@rm -f example/example
	@echo "✓ Cleaned"

# Test the package
test: prepare
	@echo "Testing Go package..."
	@go build -v ./...
	@echo "✓ Package builds successfully"
	@echo "Testing example..."
	@cd example && go build -v
	@echo "✓ Example builds successfully"

# Run the example
run-example: test
	@echo "Running example..."
	@cd example && ./example

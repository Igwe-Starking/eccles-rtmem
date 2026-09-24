CC ?= gcc

.PHONY: test sanitize clean

## Run the full configuration matrix
test:
	bash ./tests/run_tests.sh

## Same matrix under AddressSanitizer + UndefinedBehaviorSanitizer
sanitize:
	EXTRA_CFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g" bash ./tests/run_tests.sh

clean:
	rm -rf build

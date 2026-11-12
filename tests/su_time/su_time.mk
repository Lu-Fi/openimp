CC ?= cc
BIN := build/tests/su-time-test

check:
	mkdir -p build/tests
	$(CC) -std=gnu99 -Wall -Wextra -Werror tests/su_time/su_time_test.c -o $(BIN)
	$(BIN) tests/su_time/vectors.txt

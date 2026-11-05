CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CPPFLAGS ?=
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS ?= -lm

# Only GF(2) sliding-window RLNC. The block/RS prototypes are in legacy/ and
# are not built; they exist as historical comparison only.
SRC = rlnc.c
OBJ = $(SRC:.c=.o)
HDR = rlnc.h

TEST_BINS = test_rlnc
BENCH_BINS = bench_rlnc
SIM_BINS = sim_rlnc sim_dynamic

ifeq ($(BUILD),asan)
CFLAGS := -O1 -g -std=c99 -Wall -Wextra -Werror \
          -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address,undefined
endif

all: $(TEST_BINS) $(BENCH_BINS) $(SIM_BINS)

test_rlnc: test_rlnc.o $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

bench_rlnc: bench_rlnc.o $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

sim_rlnc: sim_rlnc.o $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

sim_dynamic: sim_dynamic.o $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test:
	@$(MAKE) clean
	@$(MAKE) $(TEST_BINS)
	@./test_rlnc

bench:
	@$(MAKE) clean
	@$(MAKE) $(BENCH_BINS)
	@./bench_rlnc

sim:
	@$(MAKE) clean
	@$(MAKE) $(SIM_BINS)
	@./sim_rlnc
	@./sim_dynamic

asan:
	@$(MAKE) clean
	@$(MAKE) $(TEST_BINS) BUILD=asan
	@./test_rlnc

clean:
	rm -f $(OBJ) test_rlnc.o bench_rlnc.o sim_rlnc.o sim_dynamic.o \
	      $(TEST_BINS) $(BENCH_BINS) $(SIM_BINS)

.PHONY: all test bench sim asan clean

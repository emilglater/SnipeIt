# Builds the measurement programs.
# Each test is its own binary. src/main.c is excluded so that the project entry
# point does not collide with the test entry points.
#
#   make -C tests_hw
#   sudo ./tests_hw/bin/test1_event_bus_latency 60

SRC_DIR    := ../src
BIN_DIR    := ./bin

# Every project source except the production entry point.
PROJECT_SRCS := $(filter-out $(SRC_DIR)/main.c, $(shell find $(SRC_DIR) -type f -name '*.c'))

TESTS := test1_event_bus_latency \
         test2_gps_scatter       \
         test3_turret_accuracy

CC       := gcc
CFLAGS   := -Og -ggdb3 -std=c99 -pedantic -Wall -Wextra -Werror              \
            -Winline -Wconversion -Wpadded -Wshadow -Wundef                  \
            -Wdouble-promotion -fno-common -pthread
CPPFLAGS := -D_GNU_SOURCE -DUTIL_TRACE_ENABLED=1 -DLOG_LEVEL=LOG_LEVEL_WARNING \
            -I$(SRC_DIR)
LDFLAGS  := -pthread
LDLIBS   := -luring -lgpiod -lm -lrt

.PHONY: all clean
all: $(addprefix $(BIN_DIR)/, $(TESTS))

$(BIN_DIR)/%: %.c $(PROJECT_SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(BIN_DIR)

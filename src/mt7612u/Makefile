# Standalone. Not reachable from the project's CMakeLists.txt.
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
# -MMD -MP: without header deps, editing internal.h leaves stale objects with
# the old struct layout linked against new ones. That corrupts the device
# struct at runtime and presents as a wild pointer, not a build problem.
DEPFLAGS = -MMD -MP
LDLIBS   = -lusb-1.0 -lpthread
SRCS     = $(wildcard *.c) tools/bringup.c
OBJS     = $(SRCS:.c=.o)
DEPS     = $(OBJS:.o=.d)

all: bringup
bringup: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -I. -c -o $@ $<
clean:
	rm -f $(OBJS) $(DEPS) bringup
-include $(DEPS)
.PHONY: all clean

# Standalone. Not reachable from the project's CMakeLists.txt.
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
# -MMD -MP: without header deps, editing internal.h leaves stale objects with
# the old struct layout linked against new ones. That corrupts the device
# struct at runtime and presents as a wild pointer, not a build problem.
DEPFLAGS = -MMD -MP
LDLIBS   = -lusb-1.0 -lpthread
LIBSRCS  = $(wildcard *.c)
TESTS    = tests/api_link tests/frame_shape
SRCS     = $(LIBSRCS) tools/bringup.c $(TESTS:=.c)
OBJS     = $(SRCS:.c=.o)
LIBOBJS  = $(LIBSRCS:.c=.o)
DEPS     = $(OBJS:.o=.d)

all: bringup $(TESTS)

bringup: $(LIBOBJS) tools/bringup.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Links against the public header only, so a declaration with no definition
# is a link error here rather than a surprise at the first caller.
tests/api_link: $(LIBOBJS) tests/api_link.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tests/frame_shape: $(LIBOBJS) tests/frame_shape.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -I. -Iinclude -c -o $@ $<

# No hardware and no privileges needed: safe to run anywhere.
check: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(OBJS) $(DEPS) bringup $(TESTS)
-include $(DEPS)
.PHONY: all check clean

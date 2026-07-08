CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

APP = linux_bench
APP_OBJS = linux_bench.o ../common/common.o

# clock_gettime is in libc on modern glibc; add -lrt here if your toolchain needs it
LDLIBS ?=

all: $(APP)

$(APP): $(APP_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(APP_OBJS) $(LDLIBS)

clean:
	rm -rf $(APP) $(APP_OBJS)

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

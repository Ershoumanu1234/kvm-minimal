CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -g
CFLAGS += -std=gnu11

TARGET := minimal-kvm

all: $(TARGET)

$(TARGET): minimal-kvm.c
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean

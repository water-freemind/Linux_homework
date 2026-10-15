CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
TARGET := sparse_downloader
SOURCE := sparse_downloader.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

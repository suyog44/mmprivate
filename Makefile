# SPDX-License-Identifier: GPL-2.0
#
# Makefile for mthp_classifier v3.1 (userspace, BPF-free)

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE

TARGET = mthp_classifier
SRCS   = mthp_classifier.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

# Android NDK static arm64 build
NDK_VERSION ?= 26
API         ?= 31

android:
	@if [ -z "$(NDK)" ]; then echo "set NDK=/path/to/android-ndk"; exit 1; fi
	$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$(API)-clang \
		$(CFLAGS) -static -o $(TARGET).arm64 $(SRCS)
	@echo "Built $(TARGET).arm64 -- adb push $(TARGET).arm64 /data/local/tmp/mthp_classifier"

deploy: android
	adb push $(TARGET).arm64 /data/local/tmp/mthp_classifier
	adb shell "su 0 chmod +x /data/local/tmp/mthp_classifier"
	@echo "Run: adb shell 'su 0 /data/local/tmp/mthp_classifier --setup -v'"

clean:
	rm -f $(TARGET) $(TARGET).arm64

.PHONY: all android deploy clean

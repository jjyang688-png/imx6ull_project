CC     ?= gcc
CFLAGS ?= -Wall -O2 -g

# Phase 3/4: LED 驱动测试
# Phase 5: 按键输入测试
TARGETS := test_comp_drv test_key

# 如果测试程序在 ../test/ 目录，生成到 ../test/
test_comp_drv: ../test/test_comp_drv.c
	$(CC) $(CFLAGS) -o $@ $<

test_key: ../test/test_key.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: all clean

all: $(TARGETS)

clean:
	rm -f $(TARGETS)

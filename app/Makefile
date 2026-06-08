CC     ?= gcc
CFLAGS ?= -Wall -O2 -g -Wno-unused-result

# Phase 3/4: LED 驱动测试
# Phase 5: 按键输入测试
# Phase 6: I2C/SPI 传感器测试
# Phase 8: 统一监控守护进程
TARGETS := smart_monitor


smart_monitor: ../app/smart_monitor.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: all clean

all: $(TARGETS)

clean:
	rm -f $(TARGETS)

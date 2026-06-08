CC     ?= gcc
CFLAGS ?= -Wall -O2 -g

# Phase 3/4: LED 驱动测试
# Phase 5: 按键输入测试
# Phase 6: I2C/SPI 传感器测试
# Phase 8: 统一监控守护进程
TARGETS := test_comp_drv test_key test_icm20608 test_ap3216c smart_monitor

test_comp_drv: ../test/test_comp_drv.c
	$(CC) $(CFLAGS) -o $@ $<

test_key: ../test/test_key.c
	$(CC) $(CFLAGS) -o $@ $<

test_icm20608: ../test/test_icm20608.c
	$(CC) $(CFLAGS) -o $@ $<

test_ap3216c: ../test/test_ap3216c.c
	$(CC) $(CFLAGS) -o $@ $<

smart_monitor: ../app/smart_monitor.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: all clean

all: $(TARGETS)

clean:
	rm -f $(TARGETS)

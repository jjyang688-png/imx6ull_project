KERNEL_DIR ?= /home/yang/linux/alentek_uboot/linux/linux-imx-rel_imx_4.1.15_2.1.0_ga_alientek

# List kernel modules to build
obj-m := hello.o comp_drv.o key_input.o ap3216c.o icm20608.o
all:
	@if [ -z "$(KERNEL_DIR)" ] ; then \
		echo "Error: KERNEL_DIR not set"; \
		echo "Usage: make KERNEL_DIR=/path/to/kernel"; \
		exit 1; \
	fi
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean
	rm -f modules.order Module.symvers

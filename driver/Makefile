KERNEL_DIR ?= /home/alientek/linux/imx6ull

# List kernel modules to build
obj-m := hello.o

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

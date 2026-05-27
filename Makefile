# Top-level Makefile
KERNEL_DIR   ?= /home/yang/linux/alentek_uboot/linux/linux-imx-rel_imx_4.1.15_2.1.0_ga_alientek
ARCH         ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-

.PHONY: all modules apps clean help

all: modules apps

modules:
	$(MAKE) -C driver KERNEL_DIR=$(KERNEL_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

apps:
	$(MAKE) -C app CC=$(CROSS_COMPILE)gcc

clean:
	$(MAKE) -C driver KERNEL_DIR=$(KERNEL_DIR) clean
	$(MAKE) -C app clean

help:
	@echo "=== i.MX6ULL Smart Environment Monitor ==="
	@echo ""
	@echo "  make all            Build kernel modules + user apps"
	@echo "  make modules        Build kernel modules only"
	@echo "  make apps           Build user apps only"
	@echo "  make clean          Clean artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  KERNEL_DIR          Path to compiled kernel tree"
	@echo "  ARCH                Target architecture (default: arm)"
	@echo "  CROSS_COMPILE       Cross-compiler prefix"
	@echo ""
	@echo "Example:"
	@echo "  make KERNEL_DIR=/path/to/linux-imx"

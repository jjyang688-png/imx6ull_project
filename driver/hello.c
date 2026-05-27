#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jjyang688");
MODULE_DESCRIPTION("Skeleton module to verify cross-compilation toolchain");
MODULE_VERSION("0.1");

static int __init hello_init(void)
{
	pr_info("hello: module loaded successfully\n");
	return 0;
}

static void __exit hello_exit(void)
{
	pr_info("hello: module unloaded\n");
}
module_init(hello_init);
module_exit(hello_exit);

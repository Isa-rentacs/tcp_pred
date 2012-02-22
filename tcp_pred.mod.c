#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
 .name = KBUILD_MODNAME,
 .init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
 .exit = cleanup_module,
#endif
 .arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0xcef023b6, "module_layout" },
	{ 0x3ec8886f, "param_ops_int" },
	{ 0xb6544b40, "tcp_register_congestion_control" },
	{ 0xb86e4ab9, "random32" },
	{ 0x27e1a049, "printk" },
	{ 0x2fadaae5, "tcp_slow_start" },
	{ 0x44e4f3f5, "tcp_cong_avoid_ai" },
	{ 0x7d11c268, "jiffies" },
	{ 0x5c2520b4, "tcp_is_cwnd_limited" },
	{ 0x229a536d, "tcp_unregister_congestion_control" },
	{ 0xb4390f9a, "mcount" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";


MODULE_INFO(srcversion, "697BB067F8FC0A4FD98CAD8");

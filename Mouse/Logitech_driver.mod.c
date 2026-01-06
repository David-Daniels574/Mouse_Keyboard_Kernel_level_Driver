#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xbfc7bbd9, "usb_kill_urb" },
	{ 0x1dbcabb3, "input_unregister_device" },
	{ 0xbfc7bbd9, "usb_free_urb" },
	{ 0x2c488dee, "usb_free_coherent" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0x2375837d, "usb_submit_urb" },
	{ 0xb1d6f058, "_dev_err" },
	{ 0xe8213e80, "_printk" },
	{ 0x609fdaf1, "input_event" },
	{ 0x99c04b19, "usb_deregister" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xfc961df9, "kmalloc_caches" },
	{ 0xe18fddbc, "__kmalloc_cache_noprof" },
	{ 0x1fbd1421, "input_allocate_device" },
	{ 0x3452c72d, "usb_alloc_coherent" },
	{ 0x6888e5cf, "usb_alloc_urb" },
	{ 0x9479a1e8, "strnlen" },
	{ 0xd70733be, "sized_strscpy" },
	{ 0x43a349ca, "strlen" },
	{ 0xa53f4e29, "memcpy" },
	{ 0x40a621c5, "snprintf" },
	{ 0xafe73426, "usb_control_msg" },
	{ 0xcea1c269, "input_register_device" },
	{ 0x1dbcabb3, "input_free_device" },
	{ 0xb1d6f058, "_dev_warn" },
	{ 0xe54e0a6b, "__fortify_panic" },
	{ 0xd272d446, "__fentry__" },
	{ 0x647650ef, "usb_register_driver" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0xd268ca91, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xbfc7bbd9,
	0x1dbcabb3,
	0xbfc7bbd9,
	0x2c488dee,
	0xcb8b6ec6,
	0x2375837d,
	0xb1d6f058,
	0xe8213e80,
	0x609fdaf1,
	0x99c04b19,
	0xbd03ed67,
	0xfc961df9,
	0xe18fddbc,
	0x1fbd1421,
	0x3452c72d,
	0x6888e5cf,
	0x9479a1e8,
	0xd70733be,
	0x43a349ca,
	0xa53f4e29,
	0x40a621c5,
	0xafe73426,
	0xcea1c269,
	0x1dbcabb3,
	0xb1d6f058,
	0xe54e0a6b,
	0xd272d446,
	0x647650ef,
	0xd272d446,
	0xd268ca91,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"usb_kill_urb\0"
	"input_unregister_device\0"
	"usb_free_urb\0"
	"usb_free_coherent\0"
	"kfree\0"
	"usb_submit_urb\0"
	"_dev_err\0"
	"_printk\0"
	"input_event\0"
	"usb_deregister\0"
	"random_kmalloc_seed\0"
	"kmalloc_caches\0"
	"__kmalloc_cache_noprof\0"
	"input_allocate_device\0"
	"usb_alloc_coherent\0"
	"usb_alloc_urb\0"
	"strnlen\0"
	"sized_strscpy\0"
	"strlen\0"
	"memcpy\0"
	"snprintf\0"
	"usb_control_msg\0"
	"input_register_device\0"
	"input_free_device\0"
	"_dev_warn\0"
	"__fortify_panic\0"
	"__fentry__\0"
	"usb_register_driver\0"
	"__x86_return_thunk\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");

MODULE_ALIAS("usb:v046DpC077d*dc*dsc*dp*ic*isc*ip*in*");

MODULE_INFO(srcversion, "AAA0C3C2FB7CC222D7B4092");

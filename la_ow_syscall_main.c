#include <linux/module.h> /* Needed by all modules */
#include <linux/kernel.h> /* Needed for KERN_INFO */
#include <linux/init.h> /* Needed for the macros */
#include <linux/kprobes.h> /* Needed for kprobe calls */
#include "module_version.h"

///< The license type -- this affects runtime behavior
MODULE_LICENSE("GPL");

///< The author -- visible when you use modinfo
MODULE_AUTHOR("Miao Wang");

///< The description -- see modinfo
MODULE_DESCRIPTION("LoongArch old-world syscall compatibility module");

///< The version of the module
MODULE_VERSION(THIS_MODULE_VERSION);

#include <linux/kallsyms.h>
#include <linux/syscalls.h>

#include "systable.h"

#define __EXTERN
#include "fsstat.h"
#include "signal.h"

#ifndef __loongarch64
#error This Linux kernel module is only supported on LoongArch
#endif

static unsigned long kallsyms_lookup_name_addr = 0;
static unsigned int allow_mod_unreg = 0;

#include <asm-generic/sections.h>

// Taken from https://github.com/zizzu0/LinuxKernelModules/blob/main/FindKallsymsLookupName.c
#define KPROBE_PRE_HANDLER(fname) \
	static int __kprobes fname(struct kprobe *p, struct pt_regs *regs)

static struct kprobe kp0, kp1;

KPROBE_PRE_HANDLER(handler_pre0)
{
	kallsyms_lookup_name_addr = regs->csr_era;

	return 0;
}

KPROBE_PRE_HANDLER(handler_pre1)
{
	return 0;
}

#undef KPROBE_PRE_HANDLER

static int do_register_kprobe(struct kprobe *kp, char *symbol_name,
			      void *handler)
{
	int ret = 0;

	kp->symbol_name = symbol_name;
	kp->pre_handler = handler;

	ret = register_kprobe(kp);
	if (ret < 0) {
		pr_err("register_probe() for symbol %s failed, returned %d\n",
		       symbol_name, ret);
		return ret;
	}

	pr_debug("planted kprobe for symbol %s at %p\n", symbol_name, kp->addr);

	return ret;
}

static int __init kprobe_kallsyms_lookup_name(void)
{
	int ret = 0;

	ret = do_register_kprobe(&kp0, "kallsyms_lookup_name", handler_pre0);
	if (ret < 0)
		return ret;

	ret = do_register_kprobe(&kp1, "kallsyms_lookup_name", handler_pre1);
	if (ret < 0) {
		unregister_kprobe(&kp0);
		return ret;
	}

	unregister_kprobe(&kp0);
	unregister_kprobe(&kp1);

	return ret;
}


static int __init find_kallsyms_lookup_name(void)
{
	char fn_name[KSYM_SYMBOL_LEN];

	int ret = 0;

	if (kallsyms_lookup_name_addr == 0 ||
	    kallsyms_lookup_name_addr == (unsigned long)-1) {
		ret = kprobe_kallsyms_lookup_name();
		if ( ret < 0 ) {
			return ret;
		}
		if (kallsyms_lookup_name_addr == 0 ||
		    kallsyms_lookup_name_addr == (unsigned long)-1) {
			return -EINVAL;
		}
	}
	sprint_symbol(fn_name, kallsyms_lookup_name_addr);
	if (strncmp(fn_name, "kallsyms_lookup_name+0x0",
		    strlen("kallsyms_lookup_name+0x0")) == 0) {
		pr_debug("got kallsyms_lookup_name = %lx\n",
			 kallsyms_lookup_name_addr);
		return 0;
	} else {
		pr_debug("got %s at %lx, not kallsyms_lookup_name\n", fn_name,
			 kallsyms_lookup_name_addr);
		return -EINVAL;
	}
}

int (*p_vfs_fstatat)(int dfd, const char __user *filename, struct kstat *stat,
		     int flags);
int (*p_vfs_fstat)(int fd, struct kstat *stat);

void *p_sys_setxattr, *p_sys_close, *p_sys_clone;

static struct {
	const char *func_name;
	void **stor;
} relocation_table[] = {
#define __rel(func)                             \
	{                                       \
		(#func), ((void **)&(p_##func)) \
	}
	__rel(vfs_fstatat),	  __rel(vfs_fstat),
	__rel(sys_setxattr),	  __rel(sys_close),
	__rel(sys_clone),	  __rel(sys_rt_sigprocmask),
	__rel(sys_rt_sigpending), __rel(sys_rt_sigtimedwait),
	__rel(sys_rt_sigaction),  __rel(sys_rt_sigsuspend),
	__rel(sys_pselect6),	  __rel(sys_ppoll),
#ifdef CONFIG_SIGNALFD
	__rel(sys_signalfd4),
#endif
#ifdef CONFIG_EPOLL
	__rel(sys_epoll_pwait),	  __rel(sys_epoll_pwait2),
#endif
};
#define nr_rel_tab (sizeof(relocation_table) / sizeof(relocation_table[0]))

static void **p_sys_call_table;

#include <linux/jiffies.h>
#include <linux/reboot.h>
static int __init find_sys_call_table(void)
{
	unsigned long (*p_kallsyms_lookup_name)(const char *name) =
		(void *)kallsyms_lookup_name_addr;
	unsigned long *sys_table;

	if (kallsyms_lookup_name_addr == 0) {
		return -ENOSYS;
	}

	if ((sys_table = (unsigned long *)p_kallsyms_lookup_name(
		     "sys_call_table"))) {
		p_sys_call_table = (void **)sys_table;
		pr_debug("found sys_call_table=%px\n", p_sys_call_table);
		return 0;
	}

	pr_info("failed to find sys_call_table using kallsyms_lookup_name()\n");
	pr_info("trying to find sys_call_table using memory scanning\n");

	for (sys_table = (void *)&jiffies;
	     (void *)sys_table < (void *)&reboot_mode; sys_table++) {
		if (sys_table[__NR_setxattr] == (unsigned long)p_sys_setxattr &&
		    sys_table[__NR_close] == (unsigned long)p_sys_close &&
		    sys_table[__NR_clone] == (unsigned long)p_sys_clone) {
			p_sys_call_table = (void **)sys_table;
			pr_debug("found sys_call_table=%px\n",
				 p_sys_call_table);
			return 0;
		}
	}

	return -ENOSYS;
}

static int __init oldsyscall_start(void)
{
	unsigned long (*p_kallsyms_lookup_name)(const char *name);
	int rc = find_kallsyms_lookup_name();
	if (rc < 0) {
		return rc;
	}
	p_kallsyms_lookup_name = (void *)kallsyms_lookup_name_addr;

	for (int i = 0; i < nr_rel_tab; i++) {
		unsigned long p =
			p_kallsyms_lookup_name(relocation_table[i].func_name);
		if (p == 0) {
			pr_warn("cannot find symbol %s\n",
				relocation_table[i].func_name);
			return -EINVAL;
		}
		pr_debug("found symbol %s at %px\n",
			 relocation_table[i].func_name, (void *)p);
		*relocation_table[i].stor = (void *)p;
	}
	rc = find_sys_call_table();
	if (rc < 0) {
		return rc;
	}
	for (int i = 0; syscall_to_replace[i].syscall_num != -1; i++) {
		if (syscall_to_replace[i].symbol_addr) {
			continue;
		}
		const char *symbol_name =
			sys_call_table_name[syscall_to_replace[i].syscall_num];
		unsigned long symbol_addr = p_kallsyms_lookup_name(symbol_name);
		if (symbol_addr) {
			pr_debug("found %s at %px\n", symbol_name,
				 (void *)symbol_addr);
		} else {
			pr_warn("cannot find symbol %s\n", symbol_name);
			return -EINVAL;
		}
		syscall_to_replace[i].symbol_addr = (void *)symbol_addr;
	}
	if (!allow_mod_unreg) {
		bool succ = try_module_get(THIS_MODULE);
		if (!succ) {
			return -EINVAL;
		}
	}
	for (int i = 0; syscall_to_replace[i].syscall_num != -1; i++) {
		pr_debug("will replace syscall_%ld with %px, orig %px\n",
			 syscall_to_replace[i].syscall_num,
			 syscall_to_replace[i].symbol_addr,
			 p_sys_call_table[syscall_to_replace[i].syscall_num]);
		syscall_to_replace[i].orig =
			p_sys_call_table[syscall_to_replace[i].syscall_num];
		p_sys_call_table[syscall_to_replace[i].syscall_num] =
			syscall_to_replace[i].symbol_addr;
	}
	pr_info("la_ow_syscall module successfully initialized\n");
	return 0;
}

static void __exit oldsyscall_end(void)
{
	for (int i = 0; syscall_to_replace[i].syscall_num != -1; i++) {
		pr_debug("will restore syscall_%ld to %px\n",
			 syscall_to_replace[i].syscall_num,
			 syscall_to_replace[i].orig);
		p_sys_call_table[syscall_to_replace[i].syscall_num] =
			syscall_to_replace[i].orig;
	}
}

module_init(oldsyscall_start);
module_exit(oldsyscall_end);
module_param(allow_mod_unreg, uint, 0000);
MODULE_PARM_DESC(allow_mod_unreg,
		 "Allow this module to be unload (Danger! Debug use only)");
module_param(kallsyms_lookup_name_addr, ulong, 0000);
MODULE_PARM_DESC(kallsyms_lookup_name_addr, "Address for kallsyms_lookup_name, provide this when unable to find using kprobe");

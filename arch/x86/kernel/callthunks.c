// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "callthunks: " fmt

#include <linux/debugfs.h>
#include <linux/kallsyms.h>
#include <linux/memory.h>
#include <linux/moduleloader.h>
#include <linux/static_call.h>

#include <asm/alternative.h>
#include <asm/asm-offsets.h>
#include <asm/cpu.h>
#include <asm/ftrace.h>
#include <asm/insn.h>
#include <asm/kexec.h>
#include <asm/nospec-branch.h>
#include <asm/paravirt.h>
#include <asm/sections.h>
#include <asm/switch_to.h>
#include <asm/sync_core.h>
#include <asm/text-patching.h>
#include <asm/xen/hypercall.h>

static int __initdata_or_module debug_callthunks;

#define MAX_PATCH_LEN (255-1)

#define prdbg(fmt, args...)					\
do {								\
	if (debug_callthunks)					\
		printk(KERN_DEBUG pr_fmt(fmt), ##args);		\
} while(0)

static int __init debug_thunks(char *str)
{
	debug_callthunks = 1;
	return 1;
}
__setup("debug-callthunks", debug_thunks);

#ifdef CONFIG_CALL_THUNKS_DEBUG
DEFINE_PER_CPU(u64, __x86_call_count);
DEFINE_PER_CPU(u64, __x86_ret_count);
DEFINE_PER_CPU(u64, __x86_stuffs_count);
DEFINE_PER_CPU(u64, __x86_ctxsw_count);
EXPORT_PER_CPU_SYMBOL_GPL(__x86_ctxsw_count);
EXPORT_PER_CPU_SYMBOL_GPL(__x86_call_count);
#endif

extern s32 __call_sites[], __call_sites_end[];

struct core_text {
	unsigned long	base;
	unsigned long	end;
	const char	*name;
};

static bool thunks_initialized __ro_after_init;

static const struct core_text builtin_coretext = {
	.base = (unsigned long)_text,
	.end  = (unsigned long)_etext,
	.name = "builtin",
};

asm (
	".pushsection .rodata				\n"
	".global skl_call_thunk_template		\n"
	"skl_call_thunk_template:			\n"
		__stringify(INCREMENT_CALL_DEPTH)"	\n"
	".global skl_call_thunk_tail			\n"
	"skl_call_thunk_tail:				\n"
	".popsection					\n"
);

extern u8 skl_call_thunk_template[];
extern u8 skl_call_thunk_tail[];

#define SKL_TMPL_SIZE \
	((unsigned int)(skl_call_thunk_tail - skl_call_thunk_template))

extern void error_entry(void);
extern void xen_error_entry(void);
extern void paranoid_entry(void);

static inline bool within_coretext(const struct core_text *ct, void *addr)
{
	unsigned long p = (unsigned long)addr;

	return ct->base <= p && p < ct->end;
}

static inline bool within_module_coretext(void *addr)
{
	bool ret = false;

#ifdef CONFIG_MODULES
	struct module *mod;

	guard(rcu)();
	mod = __module_address((unsigned long)addr);
	if (mod && within_module_core((unsigned long)addr, mod))
		ret = true;
#endif
	return ret;
}

static bool is_coretext(const struct core_text *ct, void *addr)
{
	if (ct && within_coretext(ct, addr))
		return true;
	if (within_coretext(&builtin_coretext, addr))
		return true;
	return within_module_coretext(addr);
}

static bool skip_addr(void *dest)
{
	if (dest == error_entry)
		return true;
	if (dest == paranoid_entry)
		return true;
	if (dest == xen_error_entry)
		return true;
	/* Does FILL_RSB... */
	if (dest == __switch_to_asm)
		return true;
	/* Accounts directly */
	if (dest == ret_from_fork)
		return true;
#if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_AMD_MEM_ENCRYPT)
	if (dest == soft_restart_cpu)
		return true;
#endif
#ifdef CONFIG_FUNCTION_TRACER
	if (dest == __fentry__)
		return true;
#endif
#ifdef CONFIG_KEXEC_CORE
# ifdef CONFIG_X86_64
	if (dest >= (void *)__relocate_kernel_start &&
	    dest < (void *)__relocate_kernel_end)
		return true;
# else
	if (dest >= (void *)relocate_kernel &&
	    dest < (void*)relocate_kernel + KEXEC_CONTROL_CODE_MAX_SIZE)
		return true;
# endif
#endif
	return false;
}

static __init_or_module void *call_get_dest(void *addr)
{
	struct insn insn;
	void *dest;
	int ret;

	ret = insn_decode_kernel(&insn, addr);
	if (ret)
		return ERR_PTR(ret);

	/* Patched out call? */
	if (insn.opcode.bytes[0] != CALL_INSN_OPCODE)
		return NULL;

	dest = addr + insn.length + insn.immediate.value;
	if (skip_addr(dest))
		return NULL;
	return dest;
}

static const u8 nops[] = {
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
};

static void *patch_dest(void *dest, bool direct)
{
	unsigned int tsize = SKL_TMPL_SIZE;
	u8 insn_buff[MAX_PATCH_LEN];
	u8 *pad = dest - tsize;

	memcpy(insn_buff, skl_call_thunk_template, tsize);
	text_poke_apply_relocation(insn_buff, pad, tsize, skl_call_thunk_template, tsize);

	/* Already patched? */
	if (!bcmp(pad, insn_buff, tsize))
		return pad;

	/* Ensure there are nops */
	if (bcmp(pad, nops, tsize)) {
		pr_warn_once("Invalid padding area for %pS\n", dest);
		return NULL;
	}

	if (direct)
		memcpy(pad, insn_buff, tsize);
	else
		text_poke_copy_locked(pad, insn_buff, tsize, true);
	return pad;
}

static __init_or_module void patch_call(void *addr, const struct core_text *ct)
{
	void *pad, *dest;
	u8 bytes[8];

	if (!within_coretext(ct, addr))
		return;

	dest = call_get_dest(addr);
	if (!dest || WARN_ON_ONCE(IS_ERR(dest)))
		return;

	if (!is_coretext(ct, dest))
		return;

	pad = patch_dest(dest, within_coretext(ct, dest));
	if (!pad)
		return;

	prdbg("Patch call at: %pS %px to %pS %px -> %px \n", addr, addr,
		dest, dest, pad);
	__text_gen_insn(bytes, CALL_INSN_OPCODE, addr, pad, CALL_INSN_SIZE);
	text_poke_early(addr, bytes, CALL_INSN_SIZE);
}

static __init_or_module void
patch_call_sites(s32 *start, s32 *end, const struct core_text *ct)
{
	s32 *s;

	for (s = start; s < end; s++)
		patch_call((void *)s + *s, ct);
}

static __init_or_module void
callthunks_setup(struct callthunk_sites *cs, const struct core_text *ct)
{
	prdbg("Patching call sites %s\n", ct->name);
	patch_call_sites(cs->call_start, cs->call_end, ct);
	prdbg("Patching call sites done%s\n", ct->name);
}

void __init callthunks_patch_builtin_calls(void)
{
	struct callthunk_sites cs = {
		.call_start	= __call_sites,
		.call_end	= __call_sites_end,
	};

	if (!cpu_feature_enabled(X86_FEATURE_CALL_DEPTH))
		return;

	pr_info("Setting up call depth tracking\n");
	mutex_lock(&text_mutex);
	callthunks_setup(&cs, &builtin_coretext);
	thunks_initialized = true;
	mutex_unlock(&text_mutex);
}

void *callthunks_translate_call_dest(void *dest)
{
	void *target;

	lockdep_assert_held(&text_mutex);

	if (!thunks_initialized || skip_addr(dest))
		return dest;

	if (!is_coretext(NULL, dest))
		return dest;

	target = patch_dest(dest, false);
	return target ? : dest;
}

#ifdef CONFIG_BPF_JIT
static bool is_callthunk(void *addr)
{
	unsigned int tmpl_size = SKL_TMPL_SIZE;
	u8 insn_buff[MAX_PATCH_LEN];
	unsigned long dest;
	u8 *pad;

	dest = roundup((unsigned long)addr, CONFIG_FUNCTION_ALIGNMENT);
	if (!thunks_initialized || skip_addr((void *)dest))
		return false;

	pad = (void *)(dest - tmpl_size);

	memcpy(insn_buff, skl_call_thunk_template, tmpl_size);
	text_poke_apply_relocation(insn_buff, pad, tmpl_size, skl_call_thunk_template, tmpl_size);

	return !bcmp(pad, insn_buff, tmpl_size);
}

int x86_call_depth_emit_accounting(u8 **pprog, void *func, void *ip)
{
	unsigned int tmpl_size = SKL_TMPL_SIZE;
	u8 insn_buff[MAX_PATCH_LEN];

	if (!thunks_initialized)
		return 0;

	/* Is function call target a thunk? */
	if (func && is_callthunk(func))
		return 0;

	memcpy(insn_buff, skl_call_thunk_template, tmpl_size);
	text_poke_apply_relocation(insn_buff, ip, tmpl_size, skl_call_thunk_template, tmpl_size);

	memcpy(*pprog, insn_buff, tmpl_size);
	*pprog += tmpl_size;
	return tmpl_size;
}
#endif

#ifdef CONFIG_MODULES
void noinline callthunks_patch_module_calls(struct callthunk_sites *cs,
					    struct module *mod)
{
	struct core_text ct = {
		.base = (unsigned long)mod->mem[MOD_TEXT].base,
		.end  = (unsigned long)mod->mem[MOD_TEXT].base + mod->mem[MOD_TEXT].size,
		.name = mod->name,
	};

	if (!thunks_initialized)
		return;

	mutex_lock(&text_mutex);
	callthunks_setup(cs, &ct);
	mutex_unlock(&text_mutex);
}
#endif /* CONFIG_MODULES */

#if defined(CONFIG_CALL_THUNKS_DEBUG) && defined(CONFIG_DEBUG_FS)
static int callthunks_debug_show(struct seq_file *m, void *p)
{
	unsigned long cpu = (unsigned long)m->private;

	seq_printf(m, "C: %16llu R: %16llu S: %16llu X: %16llu\n,",
		   per_cpu(__x86_call_count, cpu),
		   per_cpu(__x86_ret_count, cpu),
		   per_cpu(__x86_stuffs_count, cpu),
		   per_cpu(__x86_ctxsw_count, cpu));
	return 0;
}

static int callthunks_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, callthunks_debug_show, inode->i_private);
}

static const struct file_operations dfs_ops = {
	.open		= callthunks_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init callthunks_debugfs_init(void)
{
	struct dentry *dir;
	unsigned long cpu;

	dir = debugfs_create_dir("callthunks", NULL);
	for_each_possible_cpu(cpu) {
		void *arg = (void *)cpu;
		char name [10];

		sprintf(name, "cpu%lu", cpu);
		debugfs_create_file(name, 0644, dir, arg, &dfs_ops);
	}
	return 0;
}
__initcall(callthunks_debugfs_init);
#endif

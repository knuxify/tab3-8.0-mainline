#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define DEBUG_BUF_MAX 0x1000
#define TZBSP_CPU_COUNT 0x02
#define TZBSP_DIAG_NUM_OF_VMID 16
#define TZBSP_DIAG_VMID_DESC_LEN 7
#define TZBSP_DIAG_INT_NUM 32
#define TZBSP_MAX_INT_DESC 16

struct vmid_info {
	u8 vmid;
	u8 desc[TZBSP_DIAG_VMID_DESC_LEN];
};

struct boot_info {
	u32 warmboot_entry_count;
	u32 warmboot_exit_count;
	u32 pc_entry_count;
	u32 pc_exit_count;
	u32 warmboot_jmp_addr;
	u32 unused;
};

struct reset_info {
	u32 type;
	u32 count;
};

struct interrupt_info {
	u16 type;
	u8 slot_availble;
	u8 unused;
	u32 number;
	u8 desc[TZBSP_MAX_INT_DESC];
	u64 count[TZBSP_CPU_COUNT];
};

struct tzdbg {
	u32 magic;
	u32 version;
	u32 cpu_count;
	u32 vmid_table_offset;
	u32 boot_table_offset;
	u32 reset_table_offset;
	u32 int_table_offset;
	u32 ring_buffer_offset;
	u32 ring_len;

	struct vmid_info vmid_info[TZBSP_DIAG_NUM_OF_VMID];
	struct boot_info boot_info[TZBSP_CPU_COUNT];
	struct reset_info reset_info[TZBSP_CPU_COUNT];
	u32 num_interrupts;
	struct interrupt_info interrupt_info[TZBSP_DIAG_INT_NUM];
};

enum tzdbg_stats_type {
	TZDBG_BOOT = 0,
	TZDBG_RESET,
	TZDBG_INTERRUPT,
	TZDBG_VMID,
	TZDBG_GENERAL,
	TZDBG_LOG,
	TZDBG_QSEE_LOG,
	TZDBG_HYP_GENERAL,
	TZDBG_HYP_LOG,
	TZDBG_STATS_MAX
};

struct tzdbg_debugfs_data {
	void __iomem *base;
	enum tzdbg_stats_type type;
};

static const char *files[] = {
	"boot", "reset", "interrupt", "vmid", "general", "log",
};

#define TZDBG_SHOW(name, limit, loop_body)                                   \
	static void tzdbg_##name##_show(struct seq_file *s, struct tzdbg *d) \
	{                                                                    \
		struct name##_info name;                                     \
		int i;                                                       \
                                                                             \
		for (i = 0; i < limit; i++) {                                \
			name = d->name##_info[i];                            \
			loop_body                                            \
		}                                                            \
	}

#define TZDBG_FN(id, name)                  \
	case TZDBG_##id:                    \
		tzdbg_##name##_show(s, &d); \
		break;

TZDBG_SHOW(boot, d->cpu_count, {
	seq_printf(s, "CPU%d:\n", i);
	seq_printf(s, "\tWarmboot enter count:       %d\n",
		   boot.warmboot_entry_count);
	seq_printf(s, "\tWarmboot exit count:        %d\n",
		   boot.warmboot_exit_count);
	seq_printf(s, "\tWarmboot jump address:      0x%x\n",
		   boot.warmboot_jmp_addr);
	seq_printf(s, "\tPower Collapse enter count: %d\n",
		   boot.pc_entry_count);
	seq_printf(s, "\tPower Collapse exit count:  %d\n", boot.pc_exit_count);
});

TZDBG_SHOW(reset, d->cpu_count, {
	seq_printf(s, "CPU%d:\n", i);
	seq_printf(s, "\tReset type:  0x%x\n", reset.type);
	seq_printf(s, "\tReset count: %d\n", reset.count);
});

TZDBG_SHOW(interrupt, d->num_interrupts, {
	seq_printf(s, "Interrupt #%d\n", i);
	seq_printf(s, "\tType:        0x%x\n", interrupt.type);
	seq_printf(s, "\tSecure:      %s\n",
		   interrupt.slot_availble ? "no" : "yes");
	seq_printf(s, "\tNumber:      %d\n", interrupt.number);
	seq_printf(s, "\tDescription: %s\n", interrupt.desc);
});

static void tzdbg_general_show(struct seq_file *s, struct tzdbg *d)
{
	seq_printf(s, "Version:   0x%x\n", d->version);
	seq_printf(s, "Magic:     0x%x\n", d->magic);
	seq_printf(s, "CPU count: %d\n", d->cpu_count);
}

TZDBG_SHOW(vmid, TZBSP_DIAG_NUM_OF_VMID, {
	/* vmid with 0xFF means unused */
	if (vmid.vmid < 0xFF) {
		seq_printf(s, "VMID #%d:\n", i);
		seq_printf(s, "\tNumber:      %d\n", vmid.vmid);
		seq_printf(s, "\tDescription: %s\n", vmid.desc);
	}
});

static void tzdbg_log_show(struct seq_file *s, u8 __iomem *b)
{
	/* sanity check: resource size in downstream is 0x1000
	 * however original driver doesn't check buffer size
	 * at some point it may out of bounds
	 * so we'll read not remapped memory
	 *
	 * TODO: store resource size if there's different one
	 */
	u32 max = DEBUG_BUF_MAX - sizeof(struct tzdbg);
	char *buf = kzalloc(max, GFP_KERNEL);

	snprintf(buf, max, "%s", b);
	seq_printf(s, "%s\n", buf);

	kfree(buf);
}

static int tzdbg_show(struct seq_file *s, void *unused)
{
	struct tzdbg_debugfs_data *info = s->private;
	struct tzdbg d;

	memcpy_fromio(&d, info->base, sizeof(d));

	switch (info->type) {
		TZDBG_FN(BOOT, boot)
		TZDBG_FN(RESET, reset)
		TZDBG_FN(INTERRUPT, interrupt)
		TZDBG_FN(GENERAL, general);
		TZDBG_FN(VMID, vmid);
	case TZDBG_LOG:
		tzdbg_log_show(s, info->base + d.ring_buffer_offset);
		break;
	default:
		break;
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tzdbg);

static int qcom_tzdbg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *r;
	struct dentry *root, *dent;
	struct tzdbg_debugfs_data *data;
	void __iomem *base;
	u32 real_base, i;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(dev, r);
	if (IS_ERR(base))
		return PTR_ERR(base);

	BUG_ON(resource_size(r) < sizeof(struct tzdbg));

	root = debugfs_create_dir("qcom_tzdbg", NULL);
	if (!root)
		return dev_err_probe(dev, -ENODEV,
				     "Failed to create debugfs directory");
	platform_set_drvdata(pdev, root);

	/* The actual data lives here */
	real_base = readl_relaxed(base);
	base = devm_ioremap(dev, real_base, resource_size(r));
	if (!base)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to ioremap real base address\n");

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
		if (!data) {
			debugfs_remove_recursive(root);
			return dev_err_probe(
				dev, -ENOMEM,
				"Failed to allocate memory for %s\n", files[i]);
		}

		data->base = base;
		data->type = i;
		dent = debugfs_create_file(files[i], S_IRUGO, root, data,
					   &tzdbg_fops);
		if (IS_ERR(dent)) {
			debugfs_remove_recursive(root);
			return dev_err_probe(
				dev, PTR_ERR(dent),
				"Failed to create debugfs file %s\n", files[i]);
		}
	}

	return 0;
}

static void qcom_tzdbg_remove(struct platform_device *pdev)
{
	struct dentry *root = platform_get_drvdata(pdev);

	debugfs_remove_recursive(root);
}

static const struct of_device_id qcom_tzdbg_dt_match[] = {
	{ .compatible = "qcom,tz-dbg" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_tzdbg_dt_match);

static struct platform_driver qcom_tzdbg_driver = {
	.probe		= qcom_tzdbg_probe,
	.remove		= qcom_tzdbg_remove,
	.driver		= {
		.name = "qcom_tzdbg",
		.owner = THIS_MODULE,
		.of_match_table = qcom_tzdbg_dt_match,
	},
};
static int __init qcom_tzdbg_init(void)
{
	return platform_driver_register(&qcom_tzdbg_driver);
}
subsys_initcall(qcom_tzdbg_init);

MODULE_DESCRIPTION("Qualcomm TrustZone Debug driver");
MODULE_LICENSE("GPL v2");

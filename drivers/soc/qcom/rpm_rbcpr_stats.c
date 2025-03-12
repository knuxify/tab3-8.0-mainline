#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

static const char *corner_names[] = {
	"svs",
	"nominal",
	"turbo",
};

struct rbcpr_recmnd {
	u32 voltage;
	u32 timestamp;
};

struct rbcpr_corners {
	int efuse_adjustment;
	struct rbcpr_recmnd recommends[3];
	u32 programmed_voltage;
	u32 isr_counter;
	u32 min_counter;
	u32 max_counter;
};

struct rbcpr_stats {
	u32 status_count;
	u32 num_corners;
	u32 num_latest_recommends;
	struct rbcpr_corners rbcpr_corners[3];
	u32 current_corner;
	u32 railway_voltage;
	u32 enable;
};

struct rbcpr_corner_file_data {
	u8 index;
	u8 num_recommends;
	void __iomem *corner_base;
};

static int rbcpr_corner_stats_show(struct seq_file *s, void *unused)
{
	struct rbcpr_corner_file_data *d = s->private;
	struct rbcpr_corners cstat;
	int i;

	memcpy_fromio(&cstat, d->corner_base, sizeof(cstat));

	seq_printf(s, "Corner %s:\n", corner_names[d->index]);

	seq_printf(s, "\tEFUSE adjustment: %d\n", cstat.efuse_adjustment);
	seq_printf(s, "\tVoltage history:\n");

	for (i = 0; i < d->num_recommends; i++) {
		seq_printf(s, "\t\tTimestamp: %d\n",
			   cstat.recommends[i].timestamp);
		seq_printf(s, "\t\tVoltage: %d\n", cstat.recommends[i].voltage);
	}

	seq_printf(s, "\tProgrammed voltage: %d\n", cstat.programmed_voltage);
	seq_printf(s, "\tISR counter: %d\n", cstat.isr_counter);
	seq_printf(s, "\tmin counter: %d\n", cstat.min_counter);
	seq_printf(s, "\tmax counter: %d\n", cstat.max_counter);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rbcpr_corner_stats);

static int rbcpr_stats_show(struct seq_file *s, void *unused)
{
	void __iomem *base = s->private;
	struct rbcpr_stats stat;

	memcpy_fromio(&stat, base, sizeof(stat));

	seq_printf(s, "Status count: %d\n", stat.status_count);
	if (ARRAY_SIZE(corner_names) > stat.current_corner)
		seq_printf(s, "Current corner: %s\n",
			   corner_names[stat.current_corner]);
	else
		seq_printf(s, "Current corner: %d\n", stat.current_corner);
	seq_printf(s, "Railway voltage: %d\n", stat.railway_voltage);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rbcpr_stats);

static int rbcpr_stats_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rbcpr_corner_file_data *data;
	struct device_node *msgram_np;
	struct dentry *dent, *root;
	struct resource res;
	const char *name;
	char buf[20];
	void __iomem *base;
	int ret, i;
	u32 num_corners, num_recommends;
	bool enabled;

	msgram_np = of_parse_phandle(dev->of_node, "qcom,rpm-msg-ram", 0);
	if (!msgram_np)
		return dev_err_probe(dev, -ENODEV,
				     "Couldn't parse MSG RAM phandle");

	ret = of_address_to_resource(msgram_np, 0, &res);
	of_node_put(msgram_np);
	if (ret < 0)
		return ret;

	base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!base)
		return dev_err_probe(dev, -EINVAL,
				     "Could not map the MSG RAM slice");

	enabled = readl_relaxed(base + offsetof(struct rbcpr_stats, enable)) !=
		  0;

	if (!enabled)
		return dev_err_probe(dev, -EINVAL,
				     "RBCPR stats are not enabled at RPM\n");

	num_corners =
		readl_relaxed(base + offsetof(struct rbcpr_stats, num_corners));
	num_recommends = readl_relaxed(
		base + offsetof(struct rbcpr_stats, num_latest_recommends));

	if (ARRAY_SIZE(corner_names) < num_corners)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid number of corners (%d)",
				     num_corners);

	root = debugfs_create_dir("qcom_rpm_rbcpr_stats", NULL);
	if (IS_ERR(dent))
		return dev_err_probe(dev, PTR_ERR(dent),
				     "Failed to create debugfs file\n");
	platform_set_drvdata(pdev, root);

	for (i = 0; i < num_corners; i++) {
		name = corner_names[i];
		snprintf(buf, 20, "corner_%s", name);

		data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);

		if (!data) {
			debugfs_remove_recursive(root);
			return dev_err_probe(dev, -ENOMEM,
					     "Failed to allocate memory for %s",
					     buf);
		}

		data->index = i;
		data->num_recommends = num_recommends;
		data->corner_base =
			base + offsetof(struct rbcpr_stats, rbcpr_corners) +
			(sizeof(struct rbcpr_corners) * i);

		dent = debugfs_create_file(buf, 0400, root, data,
					   &rbcpr_corner_stats_fops);
		if (IS_ERR(dent))
			goto err;
	}

	dent = debugfs_create_file("general", 0400, root, base,
				   &rbcpr_stats_fops);
	if (IS_ERR(dent))
		goto err;

	return 0;
err:
	debugfs_remove_recursive(root);
	return dev_err_probe(dev, PTR_ERR(dent),
			     "Failed to create debugfs file\n");
}

static void rbcpr_stats_remove(struct platform_device *pdev)
{
	struct dentry *root = platform_get_drvdata(pdev);

	debugfs_remove_recursive(root);
}

static const struct of_device_id rpm_rbcpr_table[] = {
	{ .compatible = "qcom,rpm-rbcpr-stats" },
	{},
};
MODULE_DEVICE_TABLE(of, rpm_rbcpr_table);

static struct platform_driver rbcpr_stats_driver = {
	.probe = rbcpr_stats_probe,
	.remove = rbcpr_stats_remove,
	.driver = {
		.name = "qcom_rpm_rbcpr_stats",
		.of_match_table = rpm_rbcpr_table,
	},
};
module_platform_driver(rbcpr_stats_driver);

MODULE_DESCRIPTION(
	"Qualcomm RPM RapidBridge Core Power Reduction Statistics driver");
MODULE_LICENSE("GPL v2");

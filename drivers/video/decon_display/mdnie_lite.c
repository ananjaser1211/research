/* linux/drivers/video/mdnie.c
 *
 * Register interface file for Samsung mDNIe driver
 *
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/lcd.h>
#include <linux/fb.h>
#include <linux/pm_runtime.h>

#include "mdnie.h"

#if defined(CONFIG_DECON_LCD_S6E3FA2)
#include "mdnie_lite_table_k.h"
#endif
#if defined(CONFIG_TDMB)
#include "mdnie_lite_table_dmb.h"
#endif

#define MDNIE_SYSFS_PREFIX		"/sdcard/mdnie/"
#define PANEL_COORDINATE_PATH	"/sys/class/lcd/panel/color_coordinate"

#define IS_DMB(idx)				(idx == DMB_NORMAL_MODE)
#define IS_SCENARIO(idx)		(idx < SCENARIO_MAX && !(idx > VIDEO_NORMAL_MODE && idx < CAMERA_MODE))
#define IS_ACCESSIBILITY(idx)	(idx && idx < ACCESSIBILITY_MAX)
#define IS_HBM(idx)				(idx && idx < HBM_MAX)

#define SCENARIO_IS_VALID(idx)	(IS_DMB(idx) || IS_SCENARIO(idx))

/* Split 16 bit as 8bit x 2 */
#define GET_MSB_8BIT(x)		((x >> 8) & (BIT(8) - 1))
#define GET_LSB_8BIT(x)		((x >> 0) & (BIT(8) - 1))

static struct class *mdnie_class;

/* Do not call mdnie write directly */
static int mdnie_write(struct mdnie_info *mdnie, struct mdnie_table *table)
{
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(table->tune); i++) {
		if (mdnie->enable)
			ret = mdnie->ops.write(mdnie->data, table->tune[i].sequence, table->tune[i].size);
	}

	return ret;
}

static int mdnie_write_table(struct mdnie_info *mdnie, struct mdnie_table *table)
{
	int i, ret = 0;
	struct mdnie_table *buf = NULL;

	for (i = 0; i < MDNIE_CMD_MAX; i++) {
		if (IS_ERR_OR_NULL(table->tune[i].sequence)) {
			dev_err(mdnie->dev, "mdnie sequence %s is null, %x\n", table->name, (u32)table->tune[i].sequence);
			return -EPERM;
		}
	}

	mutex_lock(&mdnie->dev_lock);

	buf = table;

	ret = mdnie_write(mdnie, buf);

	mutex_unlock(&mdnie->dev_lock);

	return ret;
}

static struct mdnie_table *mdnie_find_table(struct mdnie_info *mdnie)
{
	struct mdnie_table *table = NULL;

	mutex_lock(&mdnie->lock);

	if (IS_ACCESSIBILITY(mdnie->accessibility)) {
		table = &accessibility_table[mdnie->accessibility];
		goto exit;
	} else if (IS_HBM(mdnie->hbm)) {
		table = &hbm_table[mdnie->hbm];
		goto exit;
#if defined(CONFIG_TDMB)
	} else if (IS_DMB(mdnie->scenario)) {
		table = &dmb_table[mdnie->mode];
		goto exit;
#endif
	} else if (IS_SCENARIO(mdnie->scenario)) {
		table = &tuning_table[mdnie->scenario][mdnie->mode];
		goto exit;
	}

exit:
	mutex_unlock(&mdnie->lock);

	return table;
}

static void mdnie_update_sequence(struct mdnie_info *mdnie, struct mdnie_table *table)
{
	struct mdnie_table *t = NULL;

	if (mdnie->tuning) {
		t = mdnie_request_table(mdnie->path, table);
		if (!IS_ERR_OR_NULL(t) && !IS_ERR_OR_NULL(t->name))
			mdnie_write_table(mdnie, t);
		else
			mdnie_write_table(mdnie, table);
	} else
		mdnie_write_table(mdnie, table);
}

static void mdnie_update(struct mdnie_info *mdnie)
{
	struct mdnie_table *table = NULL;

	if (!mdnie->enable) {
		dev_err(mdnie->dev, "mdnie state is off\n");
		return;
	}

	table = mdnie_find_table(mdnie);
	if (!IS_ERR_OR_NULL(table) && !IS_ERR_OR_NULL(table->name)) {
		mdnie_update_sequence(mdnie, table);
		dev_info(mdnie->dev, "%s\n", table->name);

		mdnie->wrgb_current.r = table->tune[MDNIE_CMD1].sequence[MDNIE_WHITE_R];
		mdnie->wrgb_current.g = table->tune[MDNIE_CMD1].sequence[MDNIE_WHITE_G];
		mdnie->wrgb_current.b = table->tune[MDNIE_CMD1].sequence[MDNIE_WHITE_B];
	}
}

static void update_color_position(struct mdnie_info *mdnie, unsigned int idx)
{
	u8 mode, scenario;
	mdnie_t *wbuf;

	dev_info(mdnie->dev, "%s: idx=%d\n", __func__, idx);

	mutex_lock(&mdnie->lock);

	for (mode = 0; mode < MODE_MAX; mode++) {
		for (scenario = 0; scenario <= EMAIL_MODE; scenario++) {
			wbuf = tuning_table[scenario][mode].tune[MDNIE_CMD1].sequence;
			if (IS_ERR_OR_NULL(wbuf))
				continue;
			if ((wbuf[MDNIE_WHITE_R] == 0xff) && (wbuf[MDNIE_WHITE_G] == 0xff) && (wbuf[MDNIE_WHITE_B] == 0xff)) {
				wbuf[MDNIE_WHITE_R] = coordinate_data[idx][0];
				wbuf[MDNIE_WHITE_G] = coordinate_data[idx][1];
				wbuf[MDNIE_WHITE_B] = coordinate_data[idx][2];
			}
		}
	}

	mutex_unlock(&mdnie->lock);
}

static int get_panel_coordinate(struct mdnie_info *mdnie, int *result)
{
	int ret = 0;
	char *fp = NULL;
	int x, y;

	ret = mdnie_open_file(PANEL_COORDINATE_PATH, &fp);
	if (IS_ERR_OR_NULL(fp) || ret <= 0) {
		dev_info(mdnie->dev, "%s: open skip: %s, %d\n", __func__, PANEL_COORDINATE_PATH, ret);
		ret = -EINVAL;
		goto skip_color_correction;
	}

	ret = sscanf(fp, "%8d, %8d", &x, &y);
	if ((ret != 2) || !(x || y)) {
		dev_info(mdnie->dev, "%s: %d, %d\n", __func__, x, y);
		ret = -EINVAL;
		goto skip_color_correction;
	}

	result[COLOR_OFFSET_FUNC_F1] = COLOR_OFFSET_F1(x, y);
	result[COLOR_OFFSET_FUNC_F2] = COLOR_OFFSET_F2(x, y);
	result[COLOR_OFFSET_FUNC_F3] = COLOR_OFFSET_F3(x, y);
	result[COLOR_OFFSET_FUNC_F4] = COLOR_OFFSET_F4(x, y);

	ret = mdnie_calibration(result);
	dev_info(mdnie->dev, "%s: %d, %d, %d\n", __func__, x, y, ret);

skip_color_correction:
	mdnie->color_correction = 1;
	if (!IS_ERR_OR_NULL(fp))
		kfree(fp);

	return ret;
}

static ssize_t mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->mode);
}

static ssize_t mode_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value = 0;
	int ret, idx, result[COLOR_OFFSET_FUNC_MAX] = {0,};

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s: %d\n", __func__, value);

	if (value >= MODE_MAX)
		return -EINVAL;

	mutex_lock(&mdnie->lock);
	mdnie->mode = value;
	mutex_unlock(&mdnie->lock);

	if (!mdnie->color_correction) {
		idx = get_panel_coordinate(mdnie, result);
		if (idx > 0)
			update_color_position(mdnie, idx);
	}

	mdnie_update(mdnie);

	return count;
}


static ssize_t scenario_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->scenario);
}

static ssize_t scenario_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s: %d\n", __func__, value);

	if (!SCENARIO_IS_VALID(value))
		value = UI_MODE;

	mutex_lock(&mdnie->lock);
	mdnie->scenario = value;
	mutex_unlock(&mdnie->lock);

	mdnie_update(mdnie);

	return count;
}

static ssize_t tuning_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	char *pos = buf;
	struct mdnie_table *table = NULL;
	int i;

	pos += sprintf(pos, "++ %s: %s\n", __func__, mdnie->path);

	if (!mdnie->tuning) {
		pos += sprintf(pos, "tunning mode is off\n");
		goto exit;
	}

	if (strncmp(mdnie->path, MDNIE_SYSFS_PREFIX, sizeof(MDNIE_SYSFS_PREFIX) - 1)) {
		pos += sprintf(pos, "file path is invalid, %s\n", mdnie->path);
		goto exit;
	}

	table = mdnie_find_table(mdnie);
	if (!IS_ERR_OR_NULL(table) && !IS_ERR_OR_NULL(table->name)) {
		table = mdnie_request_table(mdnie->path, table);
		for (i = 0; i < table->tune[MDNIE_CMD1].size; i++)
			pos += sprintf(pos, "0x%02x ", table->tune[MDNIE_CMD1].sequence[i]);
		pos += sprintf(pos, "\n");
		if (MDNIE_CMD1 != MDNIE_CMD2) {
			for (i = 0; i < table->tune[MDNIE_CMD2].size; i++)
				pos += sprintf(pos, "0x%02x ", table->tune[MDNIE_CMD2].sequence[i]);
		}
	}

exit:
	pos += sprintf(pos, "-- %s\n", __func__);

	return pos - buf;
}

static ssize_t tuning_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	int ret;

	if (sysfs_streq(buf, "0") || sysfs_streq(buf, "1")) {
		ret = kstrtouint(buf, 0, &mdnie->tuning);
		if (ret < 0)
			return ret;
		if (!mdnie->tuning)
			memset(mdnie->path, 0, sizeof(mdnie->path));

		dev_info(dev, "%s: %s\n", __func__, mdnie->tuning ? "enable" : "disable");
	} else {
		if (!mdnie->tuning)
			return count;

		if (count > (sizeof(mdnie->path) - sizeof(MDNIE_SYSFS_PREFIX))) {
			dev_err(dev, "file name %s is too long\n", mdnie->path);
			return -ENOMEM;
		}

		memset(mdnie->path, 0, sizeof(mdnie->path));
		snprintf(mdnie->path, sizeof(MDNIE_SYSFS_PREFIX) + count-1, "%s%s", MDNIE_SYSFS_PREFIX, buf);
		dev_info(dev, "%s: %s\n", __func__, mdnie->path);

		mdnie_update(mdnie);
	}

	return count;
}

static ssize_t accessibility_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->accessibility);
}

static ssize_t accessibility_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value, s[9] = {0, }, i = 0;
	int ret;
	mdnie_t *wbuf;

	ret = sscanf(buf, "%8d %8x %8x %8x %8x %8x %8x %8x %8x %8x",
		&value, &s[0], &s[1], &s[2], &s[3],
		&s[4], &s[5], &s[6], &s[7], &s[8]);

	if (ret < 0)
		return ret;

	dev_info(dev, "%s: %d, %d\n", __func__, value, ret);

	if (value >= ACCESSIBILITY_MAX)
		return -EINVAL;

	mutex_lock(&mdnie->lock);
	mdnie->accessibility = value;
	if (value == COLOR_BLIND) {
		if (ret > ARRAY_SIZE(s) + 1) {
			mutex_unlock(&mdnie->lock);
			return -EINVAL;
		}
		wbuf = &accessibility_table[COLOR_BLIND].tune[MDNIE_CMD1].sequence[MDNIE_COLOR_BLIND_OFFSET];
		while (i < ret - 1) {
			wbuf[i * 2 + 0] = GET_LSB_8BIT(s[i]);
			wbuf[i * 2 + 1] = GET_MSB_8BIT(s[i]);
			i++;
		}

		dev_info(dev, "%s: %s\n", __func__, buf);
	}
	mutex_unlock(&mdnie->lock);

	mdnie_update(mdnie);

	return count;
}

static ssize_t color_correct_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	char *pos = buf;
	int i, idx, result[COLOR_OFFSET_FUNC_MAX] = {0,};

	if (!mdnie->color_correction)
		return -EINVAL;

	idx = get_panel_coordinate(mdnie, result);

	for (i = COLOR_OFFSET_FUNC_F1; i < COLOR_OFFSET_FUNC_MAX; i++)
		pos += sprintf(pos, "f%d: %d, ", i, result[i]);
	pos += sprintf(pos, "tune%d\n", idx);

	return pos - buf;
}

static ssize_t bypass_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->bypass);
}

static ssize_t bypass_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	struct mdnie_table *table = NULL;
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s: %d\n", __func__, value);

	if (value >= BYPASS_MAX)
		return -EINVAL;

	value = (value) ? BYPASS_ON : BYPASS_OFF;

	mutex_lock(&mdnie->lock);
	mdnie->bypass = value;
	mutex_unlock(&mdnie->lock);

	table = &bypass_table[value];
	if (!IS_ERR_OR_NULL(table)) {
		mdnie_write_table(mdnie, table);
		dev_info(mdnie->dev, "%s\n", table->name);
	}

	return count;
}

static ssize_t lux_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->hbm);
}

static ssize_t lux_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int hbm = 0, update = 0;
	int ret, value;

	ret = kstrtoint(buf, 0, &value);
	if (ret < 0)
		return ret;

	mutex_lock(&mdnie->lock);
	hbm = get_hbm_index(value);
	update = (mdnie->hbm != hbm) ? 1 : 0;
	mdnie->hbm = update ? hbm : mdnie->hbm;
	mutex_unlock(&mdnie->lock);

	if (update) {
		dev_info(dev, "%s: %d\n", __func__, value);
		mdnie_update(mdnie);
	}

	return count;
}

/* Temporary solution: Do not use this sysfs as official purpose */
static ssize_t mdnie_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	char *pos = buf;
	struct mdnie_table *table = NULL;
	int i, j;
	u8 *buffer;

	if (!mdnie->enable) {
		dev_err(mdnie->dev, "mdnie state is off\n");
		goto exit;
	}

	table = mdnie_find_table(mdnie);

	for (i = 0; i < MDNIE_CMD_MAX; i++) {
		if (IS_ERR_OR_NULL(table->tune[i].sequence)) {
			dev_err(mdnie->dev, "mdnie sequence %s is null, %x\n", table->name, (u32)table->tune[i].sequence);
			goto exit;
		}
	}

	mdnie->ops.write(mdnie->data, table->tune[LEVEL1_KEY_UNLOCK].sequence, table->tune[LEVEL1_KEY_UNLOCK].size);

	pos += sprintf(pos, "+ %s\n", table->name);

	for (j = MDNIE_CMD1; j <= MDNIE_CMD2; j++) {
		buffer = kzalloc(table->tune[j].size, GFP_KERNEL);

		mdnie->ops.read(mdnie->data, table->tune[j].sequence[0], buffer, table->tune[j].size - 1);

		for (i = 0; i < table->tune[j].size - 1; i++) {
			pos += sprintf(pos, "%3d:\t0x%02x\t0x%02x", i + 1, table->tune[j].sequence[i+1], buffer[i]);
			if (table->tune[j].sequence[i+1] != buffer[i])
				pos += sprintf(pos, "\t(X)");
			pos += sprintf(pos, "\n");
		}

		kfree(buffer);
	}

	pos += sprintf(pos, "- %s\n", table->name);

	mdnie->ops.write(mdnie->data, table->tune[LEVEL1_KEY_LOCK].sequence, table->tune[LEVEL1_KEY_LOCK].size);

exit:
	return pos - buf;
}

static ssize_t sensorRGB_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d %d %d\n", mdnie->wrgb_current.r, mdnie->wrgb_current.g, mdnie->wrgb_current.b);
}

static ssize_t sensorRGB_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	struct mdnie_table *table = NULL;
	unsigned int white_r, white_g, white_b;
	int ret;

	ret = sscanf(buf, "%8d %8d %8d", &white_r, &white_g, &white_b);
	if (ret < 0)
		return ret;

	if (mdnie->enable
		&& mdnie->accessibility == ACCESSIBILITY_OFF
		&& mdnie->mode == AUTO
		&& (mdnie->scenario == BROWSER_MODE || mdnie->scenario == EBOOK_MODE)) {
		dev_info(dev, "%s: %d, %d, %d\n", __func__, white_r, white_g, white_b);

		table = mdnie_find_table(mdnie);

		memcpy(&mdnie->table_buffer, table, sizeof(struct mdnie_table));
		memcpy(&mdnie->sequence_buffer, table->tune[MDNIE_CMD1].sequence, table->tune[MDNIE_CMD1].size);
		mdnie->table_buffer.tune[MDNIE_CMD1].sequence = mdnie->sequence_buffer;

		mdnie->table_buffer.tune[MDNIE_CMD1].sequence[MDNIE_WHITE_R] = mdnie->wrgb_current.r = (unsigned char)white_r;
		mdnie->table_buffer.tune[MDNIE_CMD1].sequence[MDNIE_WHITE_G] = mdnie->wrgb_current.g = (unsigned char)white_g;
		mdnie->table_buffer.tune[MDNIE_CMD1].sequence[MDNIE_WHITE_B] = mdnie->wrgb_current.b = (unsigned char)white_b;

		mdnie_update_sequence(mdnie, &(mdnie->table_buffer));
	}

	return count;
}

static struct device_attribute mdnie_attributes[] = {
	__ATTR(mode, 0664, mode_show, mode_store),
	__ATTR(scenario, 0664, scenario_show, scenario_store),
	__ATTR(tuning, 0664, tuning_show, tuning_store),
	__ATTR(accessibility, 0664, accessibility_show, accessibility_store),
	__ATTR(color_correct, 0444, color_correct_show, NULL),
	__ATTR(bypass, 0664, bypass_show, bypass_store),
	__ATTR(lux, 0000, lux_show, lux_store),
	__ATTR(mdnie, 0444, mdnie_show, NULL),
	__ATTR(sensorRGB, 0664, sensorRGB_show, sensorRGB_store),
	__ATTR_NULL,
};

static int fb_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data)
{
	struct mdnie_info *mdnie;
	struct fb_event *evdata = data;
	int fb_blank;

	switch (event) {
	case FB_EVENT_BLANK:
		break;
	default:
		return NOTIFY_DONE;
	}

	mdnie = container_of(self, struct mdnie_info, fb_notif);
	if (!mdnie)
		return NOTIFY_DONE;

	fb_blank = *(int *)evdata->data;

	dev_info(mdnie->dev, "%s: %d\n", __func__, fb_blank);

	if (fb_blank == FB_BLANK_UNBLANK) {
		mutex_lock(&mdnie->lock);
		mdnie->enable = 1;
		mutex_unlock(&mdnie->lock);

		mdnie_update(mdnie);
	} else if (fb_blank == FB_BLANK_POWERDOWN) {
		mutex_lock(&mdnie->lock);
		mdnie->enable = 0;
		mutex_unlock(&mdnie->lock);
	}

	return NOTIFY_DONE;
}

static int mdnie_register_fb(struct mdnie_info *mdnie)
{
	memset(&mdnie->fb_notif, 0, sizeof(mdnie->fb_notif));
	mdnie->fb_notif.notifier_call = fb_notifier_callback;
	return fb_register_client(&mdnie->fb_notif);
}

int mdnie_register(struct device *p, void *data, mdnie_w w, mdnie_r r)
{
	int ret = 0;
	struct mdnie_info *mdnie;

	mdnie_class = class_create(THIS_MODULE, "mdnie");
	if (IS_ERR_OR_NULL(mdnie_class)) {
		pr_err("failed to create mdnie class\n");
		ret = -EINVAL;
		goto error0;
	}

	mdnie_class->dev_attrs = mdnie_attributes;

	mdnie = kzalloc(sizeof(struct mdnie_info), GFP_KERNEL);
	if (!mdnie) {
		pr_err("failed to allocate mdnie\n");
		ret = -ENOMEM;
		goto error1;
	}

	mdnie->dev = device_create(mdnie_class, p, 0, &mdnie, "mdnie");
	if (IS_ERR_OR_NULL(mdnie->dev)) {
		pr_err("failed to create mdnie device\n");
		ret = -EINVAL;
		goto error2;
	}

	mdnie->scenario = UI_MODE;
	mdnie->mode = STANDARD;
	mdnie->enable = 0;
	mdnie->tuning = 0;
	mdnie->accessibility = ACCESSIBILITY_OFF;
	mdnie->bypass = BYPASS_OFF;

	mdnie->data = data;
	mdnie->ops.write = w;
	mdnie->ops.read = r;

	mutex_init(&mdnie->lock);
	mutex_init(&mdnie->dev_lock);

	dev_set_drvdata(mdnie->dev, mdnie);

	mdnie_register_fb(mdnie);

	mdnie->enable = 1;
	mdnie_update(mdnie);

	dev_info(mdnie->dev, "registered successfully\n");

	return 0;

error2:
	kfree(mdnie);
error1:
	class_destroy(mdnie_class);
error0:
	return ret;
}

static int attr_store(struct device *dev,
	struct attribute *attr, const char *buf, size_t size)
{
	struct device_attribute *dev_attr = container_of(attr, struct device_attribute, attr);

	dev_attr->store(dev, dev_attr, buf, size);

	return 0;
}

static int attrs_store_iter(struct device *dev,
	const char *name, const char *buf, size_t size, struct attribute **attrs)
{
	int i;

	for (i = 0; attrs[i]; i++) {
		if (!strcmp(name, attrs[i]->name))
			attr_store(dev, attrs[i], buf, size);
	}

	return 0;
}

static int groups_store_iter(struct device *dev,
	const char *name, const char *buf, size_t size, const struct attribute_group **groups)
{
	int i;

	for (i = 0; groups[i]; i++)
		attrs_store_iter(dev, name, buf, size, groups[i]->attrs);

	return 0;
}

static int dev_attrs_store_iter(struct device *dev,
	const char *name, const char *buf, size_t size, struct device_attribute *dev_attrs)
{
	int i;

	for (i = 0; attr_name(dev_attrs[i]); i++) {
		if (!strcmp(name, attr_name(dev_attrs[i])))
			attr_store(dev, &dev_attrs[i].attr, buf, size);
	}

	return 0;
}

static int attr_find_and_store(struct device *dev,
	const char *name, const char *buf, size_t size)
{
	struct device_attribute *dev_attrs;
	const struct attribute_group **groups;

	if (dev->class && dev->class->dev_attrs) {
		dev_attrs = dev->class->dev_attrs;
		dev_attrs_store_iter(dev, name, buf, size, dev_attrs);
	}

	if (dev->type && dev->type->groups) {
		groups = dev->type->groups;
		groups_store_iter(dev, name, buf, size, groups);
	}

	if (dev->groups) {
		groups = dev->groups;
		groups_store_iter(dev, name, buf, size, groups);
	}

	return 0;
}

ssize_t attr_store_for_each(struct class *cls,
	const char *name, const char *buf, size_t size)
{
	struct class_dev_iter iter;
	struct device *dev;
	int error = 0;
	struct class *class = cls;

	if (!class)
		return -EINVAL;
	if (!class->p) {
		WARN(1, "%s called for class '%s' before it was initialized",
		     __func__, class->name);
		return -EINVAL;
	}

	class_dev_iter_init(&iter, class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter))) {
		error = attr_find_and_store(dev, name, buf, size);
		if (error)
			break;
	}
	class_dev_iter_exit(&iter);

	return error;
}

struct class *get_mdnie_class(void)
{
	return mdnie_class;
}


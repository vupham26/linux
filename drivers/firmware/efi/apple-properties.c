/*
 * apple-properties.c - EFI device properties on Macs
 * Copyright (C) 2016 Lukas Wunner <lukas@wunner.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2) as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) "apple-properties: " fmt

#include <linux/bootmem.h>
#include <linux/dmi.h>
#include <linux/efi.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/ucs2_string.h>
#include <asm/setup.h>

struct dev_header {
	u32 len;
	u32 prop_count;
	struct efi_dev_path path[0];
	/*
	 * followed by key/value pairs, each key and value preceded by u32 len,
	 * len includes itself, value may be empty (in which case its len is 4)
	 */
};

struct properties_header {
	u32 len;
	u32 version;
	u32 dev_count;
	struct dev_header dev_header[0];
};


static bool __initdata dump_properties = false;

static int __init dump_properties_enable(char *arg)
{
	dump_properties = true;
	return 0;
}

__setup("dump_apple_properties", dump_properties_enable);


static u8 __initdata one = 1;

static void __init unmarshal_key_value_pairs(struct dev_header *dev_header,
					     struct device *dev, void *ptr,
					     struct property_entry entry[])
{
	int i;

	for (i = 0; i < dev_header->prop_count; i++) {
		int remaining = dev_header->len - (ptr - (void *)dev_header);
		u32 key_len, val_len;
		char *key;

		if (sizeof(key_len) > remaining)
			break;
		key_len = *(typeof(key_len) *)ptr;
		if (key_len + sizeof(val_len) > remaining ||
		    key_len < sizeof(key_len) + sizeof(efi_char16_t) ||
		    *(efi_char16_t *)(ptr + sizeof(key_len)) == 0) {
			dev_err(dev, "invalid property name len at %#zx\n",
				ptr - (void *)dev_header);
			break;
		}
		val_len = *(typeof(val_len) *)(ptr + key_len);
		if (key_len + val_len > remaining ||
		    val_len < sizeof(val_len)) {
			dev_err(dev, "invalid property val len at %#zx\n",
				ptr - (void *)dev_header + key_len);
			break;
		}

		key = kzalloc((key_len - sizeof(key_len)) * 4 + 1, GFP_KERNEL);
		if (!key) {
			dev_err(dev, "cannot allocate property name\n");
			break;
		}
		ucs2_as_utf8(key, ptr + sizeof(key_len),
			     key_len - sizeof(key_len));

		entry[i].name = key;
		entry[i].is_array = true;
		entry[i].length = val_len - sizeof(val_len);
		entry[i].pointer.raw_data = ptr + key_len + sizeof(val_len);
		if (!entry[i].length) {
			/* driver core doesn't accept empty properties */
			entry[i].length = 1;
			entry[i].pointer.raw_data = &one;
		}

		ptr += key_len + val_len;

		if (dump_properties) {
			dev_info(dev, "property: %s\n", entry[i].name);
			print_hex_dump(KERN_INFO, pr_fmt(), DUMP_PREFIX_OFFSET,
				16, 1, entry[i].pointer.raw_data,
				entry[i].length, true);
		}
	}

	if (i != dev_header->prop_count) {
		dev_err(dev, "got %d device properties, expected %u\n", i,
			dev_header->prop_count);
		print_hex_dump(KERN_ERR, pr_fmt(), DUMP_PREFIX_OFFSET,
			16, 1, dev_header, dev_header->len, true);
	} else
		dev_info(dev, "assigning %d device properties\n", i);
}

static int __init unmarshal_devices(struct properties_header *properties)
{
	size_t offset = offsetof(struct properties_header, dev_header[0]);

	while (offset + sizeof(struct dev_header) < properties->len) {
		struct dev_header *dev_header = (void *)properties + offset;
		struct property_entry *entry = NULL;
		struct device *dev;
		int ret, i;
		void *ptr;

		if (offset + dev_header->len > properties->len) {
			pr_err("invalid len in dev_header at %#lx\n", offset);
			return -EINVAL;
		}

		ptr = dev_header->path;
		ret = get_device_by_efi_path((struct efi_dev_path **)&ptr,
			       dev_header->len - sizeof(*dev_header), &dev);
		if (ret) {
			pr_err("device path parse error %d at %#zx:\n", ret,
			       ptr - (void *)dev_header);
			print_hex_dump(KERN_ERR, pr_fmt(), DUMP_PREFIX_OFFSET,
			       16, 1, dev_header, dev_header->len, true);
			goto skip_device;
		}

		entry = kcalloc(dev_header->prop_count + 1, sizeof(*entry),
				GFP_KERNEL);
		if (!entry) {
			dev_err(dev, "cannot allocate properties\n");
			goto skip_device;
		}

		unmarshal_key_value_pairs(dev_header, dev, ptr, entry);
		if (!entry[0].name)
			goto skip_device;

		ret = device_add_properties(dev, entry); /* makes deep copy */
		if (ret)
			dev_err(dev, "error %d assigning properties\n", ret);
		for (i = 0; entry[i].name; i++)
			kfree(entry[i].name);

skip_device:
		kfree(entry);
		put_device(dev);
		offset += dev_header->len;
	}

	return 0;
}

static int __init map_properties(void)
{
	struct setup_data *data;
	u32 data_len;
	u64 pa_data;
	int ret;

	if (!dmi_match(DMI_SYS_VENDOR, "Apple Inc.") &&
	    !dmi_match(DMI_SYS_VENDOR, "Apple Computer, Inc."))
		return -ENODEV;

	pa_data = boot_params.hdr.setup_data;
	while (pa_data) {
		data = ioremap(pa_data, sizeof(*data));
		if (!data) {
			pr_err("cannot map setup_data header\n");
			return -ENOMEM;
		}

		if (data->type != SETUP_APPLE_PROPERTIES) {
			pa_data = data->next;
			iounmap(data);
			continue;
		}

		data_len = data->len;
		iounmap(data);
		data = ioremap(pa_data, sizeof(*data) + data_len);
		if (!data) {
			pr_err("cannot map setup_data payload\n");
			return -ENOMEM;
		}
		ret = unmarshal_devices((struct properties_header *)data->data);
		data->len = 0;
		iounmap(data);
		free_bootmem_late(pa_data + sizeof(*data), data_len);
		return ret;
	}
	return 0;
}

fs_initcall(map_properties);

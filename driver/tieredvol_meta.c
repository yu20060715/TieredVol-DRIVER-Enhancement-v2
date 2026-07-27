#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/crc32c.h>
#include "tieredvol.h"

#define TV_MAX_CONFIG_SIZE (1024 * 1024)

static int parse_u32(const char *s, u32 *out)
{
	unsigned long v;

	if (kstrtoul(s, 10, &v))
		return -EINVAL;
	if (v > (~0U))
		return -EINVAL;
	*out = (u32)v;
	return 0;
}

static int parse_u64(const char *s, u64 *out)
{
	unsigned long long v;

	if (kstrtoull(s, 10, &v))
		return -EINVAL;
	*out = v;
	return 0;
}

static int parse_line(char *line, char **key, char **val)
{
	char *eq;

	eq = strchr(line, '=');
	if (!eq)
		return -EINVAL;
	*eq = '\0';
	*key = line;
	*val = eq + 1;

	{
		char *nl = strchr(*val, '\n');

		if (nl)
			*nl = '\0';
		nl = strchr(*val, '\r');
		if (nl)
			*nl = '\0';
	}
	return 0;
}

static int parse_csv_u32(char *s, u32 *arr, int max, int *count)
{
	char *tok;
	int n = 0;

	for (tok = strsep(&s, ","); tok && n < max; tok = strsep(&s, ",")) {
		unsigned long v;

		if (kstrtoul(tok, 10, &v))
			return -EINVAL;
		if (v > (~0U))
			return -EINVAL;
		arr[n++] = (u32)v;
	}
	*count = n;
	return 0;
}

static int parse_num_prefix(const char *s, unsigned long *idx,
			    const char **suffix)
{
	const char *p = s;
	unsigned long val = 0;
	int found_digit = 0;

	while (*p >= '0' && *p <= '9') {
		val = val * 10 + (*p - '0');
		found_digit = 1;
		p++;
	}

	if (!found_digit)
		return -EINVAL;

	*idx = val;
	*suffix = p;
	return 0;
}

/* Compute CRC32C over file content, excluding lines containing "crc32=" */
static u32 tv_compute_config_crc(const char *buf)
{
	const char *p = buf;
	u32 crc = 0;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);

		/* Skip lines containing "crc32=" */
		if (len < 7 || memcmp(p, "crc32=", 6) != 0)
			crc = crc32c(crc, p, len);

		p = nl ? nl + 1 : p + len;
	}
	return crc;
}

int tv_metadata_load_kernel(struct tieredvol_metadata *meta,
			    const char *path)
{
	struct file *f;
	loff_t pos = 0, file_size;
	char *buf;
	char *line, *next_line;
	int ret = 0;
	u32 expected_crc = 0;
	bool has_crc = false;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	file_size = i_size_read(file_inode(f));
	if (file_size <= 0 || file_size > TV_MAX_CONFIG_SIZE) {
		filp_close(f, NULL);
		return -EINVAL;
	}

	buf = vmalloc(file_size + 1);
	if (!buf) {
		filp_close(f, NULL);
		return -ENOMEM;
	}

	{
		ssize_t nr;

		nr = kernel_read(f, buf, file_size, &pos);
		filp_close(f, NULL);

		if (nr < 0) {
			vfree(buf);
			return (int)nr;
		}
		if (nr == 0)
			pr_warn("tieredvol: config file is empty\n");
		buf[nr] = '\0';
	}

	ret = 0;

	memset(meta, 0, sizeof(*meta));

	/* CRC32 validation — must be computed before parsing modifies buf */
	if (has_crc) {
		u32 actual_crc = tv_compute_config_crc(buf);

		if (actual_crc != expected_crc) {
			pr_err("tieredvol: config CRC mismatch (expected=0x%08x actual=0x%08x) — file may be corrupted\n",
			       expected_crc, actual_crc);
			ret = -EIO;
			goto out;
		}
		pr_info("tieredvol: config CRC OK (0x%08x)\n", actual_crc);
	}

	for (line = buf; line && *line; line = next_line) {
		char *k, *v;
		unsigned long idx;
		const char *suf;

		next_line = strchr(line, '\n');
		if (next_line)
			*next_line++ = '\0';

		if (parse_line(line, &k, &v) < 0)
			continue;

		if (strcmp(k, "crc32") == 0) {
			if (kstrtou32(v, 10, &expected_crc) == 0)
				has_crc = true;
			continue;
		}

		if (strcmp(k, "version") == 0) {
			if (parse_u32(v, &meta->version) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(k, "chunk_size") == 0) {
			if (parse_u32(v, &meta->chunk_size) < 0 ||
			    meta->chunk_size == 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(k, "segment_count") == 0) {
			if (parse_u32(v, &meta->segment_count) < 0) {
				ret = -EINVAL;
				goto out;
			}
			if (meta->segment_count > TV_MAX_SEGS) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(k, "disk_count") == 0) {
			if (parse_u32(v, &meta->disk_count) < 0) {
				ret = -EINVAL;
				goto out;
			}
			if (meta->disk_count > TV_MAX_DISKS) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strncmp(k, "disk", 4) == 0 &&
			   strstr(k, "_name")) {
			if (parse_num_prefix(k + 4, &idx, &suf) == 0 &&
			    strcmp(suf, "_name") == 0 &&
			    idx < TV_MAX_DISKS &&
			    idx < meta->disk_count) {
				strncpy(meta->disk_names[idx], v, 63);
				meta->disk_names[idx][63] = '\0';
			}
		} else if (strncmp(k, "seg", 3) == 0) {
			struct tieredvol_segment *seg;

			if (parse_num_prefix(k + 3, &idx, &suf) < 0)
				continue;
			if (idx >= TV_MAX_SEGS)
				continue;

			seg = &meta->segments[idx];

		if (strcmp(suf, "_begin") == 0) {
			if (parse_u64(v, &seg->logical_begin) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_end") == 0) {
			if (parse_u64(v, &seg->logical_end) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_count") == 0) {
			if (parse_u32(v, &seg->disk_count) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_stripe") == 0) {
			if (parse_u64(v, &seg->stripe_size) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_disks") == 0) {
			int n;

			if (parse_csv_u32(v, seg->disk_index,
					   TV_MAX_DISKS, &n) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_weight") == 0) {
			int n;

			if (parse_csv_u32(v, seg->weight,
					   TV_MAX_DISKS, &n) < 0) {
				ret = -EINVAL;
				goto out;
			}
		} else if (strcmp(suf, "_mirror") == 0) {
			u32 mirror_idx;

			if (parse_u32(v, &mirror_idx) < 0) {
				ret = -EINVAL;
				goto out;
			}
			seg->mirror_enabled = true;
			seg->mirror_disk = mirror_idx;
		}
		} else if (strcmp(k, "policy") == 0) {
			long v2;

			if (kstrtol(v, 10, &v2) == 0)
				meta->runtime_policy = (int)v2;
		} else if (strcmp(k, "stale_ms") == 0) {
			parse_u32(v, &meta->runtime_stale_ms);
		} else if (strcmp(k, "ema_shift") == 0) {
			parse_u32(v, &meta->runtime_ema_shift);
		} else if (strcmp(k, "wear_bias") == 0) {
			parse_u32(v, &meta->runtime_wear_bias);
		}
	}

	/* Validate disk indices */
	{
		u32 si, j;

		for (si = 0; si < meta->segment_count; si++) {
			struct tieredvol_segment *seg = &meta->segments[si];

			for (j = 0; j < seg->disk_count; j++) {
				if (seg->disk_index[j] >= meta->disk_count) {
					pr_err("tieredvol: seg%u disk index %u >= disk_count %u\n",
					       si, seg->disk_index[j],
					       meta->disk_count);
					ret = -EINVAL;
					goto out;
				}
			}
		}
	}

	pr_info("tieredvol: loaded metadata: %u disks, %u segments\n",
		meta->disk_count, meta->segment_count);

out:
	vfree(buf);
	return ret;
}

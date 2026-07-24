#include "tieredvol.h"

struct tieredvol_map tv_map_logical(u64 logical,
				    struct tieredvol_metadata *meta)
{
	struct tieredvol_map err = { .disk = -1, .offset = 0, .length = 0 };
	int seg_idx, disk_idx;
	const struct tieredvol_segment *seg;
	u64 stripe_no, offset_in;
	u64 boundary[TV_MAX_DISKS + 1];
	int i;

	if (!meta || meta->segment_count == 0)
		return err;

	seg_idx = -1;
	for (i = 0; i < (int)meta->segment_count; i++) {
		if (logical >= meta->segments[i].logical_begin &&
		    logical <  meta->segments[i].logical_end) {
			seg_idx = i;
			break;
		}
	}

	if (seg_idx < 0)
		return err;

	seg = &meta->segments[seg_idx];

	if (seg->disk_count == 0 || seg->disk_count > TV_MAX_DISKS)
		return err;

	stripe_no = (logical - seg->logical_begin) / seg->stripe_size;
	offset_in = (logical - seg->logical_begin) % seg->stripe_size;

	boundary[0] = 0;
	for (i = 0; i < (int)seg->disk_count; i++)
		boundary[i + 1] = boundary[i] +
			(u64)seg->weight[i] * TV_CHUNK_SIZE;

	disk_idx = -1;
	for (i = 0; i < (int)seg->disk_count; i++) {
		if (offset_in >= boundary[i] && offset_in < boundary[i + 1]) {
			disk_idx = i;
			break;
		}
	}

	if (disk_idx < 0)
		return err;

	{
		struct tieredvol_map map;

		map.disk = (int)seg->disk_index[disk_idx];
		map.offset = stripe_no * (u64)seg->weight[disk_idx] *
			     TV_CHUNK_SIZE +
			     (offset_in - boundary[disk_idx]);
		map.length = (u64)seg->weight[disk_idx] * TV_CHUNK_SIZE;

		return map;
	}
}

struct tieredvol_map tv_map_logical_adaptive(u64 logical,
					    struct tieredvol_metadata *meta,
					    u64 *ema_load, bool *stale,
					    int ndisks)
{
	struct tieredvol_map err = { .disk = -1, .offset = 0, .length = 0 };
	int seg_idx;
	const struct tieredvol_segment *seg;
	u64 stripe_no, offset_in;
	int best_disk = -1;
	u64 best_load = (u64)-1;
	int i;

	if (!meta || meta->segment_count == 0)
		return err;

	seg_idx = -1;
	for (i = 0; i < (int)meta->segment_count; i++) {
		if (logical >= meta->segments[i].logical_begin &&
		    logical <  meta->segments[i].logical_end) {
			seg_idx = i;
			break;
		}
	}

	if (seg_idx < 0)
		return err;

	seg = &meta->segments[seg_idx];

	if (seg->disk_count == 0 || seg->disk_count > TV_MAX_DISKS)
		return err;

	stripe_no = (logical - seg->logical_begin) / seg->stripe_size;
	offset_in = (logical - seg->logical_begin) % seg->stripe_size;

	for (i = 0; i < (int)seg->disk_count; i++) {
		u32 d = seg->disk_index[i];

		if (d >= (u32)ndisks)
			continue;
		if (stale[d])
			continue;
		if (ema_load[d] < best_load) {
			best_load = ema_load[d];
			best_disk = i;
		}
	}

	if (best_disk < 0) {
		for (i = 0; i < (int)seg->disk_count; i++) {
			u32 d = seg->disk_index[i];

			if (d < (u32)ndisks) {
				best_disk = i;
				break;
			}
		}
	}

	if (best_disk < 0)
		return err;

	{
		struct tieredvol_map map;
		u64 disk_chunk = (u64)seg->weight[best_disk] * TV_CHUNK_SIZE;

		map.disk = (int)seg->disk_index[best_disk];
		map.offset = stripe_no * disk_chunk +
			     (offset_in % disk_chunk);
		map.length = disk_chunk;

		return map;
	}
}

// SPDX-License-Identifier: GPL-2.0-only
/* Block frontend for Apple's VFL/SwissPPNFTL logical medium. */
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mtd/apple-ppn-access.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

#include "apple-sftl.h"

#define SFTL_BLOCK_NAME "apple-sftl"
#define SFTL_DISK_MINORS 16
#define SFTL_LOGICAL_BYTES 4096U

struct sftl_disk {
	struct apple_sftl *ftl;
	struct blk_mq_tag_set tags;
	struct gendisk *disk;
	struct mutex io_lock; /* serializes requests using buffer */
	u8 *buffer;
	unsigned int id;
	bool dead;
	bool writable;
};

static DEFINE_IDA(sftl_disk_ids);
static int sftl_major;

/* The blocking queue and this lock keep the preallocated buffer exclusive. */
static int sftl_read_request(struct sftl_disk *sftl, struct request *request)
{
	struct req_iterator iterator;
	struct bio_vec bvec;
	u64 position = (u64)blk_rq_pos(request) << SECTOR_SHIFT;
	u32 cached = U32_MAX;

	rq_for_each_segment(bvec, request, iterator) {
		unsigned int done = 0;

		while (done < bvec.bv_len) {
			u32 logical = position >> 12;
			unsigned int offset = position & (SFTL_LOGICAL_BYTES - 1);
			unsigned int length = min(bvec.bv_len - done,
						  SFTL_LOGICAL_BYTES - offset);
			void *mapped;
			int ret;

			if (logical != cached) {
				ret = apple_sftl_read(sftl->ftl, logical, sftl->buffer);
				if (ret)
					return ret;
				cached = logical;
			}
			mapped = kmap_local_page(bvec.bv_page);
			memcpy(mapped + bvec.bv_offset + done,
			       sftl->buffer + offset, length);
			kunmap_local(mapped);
			done += length;
			position += length;
		}
	}
	return 0;
}

/* Preserve untouched sectors at both ends and collect up to four 4 KiB
 * pages per physical program. Request segments can split a logical page.
 */
static int sftl_write_request(struct sftl_disk *sftl, struct request *request)
{
	struct req_iterator iterator;
	struct bio_vec bvec;
	u64 position = (u64)blk_rq_pos(request) << SECTOR_SHIFT;
	u32 cached = U32_MAX, first = 0;
	unsigned int count = 0;
	int ret;

	if (request->cmd_flags & REQ_PREFLUSH) {
		ret = apple_sftl_flush(sftl->ftl);
		if (ret)
			return ret;
	}
	rq_for_each_segment(bvec, request, iterator) {
		unsigned int done = 0;

		while (done < bvec.bv_len) {
			u32 logical = position >> 12;
			unsigned int offset = position & (SFTL_LOGICAL_BYTES - 1);
			unsigned int length = min(bvec.bv_len - done,
						  SFTL_LOGICAL_BYTES - offset);
			void *mapped;

			if (logical != cached) {
				if (count == 4) {
					ret = apple_sftl_write(sftl->ftl, first,
							       sftl->buffer, count);
					if (ret)
						return ret;
					count = 0;
				}
				if (!count)
					first = logical;
				ret = apple_sftl_read(sftl->ftl, logical,
						      sftl->buffer + count * SFTL_LOGICAL_BYTES);
				if (ret)
					return ret;
				count++;
				cached = logical;
			}
			mapped = kmap_local_page(bvec.bv_page);
			memcpy(sftl->buffer + (count - 1) * SFTL_LOGICAL_BYTES + offset,
			       mapped + bvec.bv_offset + done, length);
			kunmap_local(mapped);
			done += length;
			position += length;
		}
	}
	if (count) {
		ret = apple_sftl_write(sftl->ftl, first, sftl->buffer, count);
		if (ret)
			return ret;
	}
	/* Every completed write is already verified on media. FUA shares that
	 * completion rule and never acknowledges a volatile cached page.
	 */
	return request->cmd_flags & REQ_FUA ? apple_sftl_flush(sftl->ftl) : 0;
}

static blk_status_t sftl_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *data)
{
	struct request *request = data->rq;
	struct sftl_disk *sftl = request->q->queuedata;
	sector_t capacity = get_capacity(sftl->disk);
	unsigned int noio_flags;
	blk_status_t status;
	int ret = 0;

	mutex_lock(&sftl->io_lock);
	/* Map and provider buffers may allocate while the root disk is busy. */
	noio_flags = memalloc_noio_save();
	blk_mq_start_request(request);
	if (sftl->dead) {
		ret = -ENODEV;
	} else if (req_op(request) == REQ_OP_FLUSH) {
		ret = apple_sftl_flush(sftl->ftl);
	} else if (req_op(request) != REQ_OP_READ && req_op(request) != REQ_OP_WRITE) {
		ret = -EOPNOTSUPP;
	} else if (blk_rq_pos(request) > capacity ||
		   blk_rq_sectors(request) > capacity - blk_rq_pos(request)) {
		ret = -EIO;
	} else if (req_op(request) == REQ_OP_WRITE) {
		ret = sftl->writable ? sftl_write_request(sftl, request) : -EROFS;
	} else {
		ret = sftl_read_request(sftl, request);
	}
	memalloc_noio_restore(noio_flags);
	mutex_unlock(&sftl->io_lock);
	status = errno_to_blk_status(ret);
	blk_mq_end_request(request, status);
	return BLK_STS_OK;
}

static const struct blk_mq_ops sftl_mq_ops = {
	.queue_rq = sftl_queue_rq,
};

static const struct block_device_operations sftl_block_ops = {
	.owner = THIS_MODULE,
};

static int sftl_disk_probe(struct platform_device *pdev)
{
	struct queue_limits limits = {
		.logical_block_size = SECTOR_SIZE,
		.physical_block_size = SFTL_LOGICAL_BYTES,
		.max_hw_sectors = 128,
	};
	struct apple_ppn *provider = dev_get_platdata(&pdev->dev);
	struct apple_sftl_info info;
	struct sftl_disk *sftl;
	struct gendisk *disk;
	int ret, id;

	if (!provider || !provider->ops || !provider->ops->read_page)
		return -EINVAL;
	/* A read-only disk also needs its recovered map to stay current. */
	if (provider->access) {
		ret = apple_ppn_claim_sftl(provider->access);
		if (ret)
			return ret;
		ret = devm_add_action_or_reset(&pdev->dev, apple_ppn_release_sftl,
					       provider->access);
		if (ret)
			return ret;
	}
	sftl = kzalloc(sizeof(*sftl), GFP_KERNEL);
	if (!sftl)
		return -ENOMEM;
	mutex_init(&sftl->io_lock);
	sftl->buffer = kmalloc(APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	if (!sftl->buffer) {
		ret = -ENOMEM;
		goto free_state;
	}
	ret = apple_sftl_load(provider, &sftl->ftl);
	if (ret)
		goto free_buffer;
	apple_sftl_get_info(sftl->ftl, &info);
	if (!info.disk_pages || info.disk_pages > info.logical_pages) {
		ret = -EINVAL;
		goto free_ftl;
	}
	/* Appending continues the journal; it must end where the map does. */
	sftl->writable = provider->ops->program_page && !info.read_only &&
			 info.sequence == info.journal_sequence;
	if (sftl->writable && provider->ops->erase_block && provider->checkpoint_ops)
		apple_sftl_set_checkpoint_ops(sftl->ftl, provider->checkpoint_ops,
					      provider->priv);
	if (info.read_only)
		dev_warn(&pdev->dev, "incomplete metadata publication, read-only\n");
	else if (provider->ops->program_page && !sftl->writable)
		dev_warn(&pdev->dev, "journal ends before the allocation floor, read-only\n");
	if (sftl->writable)
		limits.features |= BLK_FEAT_FUA;
	id = ida_alloc_range(&sftl_disk_ids, 0, MINORMASK / SFTL_DISK_MINORS,
			     GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto free_ftl;
	}
	sftl->id = id;
	ret = blk_mq_alloc_sq_tag_set(&sftl->tags, &sftl_mq_ops, 8,
				      BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_BLOCKING);
	if (ret)
		goto free_id;
	/* First use verifies every page in a candidate block. Leave time for
	 * the bounded 500-second scan and synchronous data/footer programs.
	 */
	if (sftl->writable)
		sftl->tags.timeout = 600U * HZ;
	disk = blk_mq_alloc_disk(&sftl->tags, &limits, sftl);
	if (IS_ERR(disk)) {
		ret = PTR_ERR(disk);
		goto free_tags;
	}
	sftl->disk = disk;
	disk->major = sftl_major;
	disk->first_minor = id * SFTL_DISK_MINORS;
	disk->minors = SFTL_DISK_MINORS;
	disk->fops = &sftl_block_ops;
	disk->private_data = sftl;
	snprintf(disk->disk_name, DISK_NAME_LEN, "applesftl%d", id);
	set_capacity(disk, (sector_t)info.disk_pages * (SFTL_LOGICAL_BYTES / SECTOR_SIZE));
	set_disk_ro(disk, !sftl->writable);
	platform_set_drvdata(pdev, sftl);
	ret = device_add_disk(&pdev->dev, disk, NULL);
	if (ret)
		goto put_disk;
	dev_info(&pdev->dev, "%s: %s, %u logical pages, sequence %llu\n",
		 disk->disk_name, sftl->writable ? "read-write" : "read-only",
		 info.disk_pages, info.sequence);
	return 0;

put_disk:
	platform_set_drvdata(pdev, NULL);
	put_disk(disk);
free_tags:
	blk_mq_free_tag_set(&sftl->tags);
free_id:
	ida_free(&sftl_disk_ids, sftl->id);
free_ftl:
	apple_sftl_free(sftl->ftl);
free_buffer:
	kfree(sftl->buffer);
free_state:
	kfree(sftl);
	return ret;
}

static void sftl_disk_remove(struct platform_device *pdev)
{
	struct sftl_disk *sftl = platform_get_drvdata(pdev);

	mutex_lock(&sftl->io_lock);
	sftl->dead = true;
	mutex_unlock(&sftl->io_lock);
	/* del_gendisk freezes and drains the queue before the provider is freed. */
	del_gendisk(sftl->disk);
	put_disk(sftl->disk);
	blk_mq_free_tag_set(&sftl->tags);
	ida_free(&sftl_disk_ids, sftl->id);
	apple_sftl_free(sftl->ftl);
	kfree(sftl->buffer);
	kfree(sftl);
}

static struct platform_driver sftl_driver = {
	.probe = sftl_disk_probe,
	.remove = sftl_disk_remove,
	.driver = {
		.name = SFTL_BLOCK_NAME,
	},
};

static int __init sftl_init(void)
{
	int ret;

	sftl_major = register_blkdev(0, SFTL_BLOCK_NAME);
	if (sftl_major < 0)
		return sftl_major;
	ret = platform_driver_register(&sftl_driver);
	if (ret)
		unregister_blkdev(sftl_major, SFTL_BLOCK_NAME);
	return ret;
}
module_init(sftl_init);

static void __exit sftl_exit(void)
{
	platform_driver_unregister(&sftl_driver);
	unregister_blkdev(sftl_major, SFTL_BLOCK_NAME);
	ida_destroy(&sftl_disk_ids);
}
module_exit(sftl_exit);

MODULE_DESCRIPTION("Apple VFL/SwissPPNFTL block driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" SFTL_BLOCK_NAME);

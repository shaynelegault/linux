// SPDX-License-Identifier: GPL-2.0-only
/* Read-only block frontend for Apple's VFL/SwissPPNFTL logical medium. */
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
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

static blk_status_t sftl_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *data)
{
	struct request *request = data->rq;
	struct sftl_disk *sftl = request->q->queuedata;
	sector_t capacity = get_capacity(sftl->disk);
	blk_status_t status;
	int ret = 0;

	blk_mq_start_request(request);
	mutex_lock(&sftl->io_lock);
	if (sftl->dead) {
		ret = -ENODEV;
	} else if (req_op(request) == REQ_OP_FLUSH) {
		/* A read-only device has no buffered writes to flush. */
	} else if (req_op(request) != REQ_OP_READ) {
		ret = -EOPNOTSUPP;
	} else if (blk_rq_pos(request) > capacity ||
		   blk_rq_sectors(request) > capacity - blk_rq_pos(request)) {
		ret = -EIO;
	} else {
		ret = sftl_read_request(sftl, request);
	}
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

	if (!provider)
		return -EINVAL;
	sftl = kzalloc(sizeof(*sftl), GFP_KERNEL);
	if (!sftl)
		return -ENOMEM;
	mutex_init(&sftl->io_lock);
	sftl->buffer = kmalloc(SFTL_LOGICAL_BYTES, GFP_KERNEL);
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
	set_disk_ro(disk, true);
	platform_set_drvdata(pdev, sftl);
	ret = device_add_disk(&pdev->dev, disk, NULL);
	if (ret)
		goto put_disk;
	dev_info(&pdev->dev, "%s: read-only, %u logical pages, sequence %llu\n",
		 disk->disk_name, info.disk_pages, info.sequence);
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

MODULE_DESCRIPTION("Apple VFL/SwissPPNFTL read-only block driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" SFTL_BLOCK_NAME);

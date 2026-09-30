#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <crypto/skcipher.h>
#include <linux/string.h>
#include <linux/random.h>
#include <linux/debugfs.h>

#define DEVICE_NAME "secure_ram"
#define RAMDISK_SIZE (16UL * 1024 * 1024)
#define SECTOR_SIZE_BYTES 512

static void *ram_storage;
static struct gendisk *ram_disk;
static int ram_major;
static struct crypto_skcipher *crypto_tfm;
static struct workqueue_struct *secure_ram_wq;
static DEFINE_MUTEX(storage_lock);
static bool dump_ciphertext;
static bool dumped_ciphertext;

module_param(dump_ciphertext, bool, 0444);
MODULE_PARM_DESC(dump_ciphertext, "Dump first encrypted sector for debugging");

static u8 encryption_key[64];
static struct dentry *debug_dir;
static struct debugfs_blob_wrapper debug_blob;

struct secure_ram_work {
    struct work_struct work;
    struct bio *bio;
};

static int crypt_sector(struct skcipher_request *req, struct crypto_wait *wait, u8 *buffer, sector_t sector, bool encrypt){
    struct scatterlist sg;
    u8 iv[16] = {0};
    __le64 sector_le;
    int ret;

    sector_le = cpu_to_le64((u64)sector);
    memcpy(iv, &sector_le, sizeof(sector_le));
    sg_init_one(&sg, buffer, SECTOR_SIZE_BYTES);

    skcipher_request_set_crypt(req, &sg, &sg, SECTOR_SIZE_BYTES, iv);

    if (encrypt) ret = crypto_wait_req(crypto_skcipher_encrypt(req), wait);
    else ret = crypto_wait_req(crypto_skcipher_decrypt(req), wait);
    return ret;
}

static int process_write_bio(struct bio *bio, struct skcipher_request *req, struct crypto_wait *wait){
    struct bio_vec bvec;
    struct bvec_iter iter;
    u8 sector_buf[SECTOR_SIZE_BYTES] __aligned(16);
    sector_t sector = bio->bi_iter.bi_sector;
    size_t fill = 0;

    bio_for_each_segment(bvec, bio, iter) {
        void *base = bvec_kmap_local(&bvec);
        size_t pos = 0;

        while (pos < bvec.bv_len) {
            size_t take = min_t(size_t, bvec.bv_len - pos, SECTOR_SIZE_BYTES - fill);
            memcpy(sector_buf + fill, (u8 *)base + pos, take);

            fill += take;
            pos += take;
            if (fill == SECTOR_SIZE_BYTES) {
                size_t offset;
                if (crypt_sector(req, wait, sector_buf, sector, true)) {
                    kunmap_local(base);
                    memzero_explicit(sector_buf, sizeof(sector_buf));
                    return -EIO;
                }
                if (dump_ciphertext && !dumped_ciphertext && sector == 0) {
                    print_hex_dump(KERN_INFO, "secure_ram ciphertext: ", DUMP_PREFIX_OFFSET, 16, 1, sector_buf, 64, true);
                    dumped_ciphertext = true;
                }
                offset = (size_t)sector << SECTOR_SHIFT;
                memcpy((u8 *)ram_storage + offset, sector_buf, SECTOR_SIZE_BYTES);

                sector++;
                fill = 0;
            }
        }
        kunmap_local(base);
    }
    if (fill != 0) {
        memzero_explicit(sector_buf, sizeof(sector_buf));
        return -EINVAL;
    }
    memzero_explicit(sector_buf, sizeof(sector_buf));
    return 0;
}

static int process_read_bio(struct bio *bio, struct skcipher_request *req, struct crypto_wait *wait){
    struct bio_vec bvec;
    struct bvec_iter iter;
    u8 sector_buf[SECTOR_SIZE_BYTES] __aligned(16);
    sector_t sector = bio->bi_iter.bi_sector;
    size_t used = SECTOR_SIZE_BYTES;

    bio_for_each_segment(bvec, bio, iter) {
        void *base = bvec_kmap_local(&bvec);
        size_t pos = 0;
        
        while (pos < bvec.bv_len) {
            size_t take;
            if (used == SECTOR_SIZE_BYTES) {
                size_t offset =  (size_t)sector << SECTOR_SHIFT;
                memcpy(sector_buf, (u8 *)ram_storage + offset, SECTOR_SIZE_BYTES);
                if (crypt_sector(req, wait,  sector_buf, sector, false)) {
                    kunmap_local(base);
                    memzero_explicit(sector_buf, sizeof(sector_buf));
                    return -EIO;
                }
                used = 0;
            }

            take = min_t(size_t, bvec.bv_len - pos, SECTOR_SIZE_BYTES - used);
            memcpy((u8 *)base + pos, sector_buf + used, take);

            used += take;
            pos += take;
            if (used == SECTOR_SIZE_BYTES) sector++;
        }
        kunmap_local(base);
    }
    memzero_explicit(sector_buf, sizeof(sector_buf));
    return 0;
}

static int process_bio(struct bio *bio){
    struct skcipher_request *req;
    DECLARE_CRYPTO_WAIT(wait);
    sector_t sector = bio->bi_iter.bi_sector;
    size_t bytes = bio->bi_iter.bi_size;
    size_t offset;
    int ret = 0;

    if (bio_op(bio) == REQ_OP_FLUSH) return 0;
    if (bio_op(bio) != REQ_OP_READ && bio_op(bio) != REQ_OP_WRITE) return -EOPNOTSUPP;
    if (bytes % SECTOR_SIZE_BYTES) return -EINVAL;
    if (sector > (RAMDISK_SIZE >> SECTOR_SHIFT)) return -EIO;

    offset = (size_t)sector << SECTOR_SHIFT;
    if (bytes > RAMDISK_SIZE - offset) return -EIO;

    req = skcipher_request_alloc(crypto_tfm, GFP_NOIO);
    if (!req) return -ENOMEM;

    skcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_SLEEP | CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &wait);

    if (bio_op(bio) == REQ_OP_WRITE) ret = process_write_bio(bio, req, &wait);
    else ret = process_read_bio(bio, req, &wait);

    skcipher_request_free(req);
    return ret;
}

static void secure_ram_bio_worker(struct work_struct *work){
    struct secure_ram_work *item = container_of(work, struct secure_ram_work, work);
    struct bio *bio = item->bio;
    int ret;

    mutex_lock(&storage_lock);
    ret = process_bio(bio);
    mutex_unlock(&storage_lock);

    if (ret) bio_io_error(bio);
    else bio_endio(bio);
    kfree(item);
}

static void secure_ram_submit_bio(struct bio *bio){
    struct secure_ram_work *item;
    item = kmalloc(sizeof(*item), GFP_NOIO);

    if (!item) {
        bio_io_error(bio);
        return;
    }

    item->bio = bio;
    INIT_WORK(&item->work, secure_ram_bio_worker);
    if (!queue_work(secure_ram_wq, &item->work)) {
        kfree(item);
        bio_io_error(bio);
    }
}

static const struct block_device_operations secure_ram_fops = {
    .owner = THIS_MODULE,
    .submit_bio = secure_ram_submit_bio,
};

static int __init secure_ram_init(void){
    struct queue_limits lim = {
        .logical_block_size = SECTOR_SIZE_BYTES,
        .physical_block_size = SECTOR_SIZE_BYTES,
    };

    int ret;
    ram_storage = vzalloc(RAMDISK_SIZE);
    if (!ram_storage) {
        pr_err("secure_ram: RAM allocation failed\n");
        return -ENOMEM;
    }

    crypto_tfm = crypto_alloc_skcipher("xts(aes)", 0, 0);

    if (IS_ERR(crypto_tfm)) {
        ret = PTR_ERR(crypto_tfm);
        pr_err("secure_ram: AES-XTS allocation failed: %d\n", ret);
        vfree(ram_storage);
        return ret;
    }

    get_random_bytes(encryption_key, sizeof(encryption_key));
    
    ret = crypto_skcipher_setkey(crypto_tfm, encryption_key, sizeof(encryption_key));
    if (ret) {
        pr_err("secure_ram: AES-XTS key setup failed: %d\n", ret);
        crypto_free_skcipher(crypto_tfm);
        vfree(ram_storage);
        return ret;
    }

    secure_ram_wq = alloc_workqueue("secure_ram_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
    if (!secure_ram_wq) {
    	pr_err("secure_ram: workqueue creation failed\n");
        crypto_free_skcipher(crypto_tfm);
        vfree(ram_storage);
        return -ENOMEM;
    }

    ram_major = register_blkdev(0, DEVICE_NAME);
    if (ram_major < 0) {
        ret = ram_major;
        destroy_workqueue(secure_ram_wq);

        crypto_free_skcipher(crypto_tfm);
        vfree(ram_storage);
        return ret;
    }

    ram_disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
    if (IS_ERR(ram_disk)) {
        ret = PTR_ERR(ram_disk);

        unregister_blkdev(ram_major, DEVICE_NAME);
        destroy_workqueue(secure_ram_wq);
        crypto_free_skcipher(crypto_tfm);

        vfree(ram_storage);
        return ret;
    }

    ram_disk->major = ram_major;
    ram_disk->first_minor = 0;
    ram_disk->minors = 1;
    ram_disk->fops = &secure_ram_fops;
    ram_disk->private_data = ram_storage;
    ram_disk->flags = GENHD_FL_NO_PART;

    snprintf(ram_disk->disk_name, DISK_NAME_LEN, DEVICE_NAME);
    set_capacity(ram_disk, RAMDISK_SIZE >> SECTOR_SHIFT);

    ret = add_disk(ram_disk);
    if (ret) {
        put_disk(ram_disk);

        unregister_blkdev(ram_major, DEVICE_NAME);
        destroy_workqueue(secure_ram_wq);
        crypto_free_skcipher(crypto_tfm);

        vfree(ram_storage);
        return ret;
    }

    debug_blob.data = ram_storage;
    debug_blob.size = RAMDISK_SIZE;
    debug_dir = debugfs_create_dir(DEVICE_NAME, NULL);
    
    if (!IS_ERR_OR_NULL(debug_dir)) {
        if (IS_ERR_OR_NULL(debugfs_create_blob("raw_storage", 0400, debug_dir, &debug_blob)))
            pr_warn("secure_ram: failed to create debugfs raw_storage\n");
    } else {
        debug_dir = NULL;
        pr_warn("secure_ram: debugfs unavailable\n");
    }

    pr_info("secure_ram: encrypted RAM disk loaded\n");
    pr_info("secure_ram: AES-XTS enabled\n");
    pr_info("secure_ram: allocated %lu MB RAM\n", RAMDISK_SIZE / (1024 * 1024));
    pr_info("secure_ram: device = /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit secure_ram_exit(void){
    del_gendisk(ram_disk);

    flush_workqueue(secure_ram_wq);
    
    debugfs_remove_recursive(debug_dir);
    put_disk(ram_disk);

    unregister_blkdev(ram_major, DEVICE_NAME);
    destroy_workqueue(secure_ram_wq);
    crypto_free_skcipher(crypto_tfm);

	memzero_explicit(encryption_key, sizeof(encryption_key));
    vfree(ram_storage);
    pr_info("secure_ram: encrypted RAM disk unloaded\n");
}

module_init(secure_ram_init);
module_exit(secure_ram_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Abhijeet");
MODULE_DESCRIPTION("In-memory encrypted virtual RAM-disk block driver");

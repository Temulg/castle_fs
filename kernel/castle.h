#ifndef __CASTLE_H__
#define __CASTLE_H__

/* Disk layout related structures */
struct castle_disk_block {
    uint32_t disk;
    uint32_t block;
};
typedef struct castle_disk_block c_disk_blk_t;
#define INVAL_DISK_BLK          ((c_disk_blk_t){0,0})
#define DISK_BLK_INVAL(_blk)    (((_blk).block == 0) && ((_blk).disk == 0))
#define DISK_BLK_EQUAL(_blk1, _blk2) (((_blk1).disk == (_blk2).disk) && \
                                      ((_blk1).block == (_blk2).block)) 

struct castle_slave_superblock {
    uint32_t magic1;
    uint32_t magic2;
    uint32_t magic3;
    uint32_t uuid;
    uint32_t free;
    uint32_t size; /* In blocks */
};

struct castle_fs_superblock {
    uint32_t magic1;
    uint32_t magic2;
    uint32_t magic3;
    uint32_t salt;
    uint32_t peper;
    uint32_t fwd_tree_disk1;
    uint32_t fwd_tree_block1;
    uint32_t fwd_tree_disk2;
    uint32_t fwd_tree_block2;
    uint32_t rev_tree_disk1;
    uint32_t rev_tree_block1;
    uint32_t rev_tree_disk2;
    uint32_t rev_tree_block2;
};

#define NODE_HEADER        0x180

#define FTREE_SLOT_LEAF       0x1
#define FTREE_SLOT_NODE       0x2
#define FTREE_SLOT_NODE_LAST  0x3
#define FTREE_SLOT_IS_NODE(_slot)       (((_slot)->type == FTREE_SLOT_NODE) || \
                                         ((_slot)->type == FTREE_SLOT_NODE_LAST))
#define FTREE_SLOT_IS_NODE_LAST(_slot)   ((_slot)->type == FTREE_SLOT_NODE_LAST)
#define FTREE_SLOT_IS_LEAF(_slot)        ((_slot)->type == FTREE_SLOT_LEAF) 

#define INVAL_BLK          ((uint32_t)-1)
#define MAX_BLK            ((uint32_t)-2)
#define BLK_INVAL(_blk)    ((_blk) == INVAL_BLK)

struct castle_ftree_slot {
    uint32_t     type;
    uint32_t     block;
    uint32_t     version;
    c_disk_blk_t cdb;
};

#define FTREE_NODE_SLOTS  ((PAGE_SIZE - NODE_HEADER)/sizeof(struct castle_ftree_slot))
struct castle_ftree_node {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;
    uint32_t used;
    uint8_t  __pad[NODE_HEADER - 16];
    struct castle_ftree_slot slots[FTREE_NODE_SLOTS];
};

struct castle_vtree_node_slot {
    uint32_t     version_nr;
    c_disk_blk_t cdb;
};

struct castle_vtree_leaf_slot {
    uint32_t     version_nr;
    uint32_t     parent;
    uint32_t     size;
    c_disk_blk_t cdb;
};

#define VTREE_SLOT_LEAF       0x1
#define VTREE_SLOT_NODE       0x2
#define VTREE_SLOT_NODE_LAST  0x3
#define VTREE_SLOT_IS_NODE(_slot)       (((_slot)->type == VTREE_SLOT_NODE) || \
                                         ((_slot)->type == VTREE_SLOT_NODE_LAST))
#define VTREE_SLOT_IS_NODE_LAST(_slot)   ((_slot)->type == VTREE_SLOT_NODE_LAST)
#define VTREE_SLOT_IS_LEAF(_slot)        ((_slot)->type == VTREE_SLOT_LEAF) 

struct castle_vtree_slot {
    uint32_t type;
    union {
        struct castle_vtree_node_slot node;
        struct castle_vtree_leaf_slot leaf;
    };
};

#define VTREE_NODE_SLOTS  ((PAGE_SIZE - NODE_HEADER)/sizeof(struct castle_vtree_slot))
struct castle_vtree_node {
    /* On disk representation of the node */
    uint32_t magic;
    uint32_t version; 
    uint32_t capacity;
    uint32_t used;
    struct castle_vtree_slot slots[VTREE_NODE_SLOTS]; 
    /* Pointers to structures in memory. Invalid for the leaf children */
    struct castle_vtree_node *children[VTREE_NODE_SLOTS];
};

/* IO related structures */
typedef struct castle_bio {
    struct bio            *bio;
    void                  *c_bvecs; 
    atomic_t               remaining;
    int                    err;
} c_bio_t;

typedef struct castle_bio_vec {
    c_bio_t            *c_bio;
    struct page        *page;
    struct work_struct  work;
    /* What block,version do we want to read */
    sector_t            block;
    uint32_t            version;
    /* Updated as the IO progresses */
    union {
        struct castle_ftree_node *node;
        c_disk_blk_t cdb;
    };
} c_bvec_t;
#define c_bvec_data_dir(_c_bvec)    bio_data_dir((_c_bvec)->c_bio->bio)

/* First class structures */
struct castle {
    struct kobject kobj;
};

struct castle_volumes {
    struct kobject kobj;
};

struct castle_slave {
    uint32_t                        id;
    struct kobject                  kobj;
    struct list_head                list;
    struct block_device            *bdev;
    struct castle_slave_superblock  cs_sb;
};

struct castle_slaves {
    struct kobject   kobj;
    struct list_head slaves;
};

struct castle_device {
    struct kobject    kobj;
    spinlock_t        lock;
    struct list_head  list;
    struct gendisk   *gd;
    int               users;
    int               sysfs_registered;

    uint32_t          version;
};

struct castle_devices { 
    struct kobject kobj;
    int major;
    struct list_head devices;
};

extern struct castle             castle;
extern struct castle_volumes     castle_volumes;
extern struct castle_slaves      castle_slaves;
extern struct castle_devices     castle_devices;

extern struct workqueue_struct  *castle_wq;

extern struct castle_vtree_node *castle_vtree_root;

/* Various utilities */
#define C_BLK_SHIFT                    (12) 
#define C_BLK_SIZE                     (1 << C_BLK_SHIFT)
#define disk_blk_to_offset(_cdb)     ((_cdb).block * C_BLK_SIZE)

void castle_bio_data_io_end(c_bvec_t *c_bvec, int err);
void castle_bio_data_io(c_bvec_t *c_bvec);

struct castle_device* castle_device_init       (struct castle_vtree_leaf_slot *version);
void                  castle_device_free       (struct castle_device *cd);
struct castle_device* castle_device_find       (dev_t dev);
struct castle_slave*  castle_claim             (uint32_t new_dev);
struct castle_slave*  castle_slave_find_by_id  (uint32_t id);
struct castle_slave*  castle_slave_find_by_uuid(uint32_t uuid);
struct castle_slave*  castle_slave_find_by_block(c_disk_blk_t cdb);
void                  castle_release           (struct castle_slave *cs);
int                   castle_fs_init           (void);

#endif /* __CASTLE_H__ */

#include <linux/module.h>
#include <linux/workqueue.h> 
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/hardirq.h>

#include "castle.h"
#include "castle_versions.h"

//#define DEBUG
#ifndef DEBUG
#define debug(_f, ...)  ((void)0)
#else
#define debug(_f, _a...)  (printk("%s:%.4d: " _f, __FILE__, __LINE__ , ##_a))
#endif

#define INVAL_VERSION       ((version_t)-1) 
#define VERSION_INVAL(_v)   ((_v) == INVAL_VERSION) 

static struct kmem_cache *castle_versions_cache = NULL;

#define CASTLE_VERSIONS_HASH_SIZE       (1000)
static    DEFINE_SPINLOCK(castle_versions_hash_lock);
static struct list_head  *castle_versions_hash  = NULL;
static          LIST_HEAD(castle_versions_init_list);

struct castle_version {
    /* Various tree links */
    version_t                  version;
    union {
        version_t              parent_v;  /* Vaild if !inited */
        struct castle_version *parent;    /* Vaild if  inited */
    };
    struct castle_version     *first_child;
    struct castle_version     *next_sybling;

    /* Aux data */
    version_t    o_order;
    version_t    r_order;
    c_disk_blk_t ftree_root;
    uint32_t     size;

    /* Lists for storing versions the hash table & the init list*/
    struct list_head hash_list; 
    int              inited;
    struct list_head init_list;
};


/***** Hash table & init list *****/
static int castle_versions_hash_idx(version_t version)
{
    return (version % CASTLE_VERSIONS_HASH_SIZE);
} 

static void castle_versions_hash_add(struct castle_version *v)
{
    int idx = castle_versions_hash_idx(v->version);
    
    spin_lock(&castle_versions_hash_lock);
    list_add(&v->hash_list, &castle_versions_hash[idx]);
    spin_unlock(&castle_versions_hash_lock);
}

static struct castle_version* __castle_versions_hash_get(version_t version)
{
    struct castle_version *v;
    struct list_head *l;
    int idx = castle_versions_hash_idx(version); 

    list_for_each(l, &castle_versions_hash[idx])
    {
        v = list_entry(l, struct castle_version, hash_list);
        if(v->version == version)
            return v;
    }

    return NULL;
} 
static void castle_versions_hash_destroy(void)
{
    struct list_head *l, *t;
    struct castle_version *v;
    int i;

    spin_lock(&castle_versions_hash_lock);
    for(i=0; i<CASTLE_VERSIONS_HASH_SIZE; i++)
    {
        list_for_each_safe(l, t, &castle_versions_hash[i])
        {
            list_del(l);
            v = list_entry(l, struct castle_version, hash_list);
            kmem_cache_free(castle_versions_cache, v);
        }
    }
    spin_unlock(&castle_versions_hash_lock);
}

static void castle_versions_init_add(struct castle_version *v)
{
    spin_lock(&castle_versions_hash_lock);
    v->inited = 0;
    list_add(&v->init_list, &castle_versions_init_list);
    spin_unlock(&castle_versions_hash_lock);
}


/***** External functions *****/
int castle_version_add(version_t version, 
                       version_t parent, 
                       c_disk_blk_t ftree_root,
                       uint32_t  size)
{
    struct castle_version *v;

    v = kmem_cache_alloc(castle_versions_cache, GFP_KERNEL);
    if(!v) return -ENOMEM;
    debug("Adding: (v, p)=(%d,%d)\n", version, parent);

    v->version      = version;
    v->parent_v     = parent;
    v->first_child  = NULL; 
    v->next_sybling = NULL; 
    v->o_order      = INVAL_VERSION;
    v->r_order      = INVAL_VERSION;
    v->ftree_root   = ftree_root;
    v->size         = size;
    INIT_LIST_HEAD(&v->hash_list);
    INIT_LIST_HEAD(&v->init_list);

    castle_versions_hash_add(v);
    /* Initialise version 0 (root version) fully */ 
    if(v->version == 0)
    {
        v->parent       = NULL;
        v->first_child  = NULL; /* This will be updated later */
        v->next_sybling = NULL;
        v->inited       = 1;

    } else
    {
        /* Defer the initialisation until all the parent 
           nodes have been collected */
        castle_versions_init_add(v);
    }

    return 0;
}

int castle_version_snap_get(version_t version, 
                            c_disk_blk_t *ftree_root,
                            uint32_t *size)
{
    struct castle_version *v;
    int ret = -EINVAL;

    spin_lock(&castle_versions_hash_lock);
    v = __castle_versions_hash_get(version);
    if(v) 
    {
        if(ftree_root) *ftree_root = v->ftree_root;
        if(size)       *size       = v->size;
        ret = 0;
    }
    spin_unlock(&castle_versions_hash_lock);

    return ret;
} 

void castle_versions_process(void)
{
    struct castle_version *v, *p, *n;
    version_t id;
    int down;

    spin_lock(&castle_versions_hash_lock);
    /* Start processing elements from the init list, one at the time */
    while(!list_empty(&castle_versions_init_list))
    {
        v = list_first_entry(&castle_versions_init_list, 
                              struct castle_version,
                              init_list);
process_version:        
        /* Remove the element from the list */
        list_del(&v->init_list);
        BUG_ON(v->inited);

        /* Find it's parent, and check if it's been inited already */
        p = __castle_versions_hash_get(v->parent_v);
        BUG_ON(!p);
        debug("Processing version: %d, parent: %d\n", v->version, p->version);
        /* If the parent hasn't been initialised yet, initialise it instead */
        if(!p->inited)
        {
            /* Re-add v back to the init list.
               Because the element is added to the front of the list O(n) is guaranteed.
               This is because after following parent pointers up to the root of the tree,
               we will come back down initialising all children on the path. */
            list_add(&v->init_list, &castle_versions_init_list);
            /* Set v to the parent */
            v = p;
            /* Retry processing, this time starting with the parent. 
               This has to succeed at some point because version 0 
               is guaranteed to be inited */
            goto process_version;
        }
        /* If we got here we know that the parent has been inited */
        debug(" Parent initialised, (v,p)=(%d,%d)\n", v->version, p->version);
        /* Insert v at the start of the sybling list. */
        v->parent       = p;
        v->next_sybling = p->first_child;
        p->first_child  = v;
        //if(v->next_sybling)
          //  debug(" Versions's sybling is version %d\n", v->next_sybling->version);
        /* We are done */
        v->inited = 1;
    }

    /* Now, once the tree has been built, assign the order to the nodes
       We assign two id's to each node. o_order is based on when is the node 
       visited first time in DFS, r_order when the node is visited last. 
       The code below implements non-recursive DFS (we don't have enough stack for
       potentialy deep recursion */  
    v = __castle_versions_hash_get(0); 
    BUG_ON(!v);
    BUG_ON(!v->inited);
    BUG_ON(v->parent);
    id = 0;
    down = 1;
    
    while(v)
    {
        debug("Looking at version: %d\n", v->version);
        n = NULL;
        /* If going down the tree select the next node in the following order
           of preference:
           - first child
           - next sybling
           - parent
           On the way up select:
           - next sybling
           - parent
           Note that the next sybling & parent cases are common to both cases.
           Also, if the parent is selected, make sure 'down' is not set */
        if(down)
        {
            v->o_order = ++id;
            debug("Assigned version=%d o_order %d\n", v->version, v->o_order);
            /* Only attempt to go to the child on the way down the tree */
            n = v->first_child;
            /* Special case for leaf nodes: r_order = o_order */
            if(!n) v->r_order = v->o_order;
        } else
        {
            /* Assign the r order first (the id of the last decendant) */
            v->r_order = id;
            debug("Assigned version=%d r_order %d\n", v->version, v->r_order);
        }
        down = 1;
        if(!n) 
            n = v->next_sybling;
        if(!n) {
            n = v->parent;
            down = 0;
        }
        if(n) debug("Next version is: %d\n", n->version);
        v = n;
    }
    spin_unlock(&castle_versions_hash_lock);
    /* Done. */
}

int castle_version_is_ancestor(version_t candidate, version_t version)
{
    struct castle_version *c, *v;
    int ret;

    spin_lock(&castle_versions_hash_lock);
    v = __castle_versions_hash_get(version);
    c = __castle_versions_hash_get(candidate);
    /* Sanity checks */
    BUG_ON(!v);
    BUG_ON(!v->inited);
    BUG_ON(VERSION_INVAL(v->o_order));
    BUG_ON(!c);
    BUG_ON(!c->inited);
    BUG_ON(VERSION_INVAL(c->o_order));
    BUG_ON(VERSION_INVAL(c->r_order));
    /* c is an ancestor of v if v->o_order is in range c->o_order to c->r_order
       inclusive */
    ret = (v->o_order >= c->o_order) && (v->o_order <= c->r_order);
    spin_unlock(&castle_versions_hash_lock);

    return ret;
}

/***** Init/fini functions *****/
int castle_versions_init(void)
{
    int i, ret;

    ret = -ENOMEM;
    castle_versions_cache = kmem_cache_create("castle_versions",
                                               sizeof(struct castle_version),
                                               0,     /* align */
                                               0,     /* flags */
                                               NULL); /* ctor */
    if(!castle_versions_cache)
    {
        printk("Could not allocate kmem cache for castle versions.\n");
        goto err_out;
    }
    
    castle_versions_hash = 
        kmalloc(sizeof(struct list_head) * CASTLE_VERSIONS_HASH_SIZE,
                GFP_KERNEL); 
    if(!castle_versions_hash)
    {
        printk("Could not allocate versions hash\n");
        goto err_out;
    }
    /* We've allocated everything, we'll succeed after here */
    ret = 0;

    for(i=0; i<CASTLE_VERSIONS_HASH_SIZE; i++)
        INIT_LIST_HEAD(&castle_versions_hash[i]); 

    return ret;

err_out:
    if(castle_versions_cache)
        kmem_cache_destroy(castle_versions_cache);
    if(castle_versions_hash)
        kfree(castle_versions_hash);
    return ret;
}

void castle_versions_fini(void)
{
    castle_versions_hash_destroy();
    kmem_cache_destroy(castle_versions_cache);
    kfree(castle_versions_hash);
}
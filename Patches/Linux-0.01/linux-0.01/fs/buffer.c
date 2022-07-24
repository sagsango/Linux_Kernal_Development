/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

/*
 * BUFFER SIZE = BLOCK SIZE
 * Because buffer is for block read
 */
#if (BUFFER_END & 0xfff)
#error "Bad BUFFER_END value"
#endif

#if (BUFFER_END > 0xA0000 && BUFFER_END <= 0x100000)
#error "Bad BUFFER_END value"
#endif

extern int end;
struct buffer_head * start_buffer = (struct buffer_head *) &end;
struct buffer_head * hash_table[NR_HASH]; // buffer list, using hash of (device, block)
static struct buffer_head * free_list; // free list of buffers aviable.
static struct task_struct * buffer_wait = NULL;
int NR_BUFFERS = 0;


// locking
static inline void wait_on_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	sti();
}


// write back dirty ( inodes + buffer )
// into the disk/dev
// NOTE: inodes are in inode_table
int sys_sync(void)
{
	int i;
	struct buffer_head * bh;

	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_on_buffer(bh);
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

// write back the dirty blocks
// of dev/disk
static int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]


/*
 * remove the in memory buffer from
 * hash queue & free list
 */
static inline void remove_from_queues(struct buffer_head * bh)
{
/*
#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]
*/

/* remove from hash-queue */
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	if (hash(bh->b_dev,bh->b_blocknr) == bh) // If its the head
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
/* remove from free list */
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh) // If its the first
		free_list = bh->b_next_free;
}




/*
 * insert a buffer into in memory 
 * free buffers + hash queue if ( have div + block )
 */
static inline void insert_into_queues(struct buffer_head * bh)
{
/*
#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]
*/

/* put at end of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}



/*
 * find buffer for given dev block
 * from hash table list
 */
static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 *
 * XXX:XXX
 *     Check the buff in hash table list,
 *     If  buff is not there return NULL
 *     wait if someone is using it
 *     check if it has not been reclaimed
 *     return 
 */
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

repeat:
	if (!(bh=find_buffer(dev,block)))
		return NULL;
	bh->b_count++;
	wait_on_buffer(bh);
	if (bh->b_dev != dev || bh->b_blocknr != block) {
		brelse(bh);
		goto repeat;
	}
	return bh;
}


/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * XXX:XXX
 *     Check if block is present in the hash table list, return
 *     Go to the free list of buffer, and find a free block
 */
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp;

repeat:
	if (tmp=get_hash_table(dev,block))
		return tmp;
	tmp = free_list;
	do {
		if (!tmp->b_count) {
			wait_on_buffer(tmp);	/* we still have to wait */
			if (!tmp->b_count)	/* on it, it might be dirty */
				break;
		}
		tmp = tmp->b_next_free;
	} while (tmp != free_list || (tmp=NULL));
	/* Kids, don't try THIS at home ^^^^^. Magic */
	if (!tmp) {
		printk("Sleeping on free buffer ..");
		sleep_on(&buffer_wait);
		printk("ok\n");
		goto repeat;
	}
	tmp->b_count++;
	remove_from_queues(tmp);
/*
 * Now, when we know nobody can get to this node (as it's removed from the
 * free list), we write it out. We can sleep here without fear of race-
 * conditions.
 */
	if (tmp->b_dirt)
		sync_dev(tmp->b_dev); // XXX:XXX WE WRITE BACK THE BUFFER INTO DISK, ONLY WHEN WE NEED THAT 
                          //         BUFFER, WHICH IS NOT BEING USED, BUT IS DIRTY.
/* update buffer contents */
	tmp->b_dev=dev;
	tmp->b_blocknr=block;
	tmp->b_dirt=0;
	tmp->b_uptodate=0;
/* NOTE!! While we possibly slept in sync_dev(), somebody else might have
 * added "this" block already, so check for that. Thank God for goto's.
 */
	if (find_buffer(dev,block)) {
		tmp->b_dev=0;		/* ok, someone else has beaten us, (allocated already) */
		tmp->b_blocknr=0;	/* to it - free this block and */
		tmp->b_count=0;		/* try again */
		insert_into_queues(tmp);
		goto repeat;
	}
/* and then insert into correct position */
	insert_into_queues(tmp);
	return tmp;
}


// XXX:XXX We write back dirty block, on
//         relaimnation of the block
//         by someone
//
//         brelse is just decrementing refcount
//         by 1
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf); // XXX:XXX Lock
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	wake_up(&buffer_wait); // XXX:XXX Unlock
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 *
 *
 * XXX:XXX
 *     read the block,
 */
struct buffer_head * bread(int dev,int block)
{
	struct buffer_head * bh;

	if (!(bh=getblk(dev,block)))
		panic("bread: getblk returned NULL\n");
	if (bh->b_uptodate)
		return bh;
	ll_rw_block(READ,bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}


/*
 * 1/
 * Make all the buffers clear
 * and make a free block list
 * Which are just inside Raw Ram.
 * from start location to end.
 * 
 * Free block list are just, an array
 * in the begining.
 *
 * They are not even in the free list of blocks
 *
 * 2/
 * Make all hash list NULL
 */
void buffer_init(void)
{
	struct buffer_head * h = start_buffer;
	void * b = (void *) BUFFER_END;
	int i;

	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) {
		h->b_dev = 0;
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b;
		h->b_prev_free = h-1;
		h->b_next_free = h+1;
		h++;
		NR_BUFFERS++;
		if (b == (void *) 0x100000)
			b = (void *) 0xA0000;
	}
	h--;
	free_list = start_buffer;
	free_list->b_prev_free = h;
	h->b_next_free = free_list;
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	

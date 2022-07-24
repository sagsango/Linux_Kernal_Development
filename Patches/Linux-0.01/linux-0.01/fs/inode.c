#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

/* XXX:XXX write dirty inode in disk */
void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);
	}
}

static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");
	if (block<7) {
		if (create && !inode->i_zone[block])
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	block -= 7;
	if (block<512) {
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		i = ((unsigned short *) (bh->b_data))[block];
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
	block -= 512;
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}

/*
 * XXX:
 * Use 1: In do_execve()
 * (inode struct + all the data) is
 * loded as executable in process's
 * page (RAM), now iput(), will
 * free only the inode not
 * the page resource.
 *
 * Use 2:
 * Free the pipe inode + pages for it.
 */
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	if (inode->i_pipe) {
		wake_up(&inode->i_wait);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	if (!inode->i_dev || inode->i_count>1) {
		inode->i_count--;
		return;
	}
repeat:
	if (!inode->i_nlinks) {
		truncate(inode);
		free_inode(inode); // Free the inode in inode-table
		return;
	}
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
                        // XXX: Write back into disk
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	return;
}

static volatile int last_allocated_inode = 0;



/*
 * XXX:XXX
 * Get in memory inode structure
 * We have to check in inode_table
 * there are NR_INODE = 32, slots
 * we have to find empty slot
 *
 *
 * See how we find empty slot
 * We travers in the ring, for it
 */
struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	int inr;

	while (1) { // XXX:XXX loop untill 
              // there is a free slot
		inode = NULL;
		inr = last_allocated_inode;
		do {
			if (!inode_table[inr].i_count) {
				inode = inr + inode_table;
				break;
			}
			inr++;
			if (inr>=NR_INODE)
				inr=0;
		} while (inr != last_allocated_inode);
		if (!inode) {
			for (inr=0 ; inr<NR_INODE ; inr++)
				printk("%04x: %6d\t",inode_table[inr].i_dev,
					inode_table[inr].i_num);
			panic("No free inodes in mem");
		}
		last_allocated_inode = inr;
		wait_on_inode(inode);
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
		if (!inode->i_count)
			break;
	}
	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

/*XXX:XXX
 *    Whole page have been given 
 *    to a pipe inode.
 *
 *    NOTE: pipe inode is only 
 *    in memory structure
 */
struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	inode->i_pipe = 1;
	return inode;
}

/*
 * XXX:XXX
 *     inode  = (dev + nr)
 *     for give (dev + nr) return an in memory
 *     inode structure
 */
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode(); // XXX:XXX get in a memory structure inode, from inode table 

  // XXX:XXX Search if inode already in inode_table
	inode = inode_table;
	while (inode < NR_INODE+inode_table) {
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		wait_on_inode(inode);
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		inode->i_count++;
		if (empty)
			iput(empty);
		return inode; // XXX:XXX return alredy present inode
	}
	if (!empty)
		return (NULL);
  // XXX:XXX If inode is not already preset 
  //         return an empty inode.
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
  // XXX:XXX 
  // read the d_inode
  // means inode info
  // present in the disk
	read_inode(inode);
	return inode;
}

static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	sb=get_super(inode->i_dev);
  // See O(1) access to reach inode block
  // inode are just index of block
  // after the superblock
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
	brelse(bh);
	unlock_inode(inode);
}

/*
 * XXX:
 * Write inode, in the disk.
 * Which the help of superblock
 */
static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	sb=get_super(inode->i_dev);
  // See O(1) access to reach (inode block) + (offset inside the block)
  // inode are just index of block
  // after the superblock
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	((struct d_inode *)bh->b_data) // block where this inode entry resides
		[(inode->i_num-1)%INODES_PER_BLOCK] // offset inside the block where this inode entry resides 
			= *(struct d_inode *)inode;
	bh->b_dirt=1;
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}

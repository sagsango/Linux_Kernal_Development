/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__("btsl %2,%3\n\tsetb %%al":"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__("btrl %2,%3\n\tsetnb %%al":"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \
	"notl %%eax\n\t" \
	"bsfl %%eax,%%edx\n\t" \
	"je 2f\n\t" \
	"addl %%edx,%%ecx\n\t" \
	"jmp 3f\n" \
	"2:\taddl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n" \
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})


  /*
   *
   * XXX:
   *      Dev will be having one file system.
   *      (Files are just way of organization
   *       of the disk space efficienlty, why
   *       we need to, and if need we can do
   *       that.)
   *
   *      Superblock:
   *      All the file system related info will
   *      be on the superblock.
   *      You can access inodes bitmap and block bitmap
   *      Bitmap is index of of inode number + block number 
   *
   *      You have to store inode + data_block
   *      we use inode_table:
   *        superblock -> s_imap -> inode -> buffer -> block.
   *      we use buffer here for block: 
   *        superblock -> s_zmap -> data_block -> buffer -> block.
   *
   *      disk read/write basic unit in block.
   *      file = inode + data block.
   *             inode will be stored on disk block
   *             data block will be stored on disk block
   *
   *      So we have a lot of buffer so we
   *      create hash of (dev,block) and
   *      put that in hash_key buffer list.
   *      see :
   *          get_hash_table(dev,block)
   *          hash_table[NR_HASH]
   *
   */


  /*XXX:XXX
   *    Free block of the given device
   *    Free the block + update the s_zmap( Zone map)
   */
void free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;

  // clear the block
	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	bh = get_hash_table(dev,block);
	if (bh) {
		if (bh->b_count != 1) {
			printk("trying to free block (%04x:%d), count=%d\n",
				dev,block,bh->b_count);
			return;
		}
		bh->b_dirt=0;
		bh->b_uptodate=0;
		brelse(bh);
	}
	block -= sb->s_firstdatazone - 1 ;

  // update the s_zmap
  // XXX:XXX ????? how - struct buffer_head * s_zmap[8]; 
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		panic("free_block: bit already cleared");
	}
	sb->s_zmap[block/8192]->b_dirt = 1;
}

/*XXX:XXX
 *    Allocate the block in given device,
 *    Aloocate the block + update the s_zmap
 */
int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

  // get free block from map
	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
  // update the s_zmap
  // XXX:XXX ????? how - struct buffer_head * s_imap[8];
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	bh->b_dirt = 1;
	j += i*8192 + sb->s_firstdatazone-1;
	if (j >= sb->s_nzones)
		return 0;
  // load the block
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
  // update in memory structure
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}

/*
 * XXX:XXX 
 *     Free a given inode
 *     free the inode + update the s_imap
 */
void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

  // check if free op is valid
	if (!inode)
		return;
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks)
		panic("trying to free inode with links");
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
  // update the map
	if (!(bh=sb->s_imap[inode->i_num>>13]))
		panic("nonexistent imap in superblock");
	if (clear_bit(inode->i_num&8191,bh->b_data))
		panic("free_inode: bit already cleared");
	bh->b_dirt = 1;
  // clean in memory structure
	memset(inode,0,sizeof(*inode));
}

/*
 * XXX:XXX
 *     Get a inode from given device
 *     get in memory structure + \
 *     check free node from superblock map + \
 *     update map after taking
 */
struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;

  // get in memory structure
	if (!(inode=get_empty_inode()))
		return NULL;
	if (!(sb = get_super(dev)))
		panic("new_inode with unknown device");
  // get free inode form s_imap
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
  // update the s_imap
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
  // update the in memory structure
	bh->b_dirt = 1;
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_dirt=1;
	inode->i_num = j + i*8192;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}

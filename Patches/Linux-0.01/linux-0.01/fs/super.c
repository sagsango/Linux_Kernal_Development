/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr,addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

struct super_block super_block[NR_SUPER];
/* XXX:XXX
 * only during mount of the device, we 
 * fill the superblock entry in our
 * global super_block table.
 *
 * mount the device = get the super block of the device
 */
struct super_block * do_mount(int dev)
{
	struct super_block * p;
	struct buffer_head * bh;
	int i,block;

	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++ )
		if (!(p->s_dev))
			break;
	p->s_dev = -1;		/* mark it in use */
	if (p >= &super_block[NR_SUPER])
		return NULL;
  /* XXX:XXX
   *     first block of device contains the super block
   */
	if (!(bh = bread(dev,1)))
		return NULL;
	*p = *((struct super_block *) bh->b_data);
	brelse(bh); // XXX: release the block after loading the 
              //      super_block in memory, global table
              //
	if (p->s_magic != SUPER_MAGIC) {
		p->s_dev = 0;
		return NULL;
	}
	for (i=0;i<I_MAP_SLOTS;i++)
		p->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		p->s_zmap[i] = NULL;
	block=2;
  /*XXX:XXX
   *    device_blocks = [ 1:super_block, X: s_imap blocks, Y: s_zmap_blocks ]
   *    NOTE, imap = inode map
   *          zmap = zone map
   *          see struct super_block
   */
	for (i=0 ; i < p->s_imap_blocks ; i++)
		if (p->s_imap[i]=bread(dev,block))
			block++;
		else
			break;
	for (i=0 ; i < p->s_zmap_blocks ; i++)
		if (p->s_zmap[i]=bread(dev,block))
			block++;
		else
			break;

  // XXX: If total blocks read, does not equal to
  //      ( super_bloks:1  + s_imap_blocks + s_zmap_blocks )
  //      something wrong happened, mark mount as fail and
  //      clean the current super_block, struct.
	if (block != 2+p->s_imap_blocks+p->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(p->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(p->s_zmap[i]);
		p->s_dev=0;
		return NULL;
	}

  // XXX:XXX
  //     imap = which inodes in the device are free
  //     zmap = which zones in the device are free
  //     see mount_root() for more info.
	p->s_imap[0]->b_data[0] |= 1;
	p->s_zmap[0]->b_data[0] |= 1;
	p->s_dev = dev;
	p->s_isup = NULL;
	p->s_imount = NULL;
	p->s_time = 0;
	p->s_rd_only = 0;
	p->s_dirt = 0; // until we did not write the super_block it is not dirty.
	return p;
}


/*
 * XXX:XXX
 *     mount the root = (get super block of root dev) + (load first inode)
 */
void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");

  //clear the file table
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0;

  // clear the super block table
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++)
		p->s_dev = 0;

  // mount root device ( geting the super block )
	if (!(p=do_mount(ROOT_DEV)))
		panic("Unable to mount root");

  // get the first inode of the device. 
 	if (!(mi=iget(ROOT_DEV,1)))
		panic("Unable to read root i-node");
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
  // update the superblock with, inode ptr
	p->s_isup = p->s_imount = mi;
	current->pwd = mi;
	current->root = mi;
	free=0;
  // count free s_zmap
	i=p->s_nzones;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
  // count free s_imap
	free=0;
	i=p->s_ninodes+1;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}

// XXX:XXX ????? ROOT_DEV is getting mounted during boot, when other devices are getting mounted?

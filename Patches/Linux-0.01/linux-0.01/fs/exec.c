#include <errno.h>
#include <sys/stat.h>
#include <a.out.h>

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/segment.h>

extern int sys_exit(int exit_code);
extern int sys_close(int fd);

/*
 * XXX:XXX RAM for a process given as pages
 *
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
#define MAX_ARG_PAGES 32

#define cp_block(from,to) \
__asm__("pushl $0x10\n\t" \
	"pushl $0x17\n\t" \
	"pop %%es\n\t" \
	"cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	"pop %%es" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * 
 * read_head() reads blocks 1-6 (not 0). Block 0 has already been
 * read for header information.
 *
 * XXX:XXX
 *     See above comment, so inode is just 8 blocks.
 *     0      :  contains the header info(see in do_execve().
 *     1-6    :  direct block
 *     7      :  1st level indirect block
 *     8      :  2nd level indirect block
 *
 *
 * XXX:XXX 
 *     load first 6 blocks of
 *     the source code inode
 *     into the process_address_apace/page(RAM).
 *
 *     ????? Are there direct blocks
 *           Because bellow we are 
 *           reading the indirect blocks
 */
int read_head(struct m_inode * inode,int blocks)
{
	struct buffer_head * bh;
	int count;

	if (blocks>6)
		blocks=6;
	for(count = 0 ; count<blocks ; count++) {
		if (!inode->i_zone[count+1])
			continue;
		if (!(bh=bread(inode->i_dev,inode->i_zone[count+1])))
			return -1;
		cp_block(bh->b_data,count*BLOCK_SIZE);
		brelse(bh);
	}
	return 0;
}


/*
 * XXX:XXX
 *     Read 1st level indirect blocks into process address_space/pages/memory.
 *     dev   : device number
 *     ind   : block with contais
 *             the block number of
 *             the indirect blocks
 *     size  : total size, of the
 *             data to be read from
 *             the indirect blocks
 *
 * load the ceil(size/BLOCK_SIZE) number of blocks
 * into the RAM (Pages, vertual address space),
 * starting from the given offset of the Vertual Address Space
 * of the process
 *
 */
int read_ind(int dev,int ind,long size,unsigned long offset)
{
	struct buffer_head * ih, * bh;
	unsigned short * table,block;

	if (size<=0)
		panic("size<=0 in read_ind");
	if (size>512*BLOCK_SIZE)
		size=512*BLOCK_SIZE;
	if (!ind)
		return 0;
	if (!(ih=bread(dev,ind)))
		return -1;
	table = (unsigned short *) ih->b_data; // store the block number into table var
                                         // do first entry of the block is block number
                                         // because we are just inreasething that var
                                         // by 1 to 
	while (size>0) {
		if (block=*(table++))
			if (!(bh=bread(dev,block))) {
				brelse(ih);
				return -1;
			} else {
				cp_block(bh->b_data,offset);
				brelse(bh);
			}
		size -= BLOCK_SIZE;
		offset += BLOCK_SIZE;
	}
	brelse(ih);
	return 0;
}

/*
 * read_area() reads an area into %fs:mem.
 *
 *
 * XXX:XXX
 *     Read the executable into 
 *     process address_space/RAM/memory
 */
int read_area(struct m_inode * inode,long size)
{
	struct buffer_head * dind;
	unsigned short * table;
	int i,count;

  // First read 1-6 direct blocks
	if ((i=read_head(inode,(size+BLOCK_SIZE-1)/BLOCK_SIZE)) ||
	    (size -= BLOCK_SIZE*6)<=0)
		return i;
  // Then read 1st level indirect blocks, which block number
  // is in direct block number 7, i_zone[7] block.
  // there can be 512 entries of indirect blocks.
	if ((i=read_ind(inode->i_dev,inode->i_zone[7],size,BLOCK_SIZE*6)) ||
	    (size -= BLOCK_SIZE*512)<=0)
		return i;
	if (!(i=inode->i_zone[8]))
		return 0;
  // read direct 8th block, which contains the 
  // 2nd - level indirect block entries.
	if (!(dind = bread(inode->i_dev,i)))
		return -1;
	table = (unsigned short *) dind->b_data;
	for(count=0 ; count<512 ; count++)
		if ((i=read_ind(inode->i_dev,*(table++),size,
		    BLOCK_SIZE*(518+count))) || (size -= BLOCK_SIZE*512)<=0)
			return i;
  // now executable size is greater than
  // (6 * BLOCK_SIZE) + (512 * BLOCK_SIZE) + (512 * 512 * BLOCK_SIZE) 
	panic("Impossibly long executable");
}

/* XXX:XXX ?????
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */
static unsigned long * create_tables(char * p,int argc,int envc)
{
	unsigned long *argv,*envp;
	unsigned long * sp;

	sp = (unsigned long *) (0xfffffffc & (unsigned long) p);
	sp -= envc+1;
	envp = sp;
	sp -= argc+1;
	argv = sp;
	put_fs_long((unsigned long)envp,--sp);
	put_fs_long((unsigned long)argv,--sp);
	put_fs_long((unsigned long)argc,--sp);
	while (argc-->0) {
		put_fs_long((unsigned long) p,argv++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,argv);
	while (envc-->0) {
		put_fs_long((unsigned long) p,envp++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,envp);
	return sp;
}

/*
 * count() counts the number of arguments/envelopes
 */
static int count(char ** argv)
{
	int i=0;
	char ** tmp;

	if (tmp = argv)
		while (get_fs_long((unsigned long *) (tmp++)))
			i++;

	return i;
}

/* XXX ?????
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 */
static unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
		unsigned long p)
{
	int len,i;
	char *tmp;

	while (argc-- > 0) {
		if (!(tmp = (char *)get_fs_long(((unsigned long *) argv)+argc)))
			panic("argc is wrong");
		len=0;		/* remember zero-padding */
		do {
			len++;
		} while (get_fs_byte(tmp++));
		if (p-len < 0)		/* this shouldn't happen - 128kB */
			return 0;
		i = ((unsigned) (p-len)) >> 12;
		while (i<MAX_ARG_PAGES && !page[i]) {
			if (!(page[i]=get_free_page()))
				return 0;
			i++;
		}
		do {
			--p;
			if (!page[p/PAGE_SIZE])
				panic("nonexistent page in exec.c");
			((char *) page[p/PAGE_SIZE])[p%PAGE_SIZE] =
				get_fs_byte(--tmp);
		} while (--len);
	}
	return p;
}

/*
 * XXX:XXX
 *     process's virual memory limit
 */
static unsigned long change_ldt(unsigned long text_size,unsigned long * page)
{
	unsigned long code_limit,data_limit,code_base,data_base;
	int i;

	code_limit = text_size+PAGE_SIZE -1;
	code_limit &= 0xFFFFF000;
	data_limit = 0x4000000;
	code_base = get_base(current->ldt[1]);
	data_base = code_base;
	set_base(current->ldt[1],code_base);
	set_limit(current->ldt[1],code_limit);
	set_base(current->ldt[2],data_base);
	set_limit(current->ldt[2],data_limit);
/* make sure fs points to the NEW data segment */
	__asm__("pushl $0x17\n\tpop %%fs"::);
	data_base += data_limit;
	for (i=MAX_ARG_PAGES-1 ; i>=0 ; i--) {
		data_base -= PAGE_SIZE;
		if (page[i])
			put_page(page[i],data_base);
	}
	return data_limit;
}

/* XXX: See How process is being created.
 * 'do_execve()' executes a new program.
 */
int do_execve(unsigned long * eip,long tmp,char * filename,
	char ** argv, char ** envp)
{
	struct m_inode * inode;
	struct buffer_head * bh;
	struct exec ex;
	unsigned long page[MAX_ARG_PAGES]; // Page is just noting an array, of index of frame. 
	int i,argc,envc;
	unsigned long p;

	if ((0xffff & eip[1]) != 0x000f)
		panic("execve called from supervisor mode");
	for (i=0 ; i<MAX_ARG_PAGES ; i++)	/* clear page-table */
		page[i]=0;
	if (!(inode=namei(filename)))		/* get executables inode */
		return -ENOENT;
	if (!S_ISREG(inode->i_mode)) {	/* must be regular file */
		iput(inode);
		return -EACCES;
	}
	i = inode->i_mode;
	if (current->uid && current->euid) {
		if (current->euid == inode->i_uid)
			i >>= 6;
		else if (current->egid == inode->i_gid)
			i >>= 3;
	} else if (i & 0111)
		i=1;
	if (!(i & 1)) {
		iput(inode);
		return -ENOEXEC;
	}

// XXX: read first direct block
	if (!(bh = bread(inode->i_dev,inode->i_zone[0]))) {
		iput(inode);
		return -EACCES;
	}
	ex = *((struct exec *) bh->b_data);	/* read exec-header */
	brelse(bh);
	if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
		ex.a_text+ex.a_data+ex.a_bss>0x3000000 ||
		inode->i_size < ex.a_text+ex.a_data+ex.a_syms+N_TXTOFF(ex)) {
		iput(inode);
		return -ENOEXEC;
	}
	if (N_TXTOFF(ex) != BLOCK_SIZE)
		panic("N_TXTOFF != BLOCK_SIZE. See a.out.h.");

  //XXX: copy argv, envp into pages.
  //     if requied get_free_page() and
  //     init ref in pages[] of process
	argc = count(argv);
	envc = count(envp);
	p = copy_strings(envc,envp,page,PAGE_SIZE*MAX_ARG_PAGES-4);
	p = copy_strings(argc,argv,page,p) ;

  //XXX: Not successfull free the pages
  //     held by the process, and fail execve
	if (!p) {
		for (i=0 ; i<MAX_ARG_PAGES ; i++)
			free_page(page[i]);
		iput(inode); // Free only inode struct in case of non pipe inode
                 // To free pages we have called free_page explicitly.
		return -1;
	}


/* OK, This is the point of no return */


// XXX:make all signal_handler function to NULL
	for (i=0 ; i<32 ; i++)
		current->sig_fn[i] = NULL;
// XXX: close the fd, if close on exec is true  
	for (i=0 ; i<NR_OPEN ; i++)
		if ((current->close_on_exec>>i)&1)
			sys_close(i);
	current->close_on_exec = 0;
  // XXX: ????? Update the pages
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	current->used_math = 0;
	p += change_ldt(ex.a_text,page)-MAX_ARG_PAGES*PAGE_SIZE;
	p = (unsigned long) create_tables((char *)p,argc,envc);
  // XXX: Define the upper limit on process address space. 
	current->brk = ex.a_bss +
		(current->end_data = ex.a_data +  // data 
		(current->end_code = ex.a_text)); // text/executable 
                                      // Heap will grow bottom to top
                                      // means brk to above
	current->start_stack = p & 0xfffff000; // stack will grow top to bottom
  // XXX: read the executable into proceess address space
	i = read_area(inode,ex.a_text+ex.a_data);
	iput(inode); // Free inode structure, not pages
	if (i<0)
		sys_exit(-1); // free the resource, 
                  // ( pages + .... )
                  // NULL the task in global task table
                  // give SIGCHLD, to its parent.
                  // call sheduler() function.
	i = ex.a_text+ex.a_data;
	while (i&0xfff) // XXX: ?????
		put_fs_byte(0,(char *) (i++));
  // XXX: ?????
	eip[0] = ex.a_entry;		/* eip, magic happens :-) */
	eip[3] = p;			/* stack pointer */
	return 0;
}

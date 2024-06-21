# Chapter8：文件系统

## 8.1 概述

| 文件描述符（File descriptor）  |
| ------------------------------ |
| 路径名（Pathname）             |
| 目录（Directory）              |
| 索引结点（Inode）              |
| 日志（Logging）                |
| 缓冲区高速缓存（Buffer cache） |
| 磁盘（Disk）                   |

- 磁盘层读取和写入virtio硬盘上的块
- 缓冲区高速缓存层缓存磁盘块并同步对它们的访问，确保每次只有一个内核进程可以修改存储在任何特定块中的数据
- 日志记录层允许更高层在一次事务（transaction）中将更新包装到多个块，并确保在遇到崩溃时自动更新这些块（即，所有块都已更新或无更新）
- 索引结点层提供单独的文件，每个文件表示为一个索引结点，其中包含唯一的索引号（i-number）和一些保存文件数据的块
- 目录层将每个目录实现为一种特殊的索引结点，其内容是一系列目录项，每个目录项包含一个文件名和索引号
- 路径名层提供了分层路径名，如***/usr/rtm/xv6/fs.c***，并通过递归查找来解析它们
- 文件描述符层使用文件系统接口抽象了许多Unix资源（例如，管道、设备、文件等），简化了应用程序员的工作

xv6将磁盘划分为几个部分，如图8.2所示。文件系统不使用块0（它保存引导扇区）。块1称为超级块：它包含有关文件系统的元数据（文件系统大小（以块为单位）、数据块数、索引节点数和日志中的块数）。从2开始的块保存日志。日志之后是索引节点，每个块有多个索引节点。然后是位图块，跟踪正在使用的数据块。其余的块是数据块：每个都要么在位图块中标记为空闲，要么保存文件或目录的内容。超级块由一个名为`mkfs`的单独的程序填充，该程序构建初始文件系统。

![img](assets/p1.png)



## 8.2 Buffer cache层

Buffer cache有两个任务：

1. 同步对磁盘块的访问，以确保磁盘块在内存中只有一个副本，并且一次只有一个内核线程使用该副本
2. 缓存常用块，以便不需要从慢速磁盘重新读取它们。代码在***bio.c***中。

Buffer cache层导出的主接口主要是`bread`和`bwrite`；

- 前者获取一个*buf*，其中包含一个可以在内存中读取或修改的块的副本
- 后者将修改后的缓冲区写入磁盘上的相应块。

内核线程必须通过调用`brelse`释放缓冲区。Buffer cache每个缓冲区使用一个睡眠锁，以确保每个缓冲区（因此也是每个磁盘块）每次只被一个线程使用；`bread`返回一个上锁的缓冲区，`brelse`释放该锁。

Buffer cache中保存磁盘块的缓冲区数量固定，这意味着如果文件系统请求还未存放在缓存中的块，Buffer cache必须回收当前保存其他块内容的缓冲区。Buffer cache为新块回收**最近使用最少的缓冲区**（LRU）



## 8.3 Buffer cache

`main`调用的函数`binit`使用静态数组`buf`（***kernel/bio.c***:26）中的`NBUF`个缓冲区初始化列表。对Buffer cache的所有其他访问都通过`bcache.head`引用**链表**，而不是`buf`数组

- ```c
  struct {
    struct spinlock lock;
    struct buf buf[NBUF];
  
    // Linked list of all buffers, through prev/next.
    // Sorted by how recently the buffer was used.
    // head.next is most recent, head.prev is least.
    struct buf head;
  } bcache;
  ```

- 缓冲区有两个与之关联的状态字段。字段`valid`表示缓冲区是否包含块的副本。字段`disk`表示缓冲区内容是否已交给磁盘

`Bread`调用`bget`为给定扇区获取缓冲区。如果缓冲区需要从磁盘进行读取，`bread`会在返回缓冲区之前调用`virtio_disk_rw`来执行此操作。

- ```c
  // Return a locked buf with the contents of the indicated block.
  struct buf*
  bread(uint dev, uint blockno)
  {
    struct buf *b;
  
    b = bget(dev, blockno);
    if(!b->valid) {
      virtio_disk_rw(b, 0);
      b->valid = 1;
    }
    return b;
  }
  ```

`Bget`扫描缓冲区列表，查找具有给定设备和扇区号的缓冲区。如果存在这样的缓冲区，`bget`将获取缓冲区的睡眠锁。然后`Bget`返回锁定的缓冲区。

- ```c
  static struct buf*
  bget(uint dev, uint blockno)
  {
    struct buf *b;
  
    acquire(&bcache.lock);
  
    // Is the block already cached?
    for(b = bcache.head.next; b != &bcache.head; b = b->next){
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        release(&bcache.lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
  
    // Not cached.
    // Recycle the least recently used (LRU) unused buffer.
    for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
      if(b->refcnt == 0) {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&bcache.lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    panic("bget: no buffers");
  }
  ```

  - 对于给定的扇区没有缓冲区，`bget`必须创建一个，这可能会重用包含其他扇区的缓冲区。它再次扫描缓冲区列表，查找未在使用中的缓冲区（`b->refcnt = 0`）：任何这样的缓冲区都可以使用。
  - `Bget`编辑缓冲区元数据以记录新设备和扇区号，并获取其睡眠锁。
  - 注意，`b->valid = 0`的布置确保了`bread`将从磁盘读取块数据，而不是错误地使用缓冲区以前的内容

- 每个磁盘扇区最多有一个缓存缓冲区是非常重要的，并且因为文件系统使用缓冲区上的锁进行同步，可以确保读者看到写操作

  - `Bget`的从第一个检查块是否缓存的循环到第二个声明块现在已缓存（通过设置`dev`、`blockno`和`refcnt`）的循环，一直持有`bcache.lock`来确保此不变量。这会导致检查块是否存在以及（如果不存在）指定一个缓冲区来存储块具有原子性

- `bget`在`bcache.lock`临界区域之外获取缓冲区的睡眠锁是安全的，因为非零`b->refcnt`防止缓冲区被重新用于不同的磁盘块。睡眠锁保护块缓冲内容的读写，而`bcache.lock`保护有关缓存哪些块的信息。

一旦`bread`读取了磁盘（如果需要）并将缓冲区返回给其调用者，调用者就可以独占使用缓冲区，并可以读取或写入数据字节。如果调用者确实修改了缓冲区，则必须在释放缓冲区之前调用`bwrite`将更改的数据写入磁盘。`Bwrite`（***kernel/bio.c***:107）调用`virtio_disk_rw`与磁盘硬件对话。

- ```c
  // Write b's contents to disk.  Must be locked.
  void
  bwrite(struct buf *b)
  {
    if(!holdingsleep(&b->lock))
      panic("bwrite");
    virtio_disk_rw(b, 1);
  }
  ```

当调用方使用完缓冲区后，它必须调用`brelse`来释放缓冲区，释放睡眠锁并将缓冲区移动到链表的前面。移动缓冲区会使列表按缓冲区的使用频率排序（意思是释放）：列表中的第一个缓冲区是最近使用的，最后一个是最近使用最少的：`bget`中的两个循环利用了这一点

- ```c
  void
  brelse(struct buf *b)
  {
    if(!holdingsleep(&b->lock))
      panic("brelse");
  
    releasesleep(&b->lock);
  
    acquire(&bcache.lock);
    b->refcnt--;
    if (b->refcnt == 0) {
      // no one is waiting for it.
      b->next->prev = b->prev;
      b->prev->next = b->next;
      b->next = bcache.head.next;
      b->prev = &bcache.head;
      bcache.head.next->prev = b;
      bcache.head.next = b;
    }
    
    release(&bcache.lock);
  }
  ```

## 8.4 日志层

文件系统设计中最有趣的问题之一是崩溃恢复。出现此问题的原因是，许多文件系统操作都涉及到对磁盘的多次写入，并且在完成写操作的部分子集后崩溃可能会使磁盘上的文件系统处于不一致的状态

**Xv6通过简单的日志记录形式解决了文件系统操作期间的崩溃问题。**

- xv6系统调用不会直接写入磁盘上的文件系统数据结构。
- 相反，它会在磁盘上的*log*（日志）中放置它希望进行的所有磁盘写入的描述。
- 一旦系统调用记录了它的所有写入操作，它就会向磁盘写入一条特殊的*commit*（提交）记录，表明日志包含一个完整的操作。
- 此时，系统调用将写操作复制到磁盘上的文件系统数据结构。完成这些写入后，系统调用将擦除磁盘上的日志。

如果系统崩溃并重新启动，则在运行任何进程之前，文件系统代码将按如下方式从崩溃中恢复。

- 如果日志标记为包含完整操作，则恢复代码会将写操作复制到磁盘文件系统中它们所属的位置。
- 如果日志没有标记为包含完整操作，则恢复代码将忽略该日志。恢复代码通过擦除日志完成。



## 8.5 日志设计

日志驻留在超级块中指定的已知固定位置。它由一个头块（header block）和一系列更新块的副本（logged block）组成。头块包含一个扇区号数组（每个logged block对应一个扇区号）以及日志块的计数。

- 磁盘上的头块中的计数或者为零，表示日志中没有事务；
- 或者为非零，表示日志包含一个完整的已提交事务，并具有指定数量的logged block。
- 在将logged blocks复制到文件系统后将计数设置为零
  - 事务中途崩溃将导致日志头块中的计数为零；提交后的崩溃将导致非零计数

为了允许不同进程并发执行文件系统操作，日志系统可以将多个系统调用的写入累积到一个事务中。为了避免在事务之间拆分系统调用，日志系统仅在没有文件系统调用进行时提交。

同时提交多个事务的想法称为**组提交**（group commit）

- 组提交减少了磁盘操作的数量
- 同时为磁盘系统提供更多并发写操作，可能允许磁盘在一个磁盘旋转时间内写入所有这些操作
- Xv6的virtio驱动程序不支持这种批处理，但是Xv6的文件系统设计允许这样做

Xv6在磁盘上留出固定的空间来保存日志。事务中系统调用写入的块总数必须可容纳于该空间。这导致两个后果：

- 任何单个系统调用都不允许写入超过日志空间的不同块
  - 这对于大多数系统调用来说都不是问题，但其中两个可能会写入许多块：`write`和`unlink`。
    - 一个大文件的`write`可以写入多个数据块和多个位图块以及一个inode块；
    - `unlink`大文件可能会写入许多位图块和inode
  - Xv6的`write`系统调用将大的写入分解为适合日志的多个较小的写入
  - `unlink`不会导致此问题，因为实际上Xv6文件系统只使用一个位图块
- 除非确定系统调用的写入将可容纳于日志中剩余的空间，否则日志系统无法允许启动系统调用



## 8.6 代码：日志

在系统调用中一个典型的日志使用就像这样：

```c
 begin_op();
 ...
 bp = bread(...);
 bp->data[...] = ...;
 log_write(bp);
 ...
 end_op();
```

`begin_op`（***kernel/log.c***:126）等待直到日志系统当前未处于提交中，并且直到有足够的未被占用的日志空间来保存此调用的写入。

- ```c
  // called at the start of each FS system call.
  void
  begin_op(void)
  {
    acquire(&log.lock);
    while(1){
      if(log.committing){
        sleep(&log, &log.lock);
      } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
        // this op might exhaust log space; wait for commit.
        sleep(&log, &log.lock);
      } else {
        log.outstanding += 1;
        release(&log.lock);
        break;
      }
    }
  }
  ```

- `log.outstanding`统计预定了日志空间的系统调用数；为此保留的总空间为`log.outstanding`乘以`MAXOPBLOCKS`。
- 递增`log.outstanding`会预定空间并防止在此系统调用期间发生提交。代码保守地假设**每个系统调用**最多可以写入`MAXOPBLOCKS`个不同的块。

`log_write`（***kernel/log.c***:214）充当`bwrite`的代理。它将块的扇区号记录在内存中，在磁盘上的日志中预定一个槽位，并调用`bpin`将缓存固定在block cache中，以防止block cache将其逐出。

- 固定在block cache是指在缓存不足需要考虑替换时，不会将这个block换出，因为事务具有原子性：假设块45被写入，将其换出的话需要写入磁盘中文件系统对应的位置，而日志系统要求所有内存必须都存入日志，最后才能写入文件系统。

- ```c
  void
  log_write(struct buf *b)
  {
    int i;
  
    acquire(&log.lock);
    if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
      panic("too big a transaction");
    if (log.outstanding < 1)
      panic("log_write outside of trans");
  
    for (i = 0; i < log.lh.n; i++) {
      if (log.lh.block[i] == b->blockno)   // log absorption
        break;
    }
    log.lh.block[i] = b->blockno;
    if (i == log.lh.n) {  // Add new block to log?
      bpin(b);
      log.lh.n++;
    }
    release(&log.lock);
  }
  ```

- `bpin`是通过增加引用计数防止块被换出的，之后需要再调用`bunpin`
- `log_write`会注意到在单个事务中多次写入一个块的情况，并在日志中为该块分配相同的槽位。这种优化通常称为合并（absorption）

`end_op`（***kernel/log.c***:146）首先减少未完成系统调用的计数。如果计数现在为零，则通过调用`commit()`提交当前事务。这一过程分为四个阶段。

1. `write_log()`（***kernel/log.c***:178）将事务中修改的每个块从缓冲区缓存复制到磁盘上日志槽位中。
2. `write_head()`（***kernel/log.c***:102）将头块写入磁盘：这是提交点，写入后的崩溃将导致从日志恢复重演事务的写入操作
3. `install_trans`（***kernel/log.c***:69）从日志中读取每个块，并将其写入文件系统中的适当位置。
4. 最后，`end_op`写入计数为零的日志头；这必须在下一个事务开始写入日志块之前发生，以便崩溃不会导致使用一个事务的头块和后续事务的日志块进行恢复。



## 8.7 代码：块分配器

文件和目录内容存储在磁盘块中，磁盘块必须从空闲池中分配。xv6的块分配器在磁盘上维护一个空闲位图，每一位代表一个块。

- 0表示对应的块是空闲的；
- 1表示它正在使用中
- 程序`mkfs`设置对应于引导扇区、超级块、日志块、inode块和位图块的比特位

块分配器提供两个功能：`balloc`分配一个新的磁盘块，`bfree`释放一个块。

- `Balloc`中的循环从块0到`sb.size`（文件系统中的块数）遍历每个块。它查找位图中位为零的空闲块。如果`balloc`找到这样一个块，它将更新位图并返回该块。
  - 为了提高效率，循环被分成两部分。外部循环读取位图中的每个块。内部循环检查单个位图块中的所有BPB位。
  - 由于任何一个位图块在buffer cache中一次只允许一个进程使用，因此，如果两个进程同时尝试分配一个块，可能会发生争用。
- `Bfree`（***kernel/fs.c***:90）找到正确的位图块并清除正确的位。同样，`bread`和`brelse`隐含的独占使用避免了显式锁定的需要。



## 8.8 索引结点层

术语inode（即索引结点）可以具有两种相关含义

1. 指包含文件大小和数据块编号列表的磁盘上的数据结构
2. 指内存中的inode，它包含磁盘上inode的副本以及内核中所需的额外信息

**磁盘上的inode**

盘上的inode都被打包到一个称为inode块的连续磁盘区域中

- 每个inode的大小都相同，因此在给定数字n的情况下，很容易在磁盘上找到第n个inode
- 这个编号n，称为inode number或i-number，是在具体实现中标识inode的方式

磁盘上的inode由`struct dinode`（***kernel/fs.h***:32）定义

- ```c
  // On-disk inode structure
  struct dinode {
    short type;           // File type
    short major;          // Major device number (T_DEVICE only)
    short minor;          // Minor device number (T_DEVICE only)
    short nlink;          // Number of links to inode in file system
    uint size;            // Size of file (bytes)
    uint addrs[NDIRECT+1];   // Data block addresses
  };
  ```

  - 字段`type`区分文件、目录和特殊文件（设备）。`type`为零表示磁盘inode是空闲的。
  - 字段`nlink`统计引用此inode的目录条目数，以便识别何时应释放磁盘上的inode及其数据块。
  - 字段`size`记录文件中内容的字节数。
  - `addrs`数组记录保存文件内容的磁盘块的块号。

**内存中的inode**

内核将活动的inode集合保存在内存中；`struct inode`（***kernel/file.h***:17）是磁盘上`struct dinode`的内存副本。

- ```c
  // in-memory copy of an inode
  struct inode {
    uint dev;           // Device number
    uint inum;          // Inode number
    int ref;            // Reference count
    struct sleeplock lock; // protects everything below here
    int valid;          // inode has been read from disk?
  
    short type;         // copy of disk inode
    short major;
    short minor;
    short nlink;
    uint size;
    uint addrs[NDIRECT+1];
  };
  ```

  - 只有当有C指针引用某个inode时，内核才会在内存中存储该inode。
  - `ref`字段统计引用内存中inode的C指针的数量，如果引用计数降至零，内核将从内存中丢弃该inode。

xv6的inode代码中有四种**锁或类似锁**的机制。

- `icache.lock`保护以下两个不变量：
  - inode最多在缓存中出现一次；
  - 缓存inode的`ref`字段记录指向缓存inode的内存指针数量。
- 每个内存中的inode都有一个包含睡眠锁的`lock`字段
  - 它确保以独占方式访问inode的字段（如文件长度）以及inode的文件或目录内容块。
- 如果inode的`ref`大于零，则会导致系统在cache中维护inode，而不会对其他inode重用此缓存项
- 每个inode都包含一个`nlink`字段（在磁盘上，如果已缓存则复制到内存中）
  - 该字段统计引用文件的目录项的数量；如果inode的链接计数大于零，xv6将不会释放inode

`iget`和`iput`函数分别获取和释放指向inode的指针，修改引用计数。指向inode的指针可以来自文件描述符、当前工作目录和如`exec`的瞬态内核代码。

- `iget()`返回的`struct inode`指针在相应的`iput()`调用之前保证有效
  - inode不会被删除，指针引用的内存也不会被其他inode重用。
- `iget()`提供对inode的非独占访问，因此可以有许多指向同一inode的指针。



## 8.9 代码：inode

为了分配新的inode（例如，在创建文件时），xv6调用`ialloc`（***kernel/fs.c***:198）。

- ```c
  struct inode*
  ialloc(uint dev, short type)
  {
    int inum;
    struct buf *bp;
    struct dinode *dip;
  
    for(inum = 1; inum < sb.ninodes; inum++){
      bp = bread(dev, IBLOCK(inum, sb));
      dip = (struct dinode*)bp->data + inum%IPB;
      if(dip->type == 0){  // a free inode
        memset(dip, 0, sizeof(*dip));
        dip->type = type;
        log_write(bp);   // mark it allocated on the disk
        brelse(bp);
        return iget(dev, inum);
      }
      brelse(bp);
    }
    printf("ialloc: no inodes\n");
    return 0;
  }
  ```

- `ialloc`类似于`balloc`：它一次一个块地遍历磁盘上的索引节点结构体，查找标记为空闲的一个。
- 当它找到一个时，它通过将新`type`写入磁盘来声明它，然后末尾通过调用`iget`（***kernel/fs.c***:210）从inode缓存返回一个条目。
- `ialloc`的正确操作取决于这样一个事实：一次只有一个进程可以保存对`bp`的引用：`ialloc`可以确保其他进程不会同时看到inode可用并尝试声明它。

`Iget`（***kernel/fs.c***:243）在inode缓存中查找具有所需设备和inode编号的活动条目（`ip->ref > 0`）。

- ```c
  static struct inode*
  iget(uint dev, uint inum)
  {
    struct inode *ip, *empty;
  
    acquire(&itable.lock);
  
    // Is the inode already in the table?
    empty = 0;
    for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
      if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
        ip->ref++;
        release(&itable.lock);
        return ip;
      }
      if(empty == 0 && ip->ref == 0)    // Remember empty slot.
        empty = ip;
    }
  
    // Recycle an inode entry.
    if(empty == 0)
      panic("iget: no inodes");
  
    ip = empty;
    ip->dev = dev;
    ip->inum = inum;
    ip->ref = 1;
    ip->valid = 0;
    release(&itable.lock);
  
    return ip;
  }
  ```

- 如果找到一个，它将返回对该incode的新引用。
- 在`iget`扫描时，它会记录第一个空槽的位置，如果需要分配缓存项，它会使用这个槽。

在读取或写入inode的元数据或内容之前，代码必须使用`ilock`锁定inode。`ilock`（kernel/fs.c:292）为此使用睡眠锁。一旦`ilock`以独占方式访问inode，它将根据需要从磁盘（更可能是buffer cache）读取inode。函数`iunlock`（***kernel/fs.c***:320）释放睡眠锁，这可能会导致任何睡眠进程被唤醒。

`iput`（***kernel/fs.c***:336）通过减少引用计数释放指向inode的C指针。如果这是最后一次引用，inode缓存中该inode的槽现在将是空闲的，可以重用于其他inode。

- ```c
  void
  iput(struct inode *ip)
  {
    acquire(&itable.lock);
  
    if(ip->ref == 1 && ip->valid && ip->nlink == 0){
      // inode has no links and no other references: truncate and free.
  
      // ip->ref == 1 means no other process can have ip locked,
      // so this acquiresleep() won't block (or deadlock).
      acquiresleep(&ip->lock);
  
      release(&itable.lock);
  
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
  
      releasesleep(&ip->lock);
  
      acquire(&itable.lock);
    }
  
    ip->ref--;
    release(&itable.lock);
  }
  ```

- 如果`iput`发现没有指向inode的C指针引用，并且inode没有指向它的链接（发生于无目录），则必须释放inode及其数据块。`Iput`调用`itrunc`将文件截断为零字节，释放数据块；将索引节点类型设置为0（未分配）；并将inode写入磁盘

`iput()`和崩溃之间存在一种具有挑战性的交互。

- `iput()`不会在文件的链接计数降至零时立即截断文件，因为某些进程可能仍在内存中保留对inode的引用：进程可能仍在读取和写入该文件，因为它已成功打开该文件

- 如果在最后一个进程关闭该文件的文件描述符之前发生崩溃，则该文件将被标记为已在磁盘上分配，但没有目录项指向它

文件系统以两种方式之一处理这种情况。

- 简单的解决方案用于恢复时：重新启动后，文件系统会扫描整个文件系统，以查找标记为已分配但没有指向它们的目录项的文件。如果存在任何此类文件，接下来可以将其释放。
- 第二种解决方案不需要扫描文件系统。在此解决方案中，文件系统在磁盘（例如在超级块中）上记录链接计数降至零但引用计数不为零的文件的i-number。如果文件系统在其引用计数达到0时删除该文件，则会通过从列表中删除该inode来更新磁盘列表。恢复时，文件系统将释放列表中的任何文件。

Xv6没有实现这两种解决方案，这意味着inode可能被标记为已在磁盘上分配，即使它们不再使用。这意味着随着时间的推移，xv6可能会面临磁盘空间不足的风险。



## 8.10 代码：inode包含内容

![img](assets/p2.png)

- 前面的`NDIRECT`个数据块被列在数组中的前`NDIRECT`个元素中；这些块称为直接块（direct blocks）
- 接下来的`NINDIRECT`个数据块不在inode中列出，而是在称为间接块（indirect block）的数据块中列出
- `addrs`数组中的最后一个元素给出了间接块的地址。
  - 因此，可以从inode中列出的块加载文件的前12 kB（`NDIRECT x BSIZE`）字节，而只有在查阅间接块后才能加载下一个256 kB（`NINDIRECT x BSIZE`）字节。
- 函数`bmap`管理这种表示，以便实现我们将很快看到的如`readi`和`writei`这样的更高级例程。`bmap(struct inode *ip, uint bn)`返回索引结点`ip`的第`bn`个数据块的磁盘块号。如果`ip`还没有这样的块，`bmap`会分配一个

函数`bmap`（***kernel/fs.c***:382）从简单的情况开始：前面的`NDIRECT`个块在inode本身中列出中。下面`NINDIRECT`个块在`ip->addrs[NDIRECT]`的间接块中列出。`Bmap`读取间接块，然后从块内的正确位置读取块号。如果块号超过`NDIRECT+NINDIRECT`，则`bmap`调用`panic`崩溃；`writei`包含防止这种情况发生的检查。

- ```c
  static uint
  bmap(struct inode *ip, uint bn)
  {
    uint addr, *a;
    struct buf *bp;
  
    if(bn < NDIRECT){
      if((addr = ip->addrs[bn]) == 0){
        addr = balloc(ip->dev);
        if(addr == 0)
          return 0;
        ip->addrs[bn] = addr;
      }
      return addr;
    }
    bn -= NDIRECT;
  
    if(bn < NINDIRECT){
      // Load indirect block, allocating if necessary.
      if((addr = ip->addrs[NDIRECT]) == 0){
        addr = balloc(ip->dev);
        if(addr == 0)
          return 0;
        ip->addrs[NDIRECT] = addr;
      }
      bp = bread(ip->dev, addr);
      a = (uint*)bp->data;
      if((addr = a[bn]) == 0){
        addr = balloc(ip->dev);
        if(addr){
          a[bn] = addr;
          log_write(bp);
        }
      }
      brelse(bp);
      return addr;
    }
  
    panic("bmap: out of range");
  }
  ```

  - `ip->addrs[]`或间接块中条目为零表示未分配块。当`bmap`遇到零时，它会用按需分配的新块

`itrunc`释放文件的块，将inode的`size`重置为零。首先释放直接块，然后释放间接块中列出的块，最后释放间接块本身。

- ```c
  void
  itrunc(struct inode *ip)
  {
    int i, j;
    struct buf *bp;
    uint *a;
  
    for(i = 0; i < NDIRECT; i++){
      if(ip->addrs[i]){
        bfree(ip->dev, ip->addrs[i]);
        ip->addrs[i] = 0;
      }
    }
  
    if(ip->addrs[NDIRECT]){
      bp = bread(ip->dev, ip->addrs[NDIRECT]);
      a = (uint*)bp->data;
      for(j = 0; j < NINDIRECT; j++){
        if(a[j])
          bfree(ip->dev, a[j]);
      }
      brelse(bp);
      bfree(ip->dev, ip->addrs[NDIRECT]);
      ip->addrs[NDIRECT] = 0;
    }
  
    ip->size = 0;
    iupdate(ip);
  }
  ```

`Readi`（***kernel/fs.c***:471）首先确保偏移量和计数不超过文件的末尾。开始于超过文件末尾的地方读取将返回错误，而从文件末尾开始或穿过文件末尾的读取返回的字节数少于请求的字节数。主循环处理文件的每个块，将数据从缓冲区复制到`dst`。

- ```c
  int
  readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
  {
    uint tot, m;
    struct buf *bp;
  
    if(off > ip->size || off + n < off)
      return 0;
    if(off + n > ip->size)
      n = ip->size - off;
  
    for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
      uint addr = bmap(ip, off/BSIZE);
      if(addr == 0)
        break;
      bp = bread(ip->dev, addr);
      m = min(n - tot, BSIZE - off%BSIZE);
      if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
        brelse(bp);
        tot = -1;
        break;
      }
      brelse(bp);
    }
    return tot;
  }
  ```

`writei`（***kernel/fs.c***:505）与`readi`相同，但有三个例外：

- ```c
  int
  writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
  {
    uint tot, m;
    struct buf *bp;
  
    if(off > ip->size || off + n < off)
      return -1;
    if(off + n > MAXFILE*BSIZE)
      return -1;
  
    for(tot=0; tot<n; tot+=m, off+=m, src+=m){
      uint addr = bmap(ip, off/BSIZE);
      if(addr == 0)
        break;
      bp = bread(ip->dev, addr);
      m = min(n - tot, BSIZE - off%BSIZE);
      if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
        brelse(bp);
        break;
      }
      log_write(bp);
      brelse(bp);
    }
  
    if(off > ip->size)
      ip->size = off;
  
    // write the i-node back to disk even if the size didn't change
    // because the loop above might have called bmap() and added a new
    // block to ip->addrs[].
    iupdate(ip);
  
    return tot;
  }
  ```

- 从文件末尾开始或穿过文件末尾的写操作会使文件增长到最大文件大小；
- 循环将数据复制到缓冲区而不是输出；
- 如果写入扩展了文件，`writei`必须更新其大小

`readi`和`writei`都是从检查`ip->type == T_DEV`开始的。这种情况处理的是数据不在文件系统中的特殊设备；我们将在文件描述符层返回到这种情况。

函数`stati`将inode元数据复制到`stat`结构体中，该结构通过`stat`系统调用向用户程序公开。



## 8.11 代码：目录层

目录的内部实现很像文件。其inode的`type`为`T_DIR`，其数据是一系列目录条目（directory entries）。每个条目（entry）都是一个`struct dirent`（***kernel/fs.h***:56），其中包含一个名称`name`和一个inode编号`inum`。名称最多为`DIRSIZ`（14）个字符；如果较短，则以`NUL`（0）字节终止。inode编号为零的条目是空的。

- ```c
  struct dirent {
    ushort inum;
    char name[DIRSIZ];
  };
  ```

函数`dirlookup`（***kernel/fs.c***:551）在目录中搜索具有给定名称的条目。

- ```c
  struct inode*
  dirlookup(struct inode *dp, char *name, uint *poff)
  {
    uint off, inum;
    struct dirent de;
  
    if(dp->type != T_DIR)
      panic("dirlookup not DIR");
  
    for(off = 0; off < dp->size; off += sizeof(de)){
      if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        panic("dirlookup read");
      if(de.inum == 0)
        continue;
      if(namecmp(name, de.name) == 0){
        // entry matches path element
        if(poff)
          *poff = off;
        inum = de.inum;
        return iget(dp->dev, inum);
      }
    }
  
    return 0;
  }
  ```

- 如果找到一个，它将返回一个指向相应inode的指针，解开锁定，并将`*poff`设置为目录中条目的字节偏移量，以满足调用方希望对其进行编辑的情形。
- 如果`dirlookup`找到具有正确名称的条目，它将更新`*poff`并返回通过`iget`获得的未锁定的inode。
- `dirlookup`是`iget`返回未锁定indoe的原因。
  - 调用者已锁定`dp`，因此，如果对`.`，当前目录的别名，进行查找，则在返回之前尝试锁定indoe将导致重新锁定`dp`并产生死锁(还有更复杂的死锁场景，涉及多个进程和`..`，父目录的别名。`.`不是唯一的问题。）调用者可以解锁`dp`，然后锁定`ip`，确保它一次只持有一个锁。

函数`dirlink`（***kernel/fs.c***:579）将给定名称和inode编号的新目录条目写入目录`dp`。如果名称已经存在，`dirlink`将返回一个错误。主循环读取目录条目，查找未分配的条目。当找到一个时，它会提前停止循环，并将`off`设置为可用条目的偏移量。否则，循环结束时会将`off`设置为`dp->size`。无论哪种方式，`dirlink`都会通过在偏移`off`处写入来向目录添加一个新条目。

- ```c
  nt
  dirlink(struct inode *dp, char *name, uint inum)
  {
    int off;
    struct dirent de;
    struct inode *ip;
  
    // Check that name is not present.
    if((ip = dirlookup(dp, name, 0)) != 0){
      iput(ip);
      return -1;
    }
  
    // Look for an empty dirent.
    for(off = 0; off < dp->size; off += sizeof(de)){
      if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        panic("dirlink read");
      if(de.inum == 0)
        break;
    }
  
    strncpy(de.name, name, DIRSIZ);
    de.inum = inum;
    if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      return -1;
  
    return 0;
  }
  ```



## 8.12 代码：路径名

路径名查找涉及一系列对`dirlookup`的调用，每个路径组件调用一个。`Namei`（***kernel/fs.c***:686）计算`path`并返回相应的inode。函数`nameiparent`是一个变体：它在最后一个元素之前停止，返回父目录的inode并将最后一个元素复制到`name`中。两者都调用通用函数`namex`来完成实际工作。

函数 `namex`

- ```c
  static struct inode*
  namex(char *path, int nameiparent, char *name)
  {
    struct inode *ip, *next;
  
    if(*path == '/')
      ip = iget(ROOTDEV, ROOTINO);
    else
      ip = idup(myproc()->cwd);
  
    while((path = skipelem(path, name)) != 0){
      ilock(ip);
      if(ip->type != T_DIR){
        iunlockput(ip);
        return 0;
      }
      if(nameiparent && *path == '\0'){
        // Stop one level early.
        iunlock(ip);
        return ip;
      }
      if((next = dirlookup(ip, name, 0)) == 0){
        iunlockput(ip);
        return 0;
      }
      iunlockput(ip);
      ip = next;
    }
    if(nameiparent){
      iput(ip);
      return 0;
    }
    return ip;
  }
  ```

  - 首先决定路径计算的开始位置。
    - 如果路径以斜线开始，则计算从根目录开始
    - 否则，从当前目录开始
  - 然后，它使用`skipelem`依次考察路径的每个元素
    - 循环的每次迭代都必须在当前索引结点`ip`中查找`name`
    - 迭代首先给`ip`上锁并检查它是否是一个目录。如果不是，则查找失败
      - 锁定`ip`是必要的，不是因为`ip->type`可以被更改，而是因为在`ilock`运行之前，`ip->type`不能保证已从磁盘加载
    - 如果调用是`nameiparent`，并且这是最后一个路径元素，则根据`nameiparent`的定义，循环会提前停止
    - 最后一个路径元素已经复制到`name`中，因此`namex`只需返回解锁的`ip`
    - 最后，循环将使用`dirlookup`查找路径元素，并通过设置`ip = next`为下一次迭代做准备。当循环用完路径元素时，它返回`ip`

`namex`过程可能需要很长时间才能完成：它可能涉及多个磁盘操作来读取路径名中所遍历目录的索引节点和目录块（如果它们不在buffer cache中）。

- Xv6经过精心设计，如果一个内核线程对`namex`的调用在磁盘I/O上阻塞，另一个查找不同路径名的内核线程可以同时进行。`Namex`分别锁定路径中的每个目录，以便在不同目录中进行并行查找。

这种并发性带来了一些挑战。例如，当一个内核线程正在查找路径名时，另一个内核线程可能正在通过取消目录链接来更改目录树。

- 一个潜在的风险是，查找可能正在搜索已被另一个内核线程删除且其块已被重新用于另一个目录或文件的目录。
  - Xv6避免了这种竞争。
    - 例如，在`namex`中执行`dirlookup`时，lookup线程持有目录上的锁，`dirlookup`返回使用`iget`获得的inode。`Iget`增加索引节点的引用计数。只有在从`dirlookup`接收inode之后，`namex`才会释放目录上的锁。现在，另一个线程可以从目录中取消inode的链接，但是xv6还不会删除inode，因为inode的引用计数仍然大于零。
- 另一个风险是死锁。例如，查找“`.`”时，`next`指向与`ip`相同的inode。在释放`ip`上的锁之前锁定`next`将导致死锁。为了避免这种死锁，`namex`在获得下一个目录的锁之前解锁该目录。这里我们再次看到为什么`iget`和`ilock`之间的分离很重要



## 8.13 文件描述符层

系统中所有打开的文件都保存在全局文件表`ftable`中。文件表具有分配文件（`filealloc`）、创建重复引用（`filedup`）、释放引用（`fileclose`）以及读取和写入数据（`fileread`和`filewrite`）的函数。



## 8.14 代码：系统调用

函数`sys_link`和`sys_unlink`编辑目录，创建或删除索引节点的引用。它们是使用事务能力的另一个很好的例子。`sys_link`从获取其参数开始，两个字符串分别是`old`和`new`。假设`old`存在并且不是一个目录，`sys_link`会增加其`ip->nlink`计数。然后`sys_link`调用`nameiparent`来查找`new`的父目录和最终路径元素，并创建一个指向`old`的inode的新目录条目。`new`的父目录必须存在并且与现有inode位于同一设备上：inode编号在一个磁盘上只有唯一的含义。如果出现这样的错误，`sys_link`必须返回并减少`ip->nlink`。

- ```c
  uint64
  sys_link(void)
  {
    char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
    struct inode *dp, *ip;
  
    if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
      return -1;
  
    begin_op();
    if((ip = namei(old)) == 0){
      end_op();
      return -1;
    }
  
    ilock(ip);
    if(ip->type == T_DIR){
      iunlockput(ip);
      end_op();
      return -1;
    }
  
    ip->nlink++;
    iupdate(ip);
    iunlock(ip);
  
    if((dp = nameiparent(new, name)) == 0)
      goto bad;
    ilock(dp);
    if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
      iunlockput(dp);
      goto bad;
    }
    iunlockput(dp);
    iput(ip);
  
    end_op();
  
    return 0;
  
  bad:
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
    end_op();
    return -1;
  }
  ```

`Sys_link`为现有inode创建一个新名称。

函数`create`（***kernel/sysfile.c***:242）为新inode创建一个新名称。它是三个文件创建系统调用的泛化：带有`O_CREATE`标志的`open`生成一个新的普通文件，`mkdir`生成一个新目录，`mkdev`生成一个新的设备文件。

```c
static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}
```

- `Create`返回一个锁定的inode，但`namei`不锁定，因此`sys_open`必须锁定inode本身。

# 实验

## Task1

### 要求

在本作业中，您将增加xv6文件的最大大小。目前，xv6文件限制为268个块或`268*BSIZE`字节（在xv6中`BSIZE`为1024）。此限制来自以下事实：一个xv6 inode包含12个“直接”块号和一个“间接”块号，“一级间接”块指一个最多可容纳256个块号的块，总共12+256=268个块。

修改`bmap()`，以便除了直接块和一级间接块之外，它还实现二级间接块。你只需要有11个直接块，而不是12个，为你的新的二级间接块腾出空间；不允许更改磁盘inode的大小。`ip->addrs[]`的前11个元素应该是直接块；第12个应该是一个一级间接块（与当前的一样）；13号应该是你的新二级间接块。当`bigfile`写入65803个块并成功运行`usertests`时，此练习完成：

### 提示

- 确保您理解`bmap()`。写出`ip->addrs[]`、间接块、二级间接块和它所指向的一级间接块以及数据块之间的关系图。确保您理解为什么添加二级间接块会将最大文件大小增加256*256个块（实际上要-1，因为您必须将直接块的数量减少一个）。
- 考虑如何使用逻辑块号索引二级间接块及其指向的间接块。
- 如果更改`NDIRECT`的定义，则可能必须更改***file.h***文件中`struct inode`中`addrs[]`的声明。确保`struct inode`和`struct dinode`在其`addrs[]`数组中具有相同数量的元素。
- 如果更改`NDIRECT`的定义，请确保创建一个新的***fs.img\***，因为`mkfs`使用`NDIRECT`构建文件系统。
- 如果您的文件系统进入坏状态，可能是由于崩溃，请删除***fs.img***（从Unix而不是xv6执行此操作）。`make`将为您构建一个新的干净文件系统映像。
- 别忘了把你`bread()`的每一个块都`brelse()`。
- 您应该仅根据需要分配间接块和二级间接块，就像原始的`bmap()`。
- 确保`itrunc`释放文件的所有块，包括二级间接块。



### 实现

修改全局变量

- 减少一个直接块，增加一个二级间接块，其大小为256*256

- 修改了块的结构后，`MAXFILE` 可以接受的数量也会发生变化

- ```c
  // kernel/fs.h
  #define NDIRECT 11
  #define NINDIRECT (BSIZE / sizeof(uint))
  #define NINDIRECT2 NINDIRECT * NINDIRECT
  #define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT2)
  ```

- 因为修改了 `NDIRECT`，所以 `struct dinode`的 `addrs`数组要修改

  - ```c
    // On-disk inode structure
    struct dinode {
      short type;           // File type
      short major;          // Major device number (T_DEVICE only)
      short minor;          // Minor device number (T_DEVICE only)
      short nlink;          // Number of links to inode in file system
      uint size;            // Size of file (bytes)
      uint addrs[NDIRECT+2];   // Data block addresses
    };
    ```

- 因为修改了 `NDIRECT`，所以 ***file.h*** 中内存中的 `struct inode` 也要修改

  - ```c
    // in-memory copy of an inode
    struct inode {
      uint dev;           // Device number
      uint inum;          // Inode number
      int ref;            // Reference count
      struct sleeplock lock; // protects everything below here
      int valid;          // inode has been read from disk?
    
      short type;         // copy of disk inode
      short major;
      short minor;
      short nlink;
      uint size;
      uint addrs[NDIRECT+2];
    };
    ```

修改 `bmap` 的逻辑

- 在寻找块的时候，增加一层二层间接块的循环

- ```c
  static uint
  bmap(struct inode *ip, uint bn)
  {
    uint addr, *a;
    struct buf *bp;
    uint addr2;
  
    if(bn < NDIRECT){
      if((addr = ip->addrs[bn]) == 0)
        ip->addrs[bn] = addr = balloc(ip->dev);
      return addr;
    }
    bn -= NDIRECT;
  
    if(bn < NINDIRECT){
      // Load indirect block, allocating if necessary.
      if((addr = ip->addrs[NDIRECT]) == 0)
        ip->addrs[NDIRECT] = addr = balloc(ip->dev);
      bp = bread(ip->dev, addr);
      a = (uint*)bp->data;
      if((addr = a[bn]) == 0){
        a[bn] = addr = balloc(ip->dev);
        log_write(bp);
      }
      brelse(bp);
      return addr;
    }
    // 新加入的部分
    bn -= NINDIRECT;
    // 判断块号位于二级间接块
    if (bn < NINDIRECT2) {
      if ((addr = ip->addrs[NDIRECT + 1]) == 0)
        ip->addrs[NDIRECT + 1] = addr = balloc(ip->dev);
      // 先找到二级间接块中的一级块编号
      bp = bread(ip->dev, addr);
      a = (uint *)bp->data;
      addr2 = bn / NINDIRECT;
      if ((addr = a[addr2]) == 0) {
        a[addr2] = addr = balloc(ip->dev);
        // 一级块也要log_write
        log_write(bp);
      }
      // 注意brelse之前的bp，否则会panic
      brelse(bp);
      // 在对应的一级块中再寻找块号
      bp = bread(ip->dev, addr);
      a = (uint *)bp->data;
      addr2 = bn % NINDIRECT;
      if ((addr = a[addr2]) == 0) {
        a[addr2] = addr = balloc(ip->dev);
        log_write(bp);
      }
      // 别忘了 brelse
      brelse(bp);
      return addr;
    }
  
    panic("bmap: out of range");
  }
  ```

修改 `itrunc`

- 和 `bmap` 的修改逻辑类似，也是增加一个二级间接块的循环

- ```c
  void
  itrunc(struct inode *ip)
  {
    int i, j;
    struct buf *bp;
    uint *a, *a2;
    uint addr2;
  
    for(i = 0; i < NDIRECT; i++){
      if(ip->addrs[i]){
        bfree(ip->dev, ip->addrs[i]);
        ip->addrs[i] = 0;
      }
    }
  
    if(ip->addrs[NDIRECT]){
      bp = bread(ip->dev, ip->addrs[NDIRECT]);
      a = (uint*)bp->data;
      for(j = 0; j < NINDIRECT; j++){
        if(a[j])
          bfree(ip->dev, a[j]);
      }
      brelse(bp);
      bfree(ip->dev, ip->addrs[NDIRECT]);
      ip->addrs[NDIRECT] = 0;
    }
    
    // 新加部分，如果二级间接块存在
    if(ip->addrs[NDIRECT + 1]) {
      bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
      a = (uint *)bp->data;
      for (addr2 = 0; addr2 < NINDIRECT; ++addr2) {
        if (a[addr2]) {
          // 这里定义一个新的bp，之前定义的bp要在这个循环结束后释放，不能复用，否则usertests中的writebig会panic:brelse
          struct buf *bp2 = bread(ip->dev, a[addr2]);
          a2 = (uint *)bp2->data;
          for (j = 0; j < NINDIRECT; ++j) {
            if (a2[j]) {
              bfree(ip->dev, a2[j]);
            }
          }
          brelse(bp2);
          bfree(ip->dev, a[addr2]);
        }
      }
      brelse(bp);
      bfree(ip->dev, ip->addrs[NDIRECT + 1]);
      ip->addrs[NDIRECT + 1] = 0;
    }
  
    ip->size = 0;
    iupdate(ip);
  }
  ```



## Task2

### 要求

在本练习中，您将向xv6添加符号链接。符号链接（或软链接）是指按路径名链接的文件；当一个符号链接打开时，内核跟随该链接指向引用的文件。符号链接类似于硬链接，但硬链接仅限于指向同一磁盘上的文件，而符号链接可以跨磁盘设备。尽管xv6不支持多个设备，但实现此系统调用是了解路径名查找工作原理的一个很好的练习。

您将实现`symlink(char *target, char *path)`系统调用，该调用在引用由`target`命名的文件的路径处创建一个新的符号链接。有关更多信息，请参阅`symlink`手册页（注：执行`man symlink`）。要进行测试，请将`symlinktest`添加到***Makefile***并运行它。当测试产生以下输出（包括`usertests`运行成功）时，您就完成本作业了。



### 提示

- 首先，为`symlink`创建一个新的系统调用号，在***user/usys.pl***、***user/user.h***中添加一个条目，并在***kernel/sysfile.c***中实现一个空的`sys_symlink`。
- 向***kernel/stat.h***添加新的文件类型（`T_SYMLINK`）以表示符号链接。
- 在k***ernel/fcntl.h***中添加一个新标志（`O_NOFOLLOW`），该标志可用于`open`系统调用。请注意，传递给`open`的标志使用按位或运算符组合，因此新标志不应与任何现有标志重叠。一旦将***user/symlinktest.c***添加到***Makefile***中，您就可以编译它。
- 实现`symlink(target, path)`系统调用，以在`path`处创建一个新的指向`target`的符号链接。请注意，系统调用的成功不需要`target`已经存在。您需要选择存储符号链接目标路径的位置，例如在inode的数据块中。`symlink`应返回一个表示成功（0）或失败（-1）的整数，类似于`link`和`unlink`。
- 修改`open`系统调用以处理路径指向符号链接的情况。如果文件不存在，则打开必须失败。当进程向`open`传递`O_NOFOLLOW`标志时，`open`应打开符号链接（而不是跟随符号链接）。
- 如果链接文件也是符号链接，则必须递归地跟随它，直到到达非链接文件为止。如果链接形成循环，则必须返回错误代码。你可以通过以下方式估算存在循环：通过在链接深度达到某个阈值（例如10）时返回错误代码。
- 其他系统调用（如`link`和`unlink`）不得跟随符号链接；这些系统调用对符号链接本身进行操作。
- 您不必处理指向此实验的目录的符号链接。



### 实现

添加系统调用的过程不再赘述

向***kernel/stat.h***添加新的文件类型（`T_SYMLINK`）以表示符号链接

- ```c
  #define T_DIR     1   // Directory
  #define T_FILE    2   // File
  #define T_DEVICE  3   // Device
  // 新增
  #define T_SYMLINK 4   // Symbolic link
  ```

在k***ernel/fcntl.h***中添加一个新标志（`O_NOFOLLOW`）

- ```c
  #define O_RDONLY  0x000
  #define O_WRONLY  0x001
  #define O_RDWR    0x002
  #define O_CREATE  0x200
  #define O_TRUNC   0x400
  // 新增
  #define O_NOFOLLOW 0x800
  ```

实现 `symlink`

- 符号链接与普通的文件一样，需要占用 inode 块

- ```c
  // kernel/sysfile.c
  
  uint64
  sys_symlink(void)
  {
    struct inode *ip;
    char target[MAXPATH], path[MAXPATH];
    if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
      return -1;
  
    begin_op();
  
    ip = create(path, T_SYMLINK, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  
    // use the first data block to store target path.
    if(writei(ip, 0, (uint64)target, 0, strlen(target)) < 0) {
      end_op();
      return -1;
    }
  
    iunlockput(ip);
  
    end_op();
    return 0;
  }
  ```

修改 `sys_open`

- ```c
  uint64
  sys_open(void)
  {
    char path[MAXPATH];
    int fd, omode;
    struct file *f;
    struct inode *ip;
    int n;
  
    if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
      return -1;
  
    begin_op();
  
    if(omode & O_CREATE){
      ip = create(path, T_FILE, 0, 0);
      if(ip == 0){
        end_op();
        return -1;
      }
    } else {
      int symlink_depth = 0;
      while(1) { // recursively follow symlinks
        if((ip = namei(path)) == 0){
          end_op();
          return -1;
        }
        ilock(ip);
        if(ip->type == T_SYMLINK && (omode & O_NOFOLLOW) == 0) {
          if(++symlink_depth > 10) {
            // too many layer of symlinks, might be a loop
            iunlockput(ip);
            end_op();
            return -1;
          }
          if(readi(ip, 0, (uint64)path, 0, MAXPATH) < 0) {
            iunlockput(ip);
            end_op();
            return -1;
          }
          iunlockput(ip);
        } else {
          break;
        }
      }
      if(ip->type == T_DIR && omode != O_RDONLY){
        iunlockput(ip);
        end_op();
        return -1;
      }
    }
  
    // .......
  
    iunlock(ip);
    end_op();
  
    return fd;
  }
  ```



# 结果

![image-20240621113500567](assets/image-20240621113500567.png)
# Chapter3.5 代码：物理内存分配

分配器(allocator)位于***kalloc.c***(***kernel/kalloc.c***:1)中

- 分配器的数据结构是可供分配的物理内存页的空闲列表。每个空闲页的列表元素是一个`struct run`(***kernel/kalloc.c***:17)。
  - 将每个空闲页的`run`结构存储在空闲页本身
- 空闲列表受到自旋锁（spin lock）的保护(***kernel/kalloc.c***:21-24)。列表和锁被封装在一个结构体中，以明确锁在结构体中保护的字段

`main`函数调用`kinit`(***kernel/kalloc.c***:27)来初始化分配器

- `kinit`初始化空闲列表以保存从内核结束到`PHYSTOP`之间的每一页
- xv6应该通过解析硬件提供的配置信息来确定有多少物理内存可用。然而，xv6假设机器有128兆字节的RAM
- `kinit`调用`freerange`将内存添加到空闲列表中，在`freerange`中每页都会调用`kfree`。
  - PTE只能引用在4096字节边界上对齐的物理地址（是4096的倍数），所以`freerange`使用`PGROUNDUP`来确保它只释放对齐的物理地址。
- 分配器开始时没有内存；这些对`kfree`的调用给了它一些管理空间。

分配器有时将地址视为整数，以便对其执行算术运算（例如，在`freerange`中遍历所有页面），有时将地址用作读写内存的指针（例如，操纵存储在每个页面中的`run`结构）；这种地址的双重用途是分配器代码充满C类型转换的主要原因。另一个原因是释放和分配从本质上改变了内存的类型。

函数`kfree` (***kernel/kalloc.c***:47)首先将内存中的每一个字节设置为1。

- 这将导致使用释放后的内存的代码（使用“悬空引用”）读取到垃圾信息而不是旧的有效内容，从而希望这样的代码更快崩溃。
- 然后`kfree`将页面前置（头插法）到空闲列表中：
  - 它将`pa`转换为一个指向`struct run`的指针`r`，在`r->next`中记录空闲列表的旧开始，并将空闲列表设置为等于`r`。

`kalloc`删除并返回**空闲列表**中的第一个元素。



# Chapter6 锁

## 6.1 竞态条件

竞态条件是指多个进程读写某些共享数据（至少有一个访问是写入）的情况。竞争通常包含bug，要么丢失更新（如果访问是写入的），要么读取未完成更新的数据结构。

- 竞争的结果取决于进程在处理器运行的确切时机以及内存系统如何排序它们的内存操作，这可能会使竞争引起的错误难以复现和调试。

避免竞争的通常方法是使用锁

- `acquire`和`release`之间的指令序列通常被称为临界区域（critical section）

当我们说锁保护数据时，我们实际上是指锁保护适用于数据的某些不变量集合。不变量是跨操作维护的数据结构的属性。

- 通常，操作的正确行为取决于操作开始时不变量是否为真。操作可能暂时违反不变量，但必须在完成之前重新建立它们。
- 将锁视为串行化（serializing）并发的临界区域，以便同时只有一个进程在运行这部分代码，从而维护不变量（假设临界区域设定了正确的隔离性）
- 还可以将由同一锁保护的临界区域视为彼此之间的原子，即彼此之间只能看到之前临界区域的完整更改集，而永远看不到部分完成的更新

尽管正确使用锁可以改正不正确的代码，但锁限制了性能。例如，如果两个进程并发调用`kfree`，锁将串行化这两个调用，我们在不同的CPU上运行它们没有任何好处。

如果多个进程同时想要相同的锁或者锁经历了争用，则称之为发生冲突（conflict）。

- 内核设计中的一个主要挑战是避免锁争用。Xv6为此几乎没做任何工作，但是复杂的内核会精心设计数据结构和算法来避免锁的争用。
- 在链表示例中，**内核可能会为每个CPU维护一个空闲列表**，并且只有当CPU的列表为空并且必须从另一个CPU挪用内存时才会触及另一个CPU的空闲列表。其他用例可能需要更复杂的设计。



## 6.2 代码：Locks

### 自旋锁

Xv6将自旋锁表示为`struct spinlock` (***kernel/spinlock.h***:2)。结构体中的重要字段是`locked`，当锁可用时为零，当它被持有时为非零。

- ```c
  struct spinlock {
    uint locked;       // Is the lock held?
  
    // For debugging:
    char *name;        // Name of lock.
    struct cpu *cpu;   // The cpu holding the lock.
  };
  ```

从逻辑上讲，xv6应该通过执行以下代码来获取锁

```c
void
acquire(struct spinlock* lk) // does not work!
{
  for(;;) {
    if(lk->locked == 0) {
      lk->locked = 1;
      break;
    }
  }
}
```

- 这种实现不能保证多处理器上的互斥

在RISC-V上，这条指令是`amoswap r, a`。`amoswap`读取内存地址`a`处的值，将寄存器`r`的内容写入该地址，并将其读取的值放入`r`中。

- 它交换寄存器和指定内存地址的内容
- 它原子地执行这个指令序列

Xv6的`acquire`(***kernel/spinlock.c***:22)使用可移植的C库调用归结为`amoswap`的指令`__sync_lock_test_and_set`；返回值是`lk->locked`的旧（交换了的）内容。

- ```c
  // Acquire the lock.
  // Loops (spins) until the lock is acquired.
  void
  acquire(struct spinlock *lk)
  {
    push_off(); // disable interrupts to avoid deadlock.
    if(holding(lk))
      panic("acquire");
  
    // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
    //   a5 = 1
    //   s1 = &lk->locked
    //   amoswap.w.aq a5, a5, (s1)
    while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
      ;
  
    // Tell the C compiler and the processor to not move loads or stores
    // past this point, to ensure that the critical section's memory
    // references happen strictly after the lock is acquired.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();
  
    // Record info about lock acquisition for holding() and debugging.
    lk->cpu = mycpu();
  }
  ```

- 每次迭代将1与`lk->locked`进行swap操作，并检查`lk->locked`之前的值。

  - 如果之前为0，swap已经把`lk->locked`设置为1，那么我们就获得了锁；
  - 如果前一个值是1，那么另一个CPU持有锁，我们原子地将1与`lk->locked`进行swap的事实并没有改变它的值。

- 获取锁后，用于调试，`acquire`将记录下来获取锁的CPU。`lk->cpu`字段受锁保护，只能在保持锁时更改。

函数`release`(***kernel/spinlock.c***:47) 与`acquire`相反：它清除`lk->cpu`字段，然后释放锁。从概念上讲，`release`只需要将0分配给`lk->locked`。C标准允许编译器用多个存储指令实现赋值，因此对于并发代码，C赋值可能是非原子的。因此`release`使用执行原子赋值的C库函数`__sync_lock_release`。该函数也可以归结为RISC-V的`amoswap`指令。



## 6.3 代码：使用锁

使用锁的一个困难部分是决定要使用多少锁，以及每个锁应该保护哪些数据和不变量。有几个基本原则。

- 任何时候可以被一个CPU写入，同时又可以被另一个CPU读写的变量，都应该使用锁来防止两个操作重叠
- 锁保护不变量（invariants）：如果一个不变量涉及多个内存位置，通常所有这些位置都需要由一个锁来保护，以确保不变量不被改变

作为粗粒度锁的一个例子，xv6的***kalloc.c***分配器有一个由单个锁保护的空闲列表。如果不同CPU上的多个进程试图同时分配页面，每个进程在获得锁之前将必须在`acquire`中自旋等待。

- 自旋会降低性能，因为它只是无用的等待。如果对锁的争夺浪费了很大一部分CPU时间，也许可以通过改变分配器的设计来提高性能，使其拥有多个空闲列表，每个列表都有自己的锁，以允许真正的并行分配

作为细粒度锁定的一个例子，xv6对每个文件都有一个单独的锁，这样操作不同文件的进程通常可以不需等待彼此的锁而继续进行。

- 文件锁的粒度可以进一步细化，以允许进程同时写入同一个文件的不同区域。最终的锁粒度决策需要由性能测试和复杂性考量来驱动。



## 6.4 死锁和锁排序

如果在内核中执行的代码路径必须同时持有数个锁，那么**所有代码路径以相同的顺序获取这些锁**是很重要的。如果它们不这样做，就有死锁的风险。

文件系统代码包含xv6最长的锁链。例如，创建一个文件需要同时持有目录上的锁、新文件inode上的锁、磁盘块缓冲区上的锁、磁盘驱动程序的`vdisk_lock`和调用进程的`p->lock`。为了避免死锁，文件系统代码总是按照前一句中提到的顺序获取锁。



## 6.5 锁和中断处理函数

一些xv6自旋锁保护线程和中断处理程序共用的数据。例如，`clockintr`定时器中断处理程序在增加`ticks`(***kernel/trap.c***:163)的同时内核线程可能在`sys_sleep`(***kernel/sysproc.c***:64)中读取`ticks`。锁`tickslock`串行化这两个访问。

自旋锁和中断的交互引发了潜在的危险。

- 假设`sys_sleep`持有`tickslock`，并且它的CPU被计时器中断中断
- `clockintr`会尝试获取`tickslock`，意识到它被持有后等待释放。
- 在这种情况下，`tickslock`永远不会被释放：只有`sys_sleep`可以释放它，但是`sys_sleep`直到`clockintr`返回前不能继续运行。所以CPU会死锁，任何需要锁的代码也会冻结。

为了避免这种情况，**如果一个自旋锁被中断处理程序所使用，那么CPU必须保证在启用中断的情况下永远不能持有该锁**。

- Xv6更保守：**当CPU获取任何锁时，xv6总是禁用该CPU上的中断**。中断仍然可能发生在其他CPU上，此时中断的`acquire`可以等待线程释放自旋锁；由于不在同一CPU上，不会造成死锁。

当CPU未持有自旋锁时，xv6重新启用中断；它必须做一些记录来处理嵌套的临界区域。

- ```c
  void
  push_off(void)
  {
    int old = intr_get();
  
    intr_off();
    if(mycpu()->noff == 0)
      mycpu()->intena = old;
    mycpu()->noff += 1;
  }
  
  void
  pop_off(void)
  {
    struct cpu *c = mycpu();
    if(intr_get())
      panic("pop_off - interruptible");
    if(c->noff < 1)
      panic("pop_off");
    c->noff -= 1;
    if(c->noff == 0 && c->intena)
      intr_on();
  }
  ```

- `acquire`调用`push_off`并且`release`调用`pop_off` 来跟踪当前CPU上锁的嵌套级别。

  - 当计数达到零时，`pop_off`恢复最外层临界区域开始时存在的中断使能状态

- `intr_off`和`intr_on`函数执行RISC-V指令分别用来禁用和启用中断

严格的在设置`lk->locked`之前让`acquire`调用`push_off`是很重要的。如果两者颠倒，会存在一个既持有锁又启用了中断的短暂窗口期，不幸的话定时器中断会使系统死锁。同样，只有在释放锁之后，`release`才调用`pop_off`也是很重要的



## 6.6 指令和内存访问顺序

编译器和CPU在重新排序时需要遵循一定规则，以确保它们不会改变正确编写的串行代码的结果。然而，规则确实允许重新排序后改变并发代码的结果，并且很容易导致多处理器上的不正确行为。CPU的排序规则称为内存模型（memory model）。

例如，在`push`的代码中，如果编译器或CPU将对应于第4行的存储指令移动到第6行`release`后的某个地方，那将是一场灾难：

```c
l = malloc(sizeof *l);
l->data = data;
acquire(&listlock);
l->next = list;
list = l;
release(&listlock);
```

如果发生这样的重新排序，将会有一个窗口期，另一个CPU可以获取锁并查看更新后的`list`，但却看到一个未初始化的`list->next`。

为了告诉硬件和编译器不要执行这样的重新排序，xv6在`acquire`和`release`中都使用了`__sync_synchronize()`。

- `__sync_synchronize()`是一个内存障碍：它告诉编译器和CPU不要跨障碍重新排序`load`或`store`指令。因为xv6在访问共享数据时使用了锁，xv6的`acquire`和`release`中的障碍在几乎所有重要的情况下都会强制顺序执行。



## 6.7 睡眠锁

**自旋锁的缺点**

- 有时xv6需要长时间保持锁。例如，文件系统（第8章）在磁盘上读写文件内容时保持文件锁定，这些磁盘操作可能需要几十毫秒。如果另一个进程想要获取自旋锁，那么长时间保持自旋锁会导致获取进程在自旋时浪费很长时间的CPU。
- 一个进程在持有自旋锁的同时不能让出（yield）CPU，然而我们希望持有锁的进程等待磁盘I/O的时候其他进程可以使用CPU

Xv6以睡眠锁（sleep-locks）的形式提供了这种锁

- 它在等待获取锁时让出CPU，并允许在持有锁时让步（以及中断）

- `acquiresleep` (***kernel/sleeplock.c***:22) 在等待时让步CPU

  - ```c
    void
    acquiresleep(struct sleeplock *lk)
    {
      acquire(&lk->lk);
      while (lk->locked) {
        sleep(lk, &lk->lk);
      }
      lk->locked = 1;
      lk->pid = myproc()->pid;
      release(&lk->lk);
    }
    ```

  - 睡眠锁有一个被自旋锁保护的锁定字段，`acquiresleep`对`sleep`的调用原子地让出CPU并释放自旋锁。结果是其他线程可以在`acquiresleep`等待时执行。

因为睡眠锁保持中断使能，所以它们**不能用在中断处理程序中**。因为`acquiresleep`可能会让出CPU，所以**睡眠锁不能在自旋锁临界区域中使用**（尽管自旋锁可以在睡眠锁临界区域中使用）。

自旋锁最适合短的临界区域；睡眠锁对于冗长的操作效果很好



# Chapter8 部分内容

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



# 实验

## Task1

### 要求

`kalloctest`中锁争用的根本原因是`kalloc()`有一个空闲列表，由一个锁保护。要消除锁争用，您必须重新设计内存分配器，以避免使用单个锁和列表。基本思想是为每个CPU维护一个空闲列表，每个列表都有自己的锁。因为每个CPU将在不同的列表上运行，不同CPU上的分配和释放可以并行运行。主要的挑战将是处理一个CPU的空闲列表为空，而另一个CPU的列表有空闲内存的情况；在这种情况下，一个CPU必须“窃取”另一个CPU空闲列表的一部分。窃取可能会引入锁争用，但这种情况希望不会经常发生。

您的工作是实现每个CPU的空闲列表，并在CPU的空闲列表为空时进行窃取。所有锁的命名必须以“`kmem`”开头。也就是说，您应该为每个锁调用`initlock`，并传递一个以“`kmem`”开头的名称。运行`kalloctest`以查看您的实现是否减少了锁争用。要检查它是否仍然可以分配所有内存，请运行`usertests sbrkmuch`。您的输出将与下面所示的类似，在`kmem`锁上的争用总数将大大减少，尽管具体的数字会有所不同。确保`usertests`中的所有测试都通过。评分应该表明考试通过。



### 提示

- 您可以使用***kernel/param.h***中的常量`NCPU`
- 让`freerange`将所有可用内存分配给运行`freerange`的CPU。
- 函数`cpuid`返回当前的核心编号，但只有在中断关闭时调用它并使用其结果才是安全的。您应该使用`push_off()`和`pop_off()`来关闭和打开中断。
- 看看***kernel/sprintf.c***中的`snprintf`函数，了解字符串如何进行格式化。尽管可以将所有锁命名为“`kmem`”。



### 实现

题目要求就是把原来所有CPU公用的一个大的空闲列表分给各个CPU使用，然后如果自己的空闲列表没了就去别的CPU那里拿

先定义一个空闲列表的数组

- ```c
  struct kmem{
    struct spinlock lock;
    struct run *freelist;
  };
  
  struct kmem cpu_kmem[NCPU];
  ```

- 注意，在原本的代码中，上面的结构是一个匿名结构

  - ```c
    struct{
      struct spinlock lock;
      struct run *freelist;
    }kmem;

  - kmem是一个变量而不是结构名，而匿名结构是不能用来指定其他变量的类型的，所以如果不修改这里而直接用 `struct kmem cpu_kmem[NCPU];` ，会报错

修改 `kinit`

- 为每个cpu的空闲列表添加一个对应的锁

- `freerange` 可以不修改，反正都是要把所有的空间都释放，没必要放在循环中

- ```c
  void
  kinit()
  {
    // initlock(&kmem.lock, "kmem");
    // freerange(end, (void*)PHYSTOP);
    for (int i = 0; i < NCPU; ++i) {
      initlock(&cpu_kmem[i].lock, "kmem");
    }
    freerange(end, (void*)PHYSTOP);
  }
  ```

修改 `kfree`

- 只要简单修改一下，获取当前cpu，然后释放当前cpu的空闲列表

- 记得用 `cpuid` 这个函数的时候要关中断 `push_off`

- ```c
  void
  kfree(void *pa)
  {
    struct run *r;
    push_off();
    int cpu_id = cpuid();
    pop_off();
  
    if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
      panic("kfree");
  
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
  
    r = (struct run*)pa;
  
    acquire(&cpu_kmem[cpu_id].lock);
    r->next = cpu_kmem[cpu_id].freelist;
    cpu_kmem[cpu_id].freelist = r;
    release(&cpu_kmem[cpu_id].lock);
  }
  ```

修改 `kalloc`

- 有一部分的逻辑和原来是一样的，只不过原来是从整个空闲列表中获取，现在是在当前cpu的空闲列表中获取

- 需要解决的是如果当前cpu没有空闲页了，那么要从其他cpu处拿

  - 写一个循环遍历其他所有的cpu，如果找到空闲页，就返回
  - 注意，操作其他cpu的空闲页的时候也要加对应的锁

- ```c
  void *
  kalloc(void)
  {
    // struct run *r;
  
    // acquire(&kmem.lock);
    // r = kmem.freelist;
    // if(r)
    //   kmem.freelist = r->next;
    // release(&kmem.lock);
  
    // if(r)
    //   memset((char*)r, 5, PGSIZE); // fill with junk
    // return (void*)r;
  
    struct run *r;
    push_off();
    int cpu_id = cpuid();
    pop_off();
  
    acquire(&cpu_kmem[cpu_id].lock);
    r = cpu_kmem[cpu_id].freelist;
    if(r) {
      cpu_kmem[cpu_id].freelist = r->next;
    }
    else {
      for (int id = 0; id < NCPU; ++id) {
        if (id == cpu_id)
          continue;
        acquire(&cpu_kmem[id].lock);
        r = cpu_kmem[id].freelist;
        if (r) {
          cpu_kmem[id].freelist = r->next;
          release(&cpu_kmem[id].lock);
          break;
        }
        // 注意，这里也要释放锁，因为上面的if语句可能不成立
        // 如果漏掉了的话测试会panic
        release(&cpu_kmem[id].lock);
      }
    }
    release(&cpu_kmem[cpu_id].lock);
    if(r)
      memset((char*)r, 5, PGSIZE); // fill with junk
    return (void*)r;
  }
  ```

`kalloc`的另一种写法

- 这是网上看到的，一次性多拿几页，从而减少拿的频率

- ```c
  void *
  kalloc(void)
  {
    struct run *r;
  
    push_off();
  
    int cpu = cpuid();
  
    acquire(&kmem[cpu].lock);
  
    if(!kmem[cpu].freelist) { // no page left for this cpu
      int steal_left = 64; // steal 64 pages from other cpu(s)
      for(int i=0;i<NCPU;i++) {
        if(i == cpu) continue; // no self-robbery
        acquire(&kmem[i].lock);
        struct run *rr = kmem[i].freelist;
        while(rr && steal_left) {
          kmem[i].freelist = rr->next;
          rr->next = kmem[cpu].freelist;
          kmem[cpu].freelist = rr;
          rr = kmem[i].freelist;
          steal_left--;
        }
        release(&kmem[i].lock);
        if(steal_left == 0) break; // done stealing
      }
    }
  
    r = kmem[cpu].freelist;
    if(r)
      kmem[cpu].freelist = r->next;
    release(&kmem[cpu].lock);
  
    pop_off();
  
    if(r)
      memset((char*)r, 5, PGSIZE); // fill with junk
    return (void*)r;
  }
  ```

### 补充

这里体现了一个先 profile 再进行优化的思路。如果一个大锁并不会引起明显的性能问题，有时候大锁就足够了。只有在万分确定性能热点是在该锁的时候才进行优化，「过早优化是万恶之源」。

这里解决性能热点的思路是「将共享资源变为不共享资源」。锁竞争优化一般有几个思路：

- 只在必须共享的时候共享（对应为将资源从 CPU 共享拆分为每个 CPU 独立）
- 必须共享时，尽量减少在关键区中停留的时间（对应“大锁化小锁”，降低锁的粒度）

该 lab 的实验目标，即是为每个 CPU 分配独立的 freelist，这样多个 CPU 并发分配物理页就不再会互相排斥了，提高了并行性。

但由于在一个 CPU freelist 中空闲页不足的情况下，仍需要从其他 CPU 的 freelist 中“偷”内存页，所以一个 CPU 的 freelist 并不是只会被其对应 CPU 访问，还可能在“偷”内存页的时候被其他 CPU 访问，故仍然需要使用单独的锁来保护每个 CPU 的 freelist。但一个 CPU freelist 中空闲页不足的情况相对来说是比较稀有的，所以总体性能依然比单独 kmem 大锁要快。在最佳情况下，也就是没有发生跨 CPU “偷”页的情况下，这些小锁不会发生任何锁竞争。



## Task2

如果多个进程密集地使用文件系统，它们可能会争夺`bcache.lock`，它保护***kernel/bio.c***中的磁盘块缓存。`bcachetest`创建多个进程，这些进程重复读取不同的文件，以便在`bcache.lock`上生成争用；在`bcache.lock`上生成争用；（在完成本实验之前）其输出如下所示：

```bash
$ bcachetest
start test0
test0 results:
--- lock kmem/bcache stats
lock: kmem: #fetch-and-add 0 #acquire() 33035
lock: bcache: #fetch-and-add 16142 #acquire() 65978
--- top 5 contended locks:
lock: virtio_disk: #fetch-and-add 162870 #acquire() 1188
lock: proc: #fetch-and-add 51936 #acquire() 73732
lock: bcache: #fetch-and-add 16142 #acquire() 65978
lock: uart: #fetch-and-add 7505 #acquire() 117
lock: proc: #fetch-and-add 6937 #acquire() 73420
tot= 16142
test0: FAIL
start test1
test1 OK
```

您可能会看到不同的输出，但`bcache`锁的`acquire`循环迭代次数将很高。如果查看***kernel/bio.c***中的代码，您将看到`bcache.lock`保护已缓存的块缓冲区的列表、每个块缓冲区中的引用计数（`b->refcnt`）以及缓存块的标识（`b->dev`和`b->blockno`）。



 YOUR JOB

修改块缓存，以便在运行`bcachetest`时，bcache（buffer cache的缩写）中所有锁的`acquire`循环迭代次数接近于零。理想情况下，块缓存中涉及的所有锁的计数总和应为零，但只要总和小于500就可以。修改`bget`和`brelse`，以便bcache中不同块的并发查找和释放不太可能在锁上发生冲突（例如，不必全部等待`bcache.lock`）。你必须保护每个块最多缓存一个副本的不变量。完成后，您的输出应该与下面显示的类似（尽管不完全相同）。确保`usertests`仍然通过。完成后，`make grade`应该通过所有测试。

```bash
$ bcachetest
start test0
test0 results:
--- lock kmem/bcache stats
lock: kmem: #fetch-and-add 0 #acquire() 32954
lock: kmem: #fetch-and-add 0 #acquire() 75
lock: kmem: #fetch-and-add 0 #acquire() 73
lock: bcache: #fetch-and-add 0 #acquire() 85
lock: bcache.bucket: #fetch-and-add 0 #acquire() 4159
lock: bcache.bucket: #fetch-and-add 0 #acquire() 2118
lock: bcache.bucket: #fetch-and-add 0 #acquire() 4274
lock: bcache.bucket: #fetch-and-add 0 #acquire() 4326
lock: bcache.bucket: #fetch-and-add 0 #acquire() 6334
lock: bcache.bucket: #fetch-and-add 0 #acquire() 6321
lock: bcache.bucket: #fetch-and-add 0 #acquire() 6704
lock: bcache.bucket: #fetch-and-add 0 #acquire() 6696
lock: bcache.bucket: #fetch-and-add 0 #acquire() 7757
lock: bcache.bucket: #fetch-and-add 0 #acquire() 6199
lock: bcache.bucket: #fetch-and-add 0 #acquire() 4136
lock: bcache.bucket: #fetch-and-add 0 #acquire() 4136
lock: bcache.bucket: #fetch-and-add 0 #acquire() 2123
--- top 5 contended locks:
lock: virtio_disk: #fetch-and-add 158235 #acquire() 1193
lock: proc: #fetch-and-add 117563 #acquire() 3708493
lock: proc: #fetch-and-add 65921 #acquire() 3710254
lock: proc: #fetch-and-add 44090 #acquire() 3708607
lock: proc: #fetch-and-add 43252 #acquire() 3708521
tot= 128
test0: OK
start test1
test1 OK
$ usertests
  ...
ALL TESTS PASSED
$
```

请将你所有的锁以“`bcache`”开头进行命名。也就是说，您应该为每个锁调用`initlock`，并传递一个以“`bcache`”开头的名称。

减少块缓存中的争用比`kalloc`更复杂，因为bcache缓冲区真正的在进程（以及CPU）之间共享。对于`kalloc`，可以通过给每个CPU设置自己的分配器来消除大部分争用；这对块缓存不起作用。我们建议您使用每个哈希桶都有一个锁的哈希表在缓存中查找块号。

在您的解决方案中，以下是一些存在锁冲突但可以接受的情形：

- 当两个进程同时使用相同的块号时。`bcachetest test0`始终不会这样做。
- 当两个进程同时在cache中未命中时，需要找到一个未使用的块进行替换。`bcachetest test0`始终不会这样做。
- 在你用来划分块和锁的方案中某些块可能会发生冲突，当两个进程同时使用冲突的块时。例如，如果两个进程使用的块，其块号散列到哈希表中相同的槽。`bcachetest test0`可能会执行此操作，具体取决于您的设计，但您应该尝试调整方案的细节以避免冲突（例如，更改哈希表的大小）。

`bcachetest`的`test1`使用的块比缓冲区更多，并且执行大量文件系统代码路径。



### 提示

- 请阅读xv6手册中对块缓存的描述（第8.1-8.3节）。
- 可以使用固定数量的散列桶，而不动态调整哈希表的大小。使用素数个存储桶（例如13）来降低散列冲突的可能性。
- 在哈希表中搜索缓冲区并在找不到缓冲区时为该缓冲区分配条目必须是原子的。
- 删除保存了所有缓冲区的列表（`bcache.head`等），改为标记上次使用时间的时间戳缓冲区（即使用***kernel/trap.c***中的`ticks`）。通过此更改，`brelse`不需要获取bcache锁，并且`bget`可以根据时间戳选择最近使用最少的块。
- 可以在`bget`中串行化回收（即`bget`中的一部分：当缓存中的查找未命中时，它选择要复用的缓冲区）。
- 在某些情况下，您的解决方案可能需要持有两个锁；例如，在回收过程中，您可能需要持有bcache锁和每个bucket（散列桶）一个锁。确保避免死锁。
- 替换块时，您可能会将`struct buf`从一个bucket移动到另一个bucket，因为新块散列到不同的bucket。您可能会遇到一个棘手的情况：新块可能会散列到与旧块相同的bucket中。在这种情况下，请确保避免死锁。
- 一些调试技巧：实现bucket锁，但将全局`bcache.lock`的`acquire`/`release`保留在`bget`的开头/结尾，以串行化代码。一旦您确定它在没有竞争条件的情况下是正确的，请移除全局锁并处理并发性问题。您还可以运行`make CPUS=1 qemu`以使用一个内核进行测试。



### 实现

做了一下午没做出来，参考了别人的代码

因为不像 kalloc 中一个物理页分配后就只归单个进程所管，bcache 中的区块缓存是会被多个进程（进一步地，被多个 CPU）共享的（由于多个进程可以同时访问同一个区块）。所以 kmem 中为每个 CPU 预先分割一部分专属的页的方法在这里是行不通的。

前面提到的：

> 锁竞争优化一般有几个思路：
>
> - 只在必须共享的时候共享（对应为将资源从 CPU 共享拆分为每个 CPU 独立）
> - 必须共享时，尽量减少在关键区中停留的时间（对应“大锁化小锁”，降低锁的粒度）

在这里， bcache 属于“必须共享”的情况，所以需要用到第二个思路，降低锁的粒度，用更精细的锁 scheme 来降低出现竞争的概率。

原版 xv6 的设计中，使用双向链表存储所有的区块缓存，每次尝试获取一个区块 blockno 的时候，会遍历链表，如果目标区块已经存在缓存中则直接返回，如果不存在则选取一个最近最久未使用的，且引用计数为 0 的 buf 块作为其区块缓存，并返回。

新的改进方案，可以**建立一个从 blockno 到 buf 的哈希表，并为每个桶单独加锁**。这样，仅有在两个进程同时访问的区块同时哈希到同一个桶的时候，才会发生锁竞争。当桶中的空闲 buf 不足的时候，从其他的桶中获取 buf。

思路上是很简单的，但是具体实现的时候，需要注意死锁问题。这里的许多死锁问题比较隐晦，而且 bcachetest 测试不出来，但是在实际运行的系统中，是有可能触发死锁的。网上看过许多其他通过了的同学的博客，代码中都没有注意到这一点。

#### 死锁问题

考虑一下我们的新设计，首先在 bcache 中定义哈希表 bufmap，并为每个桶设置锁：

```c
// kernel/bio.h
struct {
  struct buf buf[NBUF];
  struct spinlock eviction_lock;

  // Hash map: dev and blockno to buf
  struct buf bufmap[NBUFMAP_BUCKET];
  struct spinlock bufmap_locks[NBUFMAP_BUCKET];
} bcache;
```

bget(uint dev, uint blockno) 中，首先在 blockno 对应桶中扫描缓存是否存在，如果不存在，则在**所有桶**中寻找一个最近最久未使用的无引用 buf，进行缓存驱逐，然后将其重新移动到 blockno 对应的桶中（rehash），作为 blockno 的缓存返回。

这里很容易就会写出这样的代码：

```c
bget(dev, blockno) {
  key := hash(dev, blockno);

  acquire(bufmap_locks[key]); // 获取 key 桶的锁
  
  // 查找 blockno 的缓存是否存在，若是直接返回，若否继续执行
  if(b := look_for_blockno_in(bufmap[key])) {
    b->refcnt++
    release(bufmap_locks[key]);
    return b;
  }

  // 查找可驱逐缓存 b
  
  least_recently := NULL;
  
  for i := [0, NBUFMAP_BUCKET) { // 遍历所有的桶
    acquire(bufmap_locks[i]);    // 获取第 i 桶的锁

    b := look_for_least_recently_used_with_no_ref(bufmap[key]);
    // 如果找到未使用时间更长的空闲块
    if(b.last_use < least_recently.last_use) {  
      least_recently := b;
    }

    release(bufmap_locks[i]);   // 查找结束后，释放第 i 桶的锁
  }

  b := least_recently;

  // 驱逐 b 原本存储的缓存（将其从原来的桶删除）
  evict(b);

  // 将 b 加入到新的桶
  append(bucket[key], b);

  release(bufmap_locks[key]); // 释放 key 桶的锁

  // 设置 b 的各个属性
  setup(b);
  return b;
}
```

上面的代码看起来很合理，但是却有两个问题，一个导致运行结果出错，一个导致死锁。

##### 问题1：可驱逐 buf 在所对应桶锁释放后不保证仍可驱逐

第一个问题比较显而易见，后面进行缓存驱逐的时候，每扫描一个桶前会获取该桶的锁，但是每扫描完一个桶后又释放了该桶的锁。**从释放锁的那一瞬间，获取出来的最近最久未使用的空闲 buf 就不再可靠了**。因为在我们释放 b 原来所在的桶的锁后（`release(bufmap_locks[i]);` 后），但是从原桶删除 b 之前（`evict(b);` 前），另一个 CPU 完全可能会调用 bget 请求 b，使得 b 的引用计数变为不为零。此时我们对 b 进行驱逐就是不安全的了。

解决方法也并不复杂，只需要在扫描桶的时候，确保找到最近最久未使用的空闲 buf 后，不释放桶锁，继续持有其对应的桶的锁直到驱逐完成即可。

> 这里维护的不变量（invariant）是：「扫描到的 buf 在驱逐完成前保持可驱逐」，以及「桶中若存在某个块的 buf，则这个 buf 可用，bget可以直接返回这个 buf」。

```c
bget(dev, blockno) {
  acquire(bufmap_locks[key]); // 获取 key 桶锁
  
  // 查找 blockno 的缓存是否存在，若是直接返回，若否继续执行
  if(b := look_for_blockno_in(bufmap[key])) {
    b->refcnt++
    release(bufmap_locks[key]);
    return b;
  }

  // 缓存不存在，查找可驱逐缓存 b
  
  least_recently := NULL;
  holding_bucket := -1;
  
  for i := [0, NBUFMAP_BUCKET) { // 遍历所有的桶
    acquire(bufmap_locks[i]);    // 获取第 i 桶的锁

    b := look_for_least_recently_used_with_no_ref(bufmap[key]);
    // 如果找到未使用时间更长的空闲块（新的 least_recently）
    
    if(b.last_use >= least_recently.last_use) {
      release(bufmap_locks[i]);   // 该桶中没有找到新的 least_recently，释放该桶的锁

    } else {
      // b.last_use < least_recently.last_use
      least_recently := b;

      // 释放原本 holding 的锁（holding_bucket < i）
      if(holding_bucket != -1 && holding_bucket != key) release(bufmap_locks[holding_bucket]);
      // 保持第 i 桶的锁不释放......
      holding_bucket := i;
    }
  }

  b := least_recently;

  // 此时，仍然持有 b 所在的桶的锁 bufmap_locks[holding_bucket]
  // 驱逐 b 原本存储的缓存（将其从原来的桶删除）
  evict(b);
  release(bufmap_locks[holding_bucket]); // 驱逐后再释放 b 原本所在桶的锁

  // 将 b 加入到新的桶
  append(bucket[key], b);

  release(bufmap_locks[key]); // 释放 key 桶锁

  // 设置 b 的各个属性
  setup(b);
  return b;
}
```

##### 问题2：两个请求形成环路死锁

这里出现的第二个问题就是，一开始我们在 blockno 对应的桶中遍历检查缓存是否存在时，获取了它的锁。而在我们发现 blockno 不存在缓存中之后，需要在拿着 key 桶锁的同时，**遍历所有的桶并依次获取它们每个的锁**，考虑这种情况：

```
假设块号 b1 的哈希值是 2，块号 b2 的哈希值是 5
并且两个块在运行前都没有被缓存
----------------------------------------
CPU1                  CPU2
----------------------------------------
bget(dev, b1)         bget(dev,b2)
    |                     |
    V                     V
获取桶 2 的锁           获取桶 5 的锁
    |                     |
    V                     V
缓存不存在，遍历所有桶    缓存不存在，遍历所有桶
    |                     |
    V                     V
  ......                遍历到桶 2
    |                尝试获取桶 2 的锁
    |                     |
    V                     V
  遍历到桶 5          桶 2 的锁由 CPU1 持有，等待释放
尝试获取桶 5 的锁
    |
    V
桶 5 的锁由 CPU2 持有，等待释放

!此时 CPU1 等待 CPU2，而 CPU2 在等待 CPU1，陷入死锁!
```

这里，由于 CPU1 持有锁 2 的情况下去申请锁 5，而 CPU2 持有锁 5 的情况下申请锁 2，造成了**环路等待**。

复习一下死锁的四个条件：

1. 互斥（一个资源在任何时候只能属于一个线程）
2. 请求保持（线程在拿着一个锁的情况下，去申请另一个锁）
3. 不剥夺（外力不强制剥夺一个线程已经拥有的资源）
4. 环路等待（请求资源的顺序形成了一个环）

只要破坏了四个条件中的任何一个，就能破坏死锁。为了尝试解决这个死锁问题，我们考虑破坏每一个条件的可行性：

1. 互斥：在这里一个桶只能同时被一个 CPU（线程）处理，互斥条件是必须的，无法破坏。
2. 请求保持
3. 不剥夺：遍历桶的时候，在环路请求出现时强行释放一方的锁？即使能检测，被强制释放锁的一方的 bget 请求会失败，造成文件系统相关系统调用失败，不可行。
4. 环路等待：改变访问顺序，比如永远只遍历当前 key 左侧的桶，使得无论如何访问都不会出现环路？可解决死锁，但假设 blockno 哈希到第一个桶，并且 cache missed 时，将无法进行缓存驱逐来腾出新块供其使用（因为第一个桶左侧没有任何桶）。

从「互斥」、「不剥夺」和「环路等待」条件入手都无法解决这个死锁问题，那只能考虑「请求保持」了。

这里死锁出现的原因是我们在拿着一个锁的情况下，去尝试申请另一个锁，并且请求顺序出现了环路。既然带环路的请求顺序是不可避免的，那唯一的选项就是**在申请任何其他桶锁之前，先放弃之前持有的 key 的桶锁**，在找到并驱逐最近最久未使用的空闲块 b 后，再重新获取 key 的桶锁，将 b 加入桶。

大致代码是这样：

```c
bget(dev, blockno) {
  acquire(bufmap_locks[key]); // 获取 key 桶锁
  
  // 查找 blockno 的缓存是否存在，若是直接返回，若否继续执行
  if(b := look_for_blockno_in(bufmap[key])) {
    b->refcnt++
    release(bufmap_locks[key]);
    return b;
  }

  release(bufmap_locks[key]); // 先释放 key 桶锁，防止查找驱逐时出现环路等待

  // 缓存不存在，查找可驱逐缓存 b
  
  holding_bucket := -1;
  for i := [0, NBUFMAP_BUCKET) {
    acquire(bufmap_locks[i]); // 请求时不持有 key 桶锁，不会出现环路等待
    if(b := look_for_least_recently_used_with_no_ref(bufmap[key])) {
      if(holding_bucket != -1) release(bufmap_locks[holding_bucket]);
      holding_bucket := i;
      // 如果找到新的未使用时间更长的空闲块，则将原来的块所属桶的锁释放掉，保持新块所属桶的锁...
    } else {
      release(bufmap_locks[holding_bucket]);
    }
  }

  // 驱逐 b 原本存储的缓存（将其从原来的桶删除）
  evict(b);
  release(bufmap_locks[holding_bucket]); // 释放 b 原所在桶的锁

  acquire(bufmap_locks[key]); // 再次获取 key 桶锁
  append(b, bucket[key]);     // 将 b 加入到新的桶
  release(bufmap_locks[key]); // 释放 key 桶锁


  // 设置 b 的各个属性
  setup(b);
  return b;
}
```

这样以来，bget 中无论任何位置，获取桶锁的时候都要么没拿其他锁，要么只拿了其左侧的桶锁（遍历所有桶查找可驱逐缓存 b 的过程中，对桶的遍历固定从小到大访问），所以永远不会出现环路，死锁得到了避免。但是这样的方案又会带来新的问题。

##### 新的问题：释放自身桶锁可能使得同 blockno 重复驱逐与分配

注意到我们开始「搜索所有桶寻找可驱逐的 buf」这个过程前，为了防止环路等待，而释放了 key 的桶锁（key 为请求的 blockno 的哈希值），直到遍历所有桶并驱逐最近最久未使用的空闲 buf 的过程完成后才重新获取 key 桶锁。问题在于，在释放掉 key 桶锁之后，第一块关键区（“查找 blockno 的缓存是否存在，若是直接返回，若否继续执行”的区域）就得不到锁保护了。这意味着在「释放掉 key 桶锁后」到「重新获取 key 桶锁前」的这个阶段，也就是我们进行驱逐+重分配时，另外一个 CPU 完全有可能访问同一个 blockno，获取到 key 的桶锁，通过了一开始「缓存不存在」的测试，然后也进入到驱逐+重分配中，导致「一个区块有多份缓存」的错误情况出现。

怎么保障同一个区块不会有两个缓存呢？

这个问题相对比较棘手，我们目前知道的限制条件有：

- 在遍历桶查找可驱逐 buf 的过程中，不能持有 key 的桶锁，否则会出现死锁。
- 在遍历桶查找可驱逐 buf 的过程中，不持有 key 桶锁的话，可能会有其他 CPU 访问同一 blockno，并完成驱逐+重分配，导致同一 blockno 被重复缓存。

这里不得不承认，我并没有想到什么特别好的方法，只想到了一个牺牲一点效率，但是能保证极端情况下安全的方案：

- 添加 eviction_lock，将驱逐+重分配的过程限制为单线程

  注意此处应该先释放桶锁后，再获取 eviction_lock。写反会导致 eviction_lock 和桶锁发生死锁。（线程 1 拿着桶 A 锁请求 eviction_lock， 线程 2 拿着 eviction_lock 驱逐时遍历请求到桶 A 锁）

  ` `

```c
bget(dev, blockno) {
  acquire(bufmap_locks[key]); // 获取 key 桶锁
    
  // 查找 blockno 的缓存是否存在，若是直接返回，若否继续执行
  if(b := look_for_blockno_in(bufmap[key])) {
    b->refcnt++
    release(bufmap_locks[key]);
    return b;
  }
    
  // 注意这里的 acquire 和 release 的顺序
  release(bufmap_locks[key]); // 先释放 key 桶锁，防止查找驱逐时出现环路死锁
  acquire(eviction_lock);     // 获得驱逐锁，防止多个 CPU 同时驱逐影响后续判断

  // 缓存不存在，查找可驱逐缓存 b
    
  // .......

  acquire(bufmap_locks[key]); // 再次获取 key 桶锁
  append(b, bucket[key]);     // 将 b 加入到新的桶
  release(bufmap_locks[key]); // 释放 key 桶锁

  release(eviction_lock);     // 释放驱逐锁

  // 设置 b 的各个属性
  setup(b);
  return b;
}
```

在获取 eviction_lock 之后，马上**再次判断 blockno 的缓存是否存在**，若是直接返回，若否继续执行

```c
bget(dev, blockno) {
  acquire(bufmap_locks[key]); // 获取 key 桶锁
    
  // 查找 blockno 的缓存是否存在，若是直接返回，若否继续执行
  if(b := look_for_blockno_in(bufmap[key])) {
    b->refcnt++
    release(bufmap_locks[key]);
    return b;
  }
    
  // 注意这里的 acquire 和 release 的顺序
  release(bufmap_locks[key]); // 先释放 key 桶锁，防止查找驱逐时出现环路死锁
  acquire(eviction_lock);     // 获得驱逐锁，防止多个 CPU 同时驱逐影响后续判断

  // **再次查找 blockno 的缓存是否存在**，若是直接返回，若否继续执行
  // 这里由于持有 eviction_lock，没有任何其他线程能够进行驱逐操作，所以
  // 没有任何其他线程能够改变 bufmap[key] 桶链表的结构，所以这里不事先获取
  // 其相应桶锁而直接开始遍历是安全的。
  if(b := look_for_blockno_in(bufmap[key])) {
    acquire(bufmap_locks[key]); // 必须获取，保护非原子操作 `refcnt++`
    b->refcnt++
    release(bufmap_locks[key]);

    release(eviction_lock);
    return b;
  }

  // 缓存不存在，查找可驱逐缓存 b
    
  // .......

  acquire(bufmap_locks[key]); // 再次获取 key 桶锁
  append(b, bucket[key]);     // 将 b 加入到新的桶
  release(bufmap_locks[key]); // 释放 key 桶锁

  release(eviction_lock);     // 释放驱逐锁

  // 设置 b 的各个属性
  setup(b);
  return b;
}
```

这样以来，即使有多个线程同时请求同一个 blockno，并且所有线程都碰巧通过了一开始的「blockno 的缓存是否存在」的判断且结果都为「缓存不存在」，则进入受 eviction_lock 保护的驱逐+重分配区代码后，能够实际进行驱逐+重分配的，也只有第一个进入的线程。

第一个线程进入并驱逐+重分配完毕后才释放 eviction_lock，此时 blockno 的缓存已经由不存在变为存在了，后续的所有线程此时进入后都会被第二次「blockno 缓存是否存在」的判断代码拦住，并直接返回已分配好的缓存 buf，而不会重复对同一个 blockno 进行驱逐+重分配。

这么做的好处是，保证了查找过程中不会出现死锁，并且不会出现极端情况下一个块产生多个缓存的情况。而坏处是，引入了全局 eviction_lock，使得原本可并发的遍历驱逐过程的并行性降低了。并且每一次 cache miss 的时候，都会多一次额外的桶遍历开销。

然而，cache miss 本身（hopefully）为比较稀有事件，并且对于 cache miss 的块，由于后续需要从**磁盘**中读入其数据，磁盘读入的耗时将比一次桶遍历的耗时多好几个数量级，所以我认为这样的方案的开销还是可以接受的。

> ps. 这样的设计，有一个名词称为「乐观锁（optimistic locking）」，即在冲突发生概率很小的关键区内，不使用独占的互斥锁，而是在提交操作前，检查一下操作的数据是否被其他线程修改（在这里，检测的是 blockno 的缓存是否已被加入），如果是，则代表冲突发生，需要特殊处理（在这里的特殊处理即为直接返回已加入的 buf）。这样的设计，相比较「悲观锁（pessimistic locking）」而言，可以在冲突概率较低的场景下（例如 bget），降低锁开销以及不必要的线性化，提升并行性（例如在 bget 中允许「缓存是否存在」的判断并行化）。有时候还能用于避免死锁。

#### 完整代码

```c
struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  uint lastuse; // *newly added, used to keep track of the least-recently-used buf
  struct buf *next;
  uchar data[BSIZE];
};
```

```c
// kernel/bio.c

// bucket number for bufmap
#define NBUFMAP_BUCKET 13
// hash function for bufmap
#define BUFMAP_HASH(dev, blockno) ((((dev)<<27)|(blockno))%NBUFMAP_BUCKET)

struct {
  struct buf buf[NBUF];
  struct spinlock eviction_lock;

  // Hash map: dev and blockno to buf
  struct buf bufmap[NBUFMAP_BUCKET];
  struct spinlock bufmap_locks[NBUFMAP_BUCKET];
} bcache;

void
binit(void)
{
  // Initialize bufmap
  for(int i=0;i<NBUFMAP_BUCKET;i++) {
    initlock(&bcache.bufmap_locks[i], "bcache_bufmap");
    bcache.bufmap[i].next = 0;
  }

  // Initialize buffers
  for(int i=0;i<NBUF;i++){
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    b->lastuse = 0;
    b->refcnt = 0;
    // put all the buffers into bufmap[0]
    b->next = bcache.bufmap[0].next;
    bcache.bufmap[0].next = b;
  }

  initlock(&bcache.eviction_lock, "bcache_eviction");
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint key = BUFMAP_HASH(dev, blockno);

  acquire(&bcache.bufmap_locks[key]);

  // Is the block already cached?
  for(b = bcache.bufmap[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.

  // to get a suitable block to reuse, we need to search for one in all the buckets,
  // which means acquiring their bucket locks.
  // but it's not safe to try to acquire every single bucket lock while holding one.
  // it can easily lead to circular wait, which produces deadlock.

  release(&bcache.bufmap_locks[key]);
  // we need to release our bucket lock so that iterating through all the buckets won't
  // lead to circular wait and deadlock. however, as a side effect of releasing our bucket
  // lock, other cpus might request the same blockno at the same time and the cache buf for  
  // blockno might be created multiple times in the worst case. since multiple concurrent
  // bget requests might pass the "Is the block already cached?" test and start the 
  // eviction & reuse process multiple times for the same blockno.
  //
  // so, after acquiring eviction_lock, we check "whether cache for blockno is present"
  // once more, to be sure that we don't create duplicate cache bufs.
  acquire(&bcache.eviction_lock);

  // Check again, is the block already cached?
  // no other eviction & reuse will happen while we are holding eviction_lock,
  // which means no link list structure of any bucket can change.
  // so it's ok here to iterate through `bcache.bufmap[key]` without holding
  // it's cooresponding bucket lock, since we are holding a much stronger eviction_lock.
  for(b = bcache.bufmap[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      acquire(&bcache.bufmap_locks[key]); // must do, for `refcnt++`
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      release(&bcache.eviction_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Still not cached.
  // we are now only holding eviction lock, none of the bucket locks are held by us.
  // so it's now safe to acquire any bucket's lock without risking circular wait and deadlock.

  // find the one least-recently-used buf among all buckets.
  // finish with it's corresponding bucket's lock held.
  struct buf *before_least = 0; 
  uint holding_bucket = -1;
  for(int i = 0; i < NBUFMAP_BUCKET; i++){
    // before acquiring, we are either holding nothing, or only holding locks of
    // buckets that are *on the left side* of the current bucket
    // so no circular wait can ever happen here. (safe from deadlock)
    acquire(&bcache.bufmap_locks[i]);
    int newfound = 0; // new least-recently-used buf found in this bucket
    for(b = &bcache.bufmap[i]; b->next; b = b->next) {
      if(b->next->refcnt == 0 && (!before_least || b->next->lastuse < before_least->next->lastuse)) {
        before_least = b;
        newfound = 1;
      }
    }
    if(!newfound) {
      release(&bcache.bufmap_locks[i]);
    } else {
      if(holding_bucket != -1) release(&bcache.bufmap_locks[holding_bucket]);
      holding_bucket = i;
      // keep holding this bucket's lock....
    }
  }
  if(!before_least) {
    panic("bget: no buffers");
  }
  b = before_least->next;
  
  if(holding_bucket != key) {
    // remove the buf from it's original bucket
    before_least->next = b->next;
    release(&bcache.bufmap_locks[holding_bucket]);
    // rehash and add it to the target bucket
    acquire(&bcache.bufmap_locks[key]);
    b->next = bcache.bufmap[key].next;
    bcache.bufmap[key].next = b;
  }
  
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  release(&bcache.bufmap_locks[key]);
  release(&bcache.eviction_lock);
  acquiresleep(&b->lock);
  return b;
}

// ......

// Release a locked buffer.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->lastuse = ticks;
  }
  release(&bcache.bufmap_locks[key]);
}

void
bpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt++;
  release(&bcache.bufmap_locks[key]);
}

void
bunpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);

  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  release(&bcache.bufmap_locks[key]);
}
```



# 结果

![image-20240620171942961](assets/image-20240620171942961.png)



# 补充

补充一下各个锁的作用：

- 拿着 bufmap_locks[key] 锁的时候，代表key桶这一个桶中的链表结构、以及所有链表节点的 refcnt 都不会被其他线程改变。也就是说，如果想访问/修改一个桶的结构，或者桶内任意节点的 refcnt，必须先拿那个桶 key 对应的 bufmap_locks[key] 锁。 （理由：1.只有 eviction 会改变某个桶的链表结构，而 eviction 本身也会尝试获取该锁 bufmap_locks[key]，所以只要占有该锁，涉及该桶的 eviction 就不会进行，也就代表该桶链表结构不会被改变；2.所有修改某个节点 refcnt 的操作都会先获取其对应的桶锁 bufmap_locks[key]，所以只要占有该锁，桶内所有节点的 refcnt 就不会改变。）
- 拿着 eviction_lock 的时候，代表不会有其他线程可以进行驱逐操作。由于只有 eviction 可以改变桶的链表结构，拿着该锁，也就意味着**整个哈希表**中的**所有桶**的**链表结构**都不会被改变，但**不保证链表内节点的refcnt不会改变**。也就是说，拿着 eviction_lock 的时候，refcnt 依然可能会因为多线程导致不一致，但是可以保证拿着锁的整个过程中，每个桶的链表节点数量不会增加、减少，也不会改变顺序。所以拿着 eviction_lock 的时候，可以安全遍历每个桶的每个节点，但是**不能访问 refcnt**。如果遍历的时候需要访问某个 buf 的 refcnt，则需要另外再拿其所在桶的 bufmap_locks[key] 锁。

更简短地讲：

- bufmap_locks 保护单个桶的链表结构，以及桶内所有节点的 refcnt
- eviction_lock 保护所有桶的链表结构，但是不保护任何 refcnt

驱逐过程中，首先需要拿 eviction_lock，使得可以遍历所有桶的链表结构。然后遍历链表结构寻找可驱逐块的时候，由于在某个桶i中判断是否有可驱逐块的过程需要读取 refcnt，所以需要再拿该桶的 bufmap_locks[i]。

Tricky的地方就是，bget 方法一开始判断块是否在缓存中时也获取了一个桶的 bufmap_locks[key]，此时如果遍历获取所有桶的 bufmap_locks[i] 的话，很容易引起环路等待而触发死锁。若在判断是否存在后立刻释放掉 bufmap_locks[key] 再拿 eviction_lock 的话，又会导致在释放桶锁和拿 eviction_lock 这两个操作中间的微小间隙，其他线程可能会对同一个块号进行 bget 访问，导致最终同一个块被插入两次。

博客后半部分都是在讲我是如何（尝试）解决这一问题的。

最终方案是在释放 bufmap_locks[key]，获取 eviction_lock 之后，再判断一次目标块号是否已经插入。这意味着依然会出现 bget 尝试对同一个 blockno 进行驱逐并插入两次的情况，但是能够保证除了第一个驱逐+插入的尝试能成功外，后续的尝试都不会导致重复驱逐+重复插入，而是能正确返回第一个成功的驱逐+插入产生的结果。也就是**允许竞态条件的发生，但是对其进行检测**，可以理解为一种乐观锁 optimistic locking。
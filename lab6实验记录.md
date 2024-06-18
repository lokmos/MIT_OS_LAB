# Task1

## 要求

xv6中的`fork()`系统调用将父进程的所有用户空间内存复制到子进程中。如果父进程较大，则复制可能需要很长时间。更糟糕的是，这项工作经常造成大量浪费；例如，子进程中的`fork()`后跟`exec()`将导致子进程丢弃复制的内存，而其中的大部分可能都从未使用过。另一方面，如果父子进程都使用一个页面，并且其中一个或两个对该页面有写操作，则确实需要复制。

copy-on-write (COW) fork()的目标是推迟到子进程实际需要物理内存拷贝时再进行分配和复制物理内存页面。

COW fork()只为子进程创建一个页表，用户内存的PTE指向父进程的物理页。COW fork()将父进程和子进程中的所有用户PTE标记为不可写。当任一进程试图写入其中一个COW页时，CPU将强制产生页面错误。内核页面错误处理程序检测到这种情况将为出错进程分配一页物理内存，将原始页复制到新页中，并修改出错进程中的相关PTE指向新的页面，将PTE标记为可写。当页面错误处理程序返回时，用户进程将能够写入其页面副本。

COW fork()将使得释放用户内存的物理页面变得更加棘手。给定的物理页可能会被多个进程的页表引用，并且只有在最后一个引用消失时才应该被释放。

## 提示

**这是一个合理的攻克计划：**

1. 修改`uvmcopy()`将父进程的物理页映射到子进程，而不是分配新页。在子进程和父进程的PTE中清除`PTE_W`标志。
2. 修改`usertrap()`以识别页面错误。当COW页面出现页面错误时，使用`kalloc()`分配一个新页面，并将旧页面复制到新页面，然后将新页面添加到PTE中并设置`PTE_W`。
3. 确保每个物理页在最后一个PTE对它的引用撤销时被释放——而不是在此之前。这样做的一个好方法是为每个物理页保留引用该页面的用户页表数的“引用计数”。当`kalloc()`分配页时，将页的引用计数设置为1。当`fork`导致子进程共享页面时，增加页的引用计数；每当任何进程从其页表中删除页面时，减少页的引用计数。`kfree()`只应在引用计数为零时将页面放回空闲列表。可以将这些计数保存在一个固定大小的整型数组中。你必须制定一个如何索引数组以及如何选择数组大小的方案。例如，您可以用页的物理地址除以4096对数组进行索引，并为数组提供等同于***kalloc.c***中`kinit()`在空闲列表中放置的所有页面的最高物理地址的元素数。
4. 修改`copyout()`在遇到COW页面时使用与页面错误相同的方案。

**提示：**

- lazy page allocation实验可能已经让您熟悉了许多与copy-on-write相关的xv6内核代码。但是，您不应该将这个实验室建立在您的lazy allocation解决方案的基础上；相反，请按照上面的说明从一个新的xv6开始。
- 有一种可能很有用的方法来记录每个PTE是否是COW映射。您可以使用RISC-V PTE中的RSW（reserved for software，即为软件保留的）位来实现此目的。
- `usertests`检查`cowtest`不测试的场景，所以别忘两个测试都需要完全通过。
- ***kernel/riscv.h***的末尾有一些有用的宏和页表标志位的定义。
- 如果出现COW页面错误并且没有可用内存，则应终止进程。

## 实现

这个实验的提示给得比较混乱，因此重新整理

首先，copy on write 策略要求在 fork 时只映射但不分配内存，同时将父子两端的页表都设置为不可写

- 修改 `uvmcopy`，这是原本 fork 实现中复制内存的函数

  - 需要做几件事

    - 删掉分配内存的代码

    - 修改父子进程页表的权限（不可写，同时要增加一个表示COW的标志位）

      - 页表flags中的8-10位都被保留用于自定义用途

      - ```c
        // kernel/riscv.h
        #define PTE_V (1L << 0) // valid
        #define PTE_R (1L << 1)
        #define PTE_W (1L << 2)
        #define PTE_X (1L << 3)
        #define PTE_U (1L << 4) // 1 -> user can access
        #define PTE_COW (1L << 8) // 是否为懒复制页，使用页表项 flags 中保留的第 8 位表示
        // （页表项 flags 中，第 8、9、10 位均为保留给操作系统使用的位，可以用作任意自定义用途）
        ```

    - 增加页面的引用计数，表示该物理页被引用

      - 定义一个函数，稍后在 kalloc 部分会解释

  - ```c
    int
    uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
    {
      pte_t *pte;
      uint64 pa, i;
      uint flags;
      // char *mem;
    
      for(i = 0; i < sz; i += PGSIZE){
        if((pte = walk(old, i, 0)) == 0)
          panic("uvmcopy: pte should exist");
        if((*pte & PTE_V) == 0)
          panic("uvmcopy: page not present");
        pa = PTE2PA(*pte);
        // 不可写，设置COW标志
        if (*pte & PTE_W) {
          *pte &= ~PTE_W;
          *pte |= PTE_COW;
        }
        // 传给子进程的flag和父进程的相同
        flags = PTE_FLAGS(*pte);
        
        // if((mem = kalloc()) == 0)
        //   goto err;
        // memmove(mem, (char*)pa, PGSIZE);
        if(mappages(new, i, PGSIZE, pa, flags) != 0){
          // kfree(mem);
          goto err;
        }
        // 增加页面引用计数
        krefpage((void *)pa);
      }
      return 0;
    
     err:
      uvmunmap(new, 0, i / PGSIZE, 1);
      return -1;
    }
    ```

修改 `usertrap`，和lab5一样，也是针对 pagefault 和 COW 同时检测

- 为了模块化，把检测是否为 COW 的部分和分配内存的部分写成两个函数

  - ```c
    
    uint
    checkCow(uint64 va) {
      struct proc* p = myproc();
      pte_t* pte;
       return va < p->sz // 在进程内存范围内
        && ((pte = walk(p->pagetable, va, 0))!=0)
        && (*pte & PTE_V) // 页表项存在
        && (*pte & PTE_COW); // 页是一个懒复制页
    }
    
    int
    copyOnWrite(uint64 va) {
      pte_t *pte;
      struct proc *p = myproc();
      if ((pte = walk(p->pagetable, va, 0)) == 0)
        panic("copyOnWrite: pte should exist");
      
      uint64 pa = PTE2PA(*pte);
      // 定义在kalloc中的新函数，负责为虚拟地址分配新的页
      uint64 new = (uint64)kcopy_n_deref((void *)pa);
      if (new == 0) {
        return -1;
      }
      // 取消页的COW，恢复权限可写
      uint64 flags = (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW;
      // 取消原来映射，重新建立新映射
      uvmunmap(p->pagetable, PGROUNDDOWN(va), 1, 0);
      if(mappages(p->pagetable, va, 1, new, flags) == -1) {
        panic("copyOnWrite: mappages");
      }
      return 0;
    }
    ```

- 然后修改 `usertrap`

  - ```C
    if(r_scause() == 8){
        // system call
    
        if(p->killed)
          exit(-1);
    
        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        p->trapframe->epc += 4;
    
        // an interrupt will change sstatus &c registers,
        // so don't enable until done with those registers.
        intr_on();
    
        syscall();
      } 
    // 判断页故障
    else if (r_scause() == 13 || r_scause() == 15) {
        // 获取虚拟地址
        uint64 va = r_stval();
        // 如果该地址对应的页是一个COW页，则为其分配内存
        if (checkCow(va)) {
          if (copyOnWrite(va) < 0) {
            p->killed = 1;
          }
        } else {
          p->killed = 1;
        }
      }
    ```

修改 copyout

- copyout 是软件访问页表，其不会陷入缺页异常，所以不能依靠 usertrap 来捕获，需要手动检测

  - ```c
    int
    copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
    {
      uint64 n, va0, pa0;
    
      while(len > 0){
        // 对每个页检查其是否为COW页
        if (checkCow(dstva)) {
          copyOnWrite(dstva);
        }
        // ...
    }
    
    ```

接下来，要完成页的生命周期管理，也就是实现之前的修改中使用到的函数

- 分配物理页的时候，引用计数为1
- 释放物理页时，引用数-1；只有引用数为0了，才可以真正释放回收物理页

实现的几个函数为：

- kalloc：分配物理页，将引用计数置为1
- krefpage：创建物理页的一个新引用，引用计数加1
- kcopy_n_deref: 将物理页的一个引用实复制到一个新物理页上（引用计数为 1），返回得到的副本页；并将本物理页的引用计数减 1
- kfree: 释放物理页的一个引用，引用计数减 1；如果计数变为 0，则释放回收物理页

具体调用的流程为：

1. 一个物理页 p 首先会被父进程使用 kalloc() 创建
2. fork 的时候，新创建的子进程会使用 krefpage() 声明自己对父进程物理页的引用
3. 当尝试修改父进程或子进程中的页时，kcopy_n_deref() 负责将想要修改的页实复制到独立的副本，并记录解除旧的物理页的引用（引用计数减 1）
4. 最后 kfree() 保证只有在所有的引用者都释放该物理页的引用时，才释放回收该物理页

按照提示，要定义一个数组来管理物理页和它们的引用计数

- 可以使用的范围为 `PHYSTOP-KERNBASE` 中间的这一段

- 还要定义一个用于该数组的锁，防止竞态条件引起内存泄漏

  - 即对引用计数的修改要互斥
  - 一定要加锁，否则usertests无法通过

- ```c
  // 定义地址和索引的转换
  #define PA2PGREF_ID(p) (((p - KERNBASE) / PGSIZE))
  // 数组最大的个数
  #define MAX_PA2PGREF PA2PGREF_ID(PHYSTOP)
  // 锁
  struct spinlock pgreflock;
  // 页引用数组
  int pageref[MAX_PA2PGREF];

随后修改和完成 kalloc 中涉及到的函数

- kinit 需要初始化一个新的锁

  - ```c
    void
    kinit()
    {
      initlock(&kmem.lock, "kmem");
      initlock(&pgreflock, "pgref");
      freerange(end, (void*)PHYSTOP);
    }
    ```

- kfree 减少引用次数，并在其为0时释放页

  - 注意加锁

  - ```c
    void
    kfree(void *pa)
    {
      struct run *r;
    
      if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");
      
      acquire(&pgreflock);
    
      if(--pageref[PA2PGREF_ID((uint64)pa)] <= 0) {
        // 当页面的引用计数小于等于 0 的时候，释放页面
    
        // Fill with junk to catch dangling refs.
        // pa will be memset multiple times if race-condition occurred.
        memset(pa, 1, PGSIZE);
    
        r = (struct run*)pa;
    
        acquire(&kmem.lock);
        r->next = kmem.freelist;
        kmem.freelist = r;
        release(&kmem.lock);
      }
      release(&pgreflock);
    }
    ```

- kalloc 给新分配的物理页添加引用计数1

  - ```c
    void *
    kalloc(void)
    {
      struct run *r;
    
      acquire(&kmem.lock);
      r = kmem.freelist;
      if(r)
        kmem.freelist = r->next;
      release(&kmem.lock);
    
      if(r) {
        memset((char*)r, 5, PGSIZE); // fill with junk
        pageref[PA2PGREF_ID((uint64)r)] = 1;
      }
      return (void*)r;
    }
    ```

  - kalloc() 可以不用加锁，因为 kmem 的锁已经保证了同一个物理页不会同时被两个进程分配，并且在 kalloc() 返回前，其他操作 pageref() 的函数也不会被调用，因为没有任何其他进程能够在 kalloc() 返回前得到这个新页的地址。

- kcopy_n_deref 创建和复制内存到新的物理页

  - 如果引用小于等于1，说明只有当前的物理页引用了该页，此时不用分配，直接返回该页即可

  - ```c
    void *
    kcopy_n_deref(void *pa) {
      acquire(&pgreflock);
      // 如果引用数小于等于1，不用复制
      if (pageref[PA2PGREF_ID((uint64)pa)] <= 1) {
        release(&pgreflock);
        return pa;
      }
      uint64 newa = (uint64)kalloc();
      if (newa == 0) {
        release(&pgreflock);
        return 0;
      }
      memmove((void *)newa, pa, PGSIZE);
      // 减少旧页面的引用
      pageref[PA2PGREF_ID((uint64)pa)]--;
      release(&pgreflock);
      return (void *)newa;
    }
    ```

- krefpage 增加特定页面的引用

  - ```c
    void
    krefpage(void *pa) {
      acquire(&pgreflock);
      pageref[PA2PGREF_ID((uint64)pa)]++;
      release(&pgreflock);
    }
    ```

# 结果

![image-20240618201708853](assets/image-20240618201708853.png)

# 补充

不加锁导致的竞态条件：

举一个很常见的 fork() 后 exec() 的例子：

```
父进程: 分配物理页 p（p 引用计数 = 1）
父进程: fork()（p 引用计数 = 2）
父进程: 尝试修改 p，触发页异常
父进程: 由于 p 引用计数大于 1，开始实复制 p（p 引用计数 = 2）
--- 调度器切换到子进程
子进程: exec() 替换进程影像，释放所有旧的页
子进程: 尝试释放 p（引用计数减 1），子进程丢弃对 p 的引用（p 引用计数 = 1）
--- 调度器切换到父进程
父进程: （继续实复制p）创建新页 q，将 p 复制到 q，将 q 标记为可写并建立映射，在这过程中父进程丢弃对旧 p 的引用
```

在这一个执行流过后，最终结果是物理页 p 并没有被释放回收，然而父进程和子进程都已经丢弃了对 p 的引用（页表中均没有指向 p 的页表项），这样一来 p 占用的内存就属于泄漏内存了，永远无法被回收。

加了锁之后，保证了这种情况不会出现。

 `kcopy_n_deref()`实现的逻辑是，如果一个页的引用大于1，则复制该页并将引用数量减1（背后含义其实是把一个对该页的只读引用变为一份可读写的复制，然后交给请求者，归请求者所有），如果一个页的引用等于1，则省略复制步骤，直接将其本身作为结果返回（因为只有一个引用，相当于直接把这个副本的所有权交给请求者）。

这个例子的问题在于`kcopy_n_deref()`中，「检测PGREF是否大于一」和「PGREF自减1」这两件事情不是原子性的。导致了父进程的`kcopy_n_deref()`进入了引用数>1才会进入的复制流程，而子进程的kfree也同时（错误地）认为引用数在释放后应该还剩下1，导致父进程多复制了一份页并丢掉了对原来页的引用，而子进程kfree也丢掉了对原来页的引用，导致原始的内存页没有任何进程有对其的引用，成为泄漏内存。

这个实际上是比较经典的并行程序修改共享数据（PGREF）的同步问题。由于`kcopy_n_deref()`对PGREF分别的检测和修改并不是一步完成的，如果有另一个进程在「检测」和「修改」的间隙对该内存进行访问/修改（如kfree检查该PGREF值），则会出现数据竞争，导致进入异常状态（在这里后果是一个内存泄漏）。

解决方法就是加锁，保护这两个操作，使得在外界看起来这两个操作是原子的，即不能在中间插入任何其他操作的。
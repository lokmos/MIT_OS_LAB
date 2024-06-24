# 实验

## Task1

### 要求

`mmap`和`munmap`系统调用允许UNIX程序对其地址空间进行详细控制。它们可用于在进程之间共享内存，将文件映射到进程地址空间，并作为用户级页面错误方案的一部分，如本课程中讨论的垃圾收集算法。在本实验室中，您将把`mmap`和`munmap`添加到xv6中，重点关注内存映射文件（memory-mapped files）。

手册页面（运行`man 2 mmap`）显示了`mmap`的以下声明：

```c
void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset);
```

可以通过多种方式调用`mmap`，但本实验只需要与内存映射文件相关的功能子集。您可以假设`addr`始终为零，这意味着内核应该决定映射文件的虚拟地址。`mmap`返回该地址，如果失败则返回`0xffffffffffffffff`。`length`是要映射的字节数；它可能与文件的长度不同。`prot`指示内存是否应映射为可读、可写，以及/或者可执行的；您可以认为`prot`是`PROT_READ`或`PROT_WRITE`或两者兼有。`flags`要么是`MAP_SHARED`（映射内存的修改应写回文件），要么是`MAP_PRIVATE`（映射内存的修改不应写回文件）。您不必在`flags`中实现任何其他位。`fd`是要映射的文件的打开文件描述符。可以假定`offset`为零（它是要映射的文件的起点）。

允许进程映射同一个`MAP_SHARED`文件而不共享物理页面。

`munmap(addr, length)`应删除指定地址范围内的`mmap`映射。如果进程修改了内存并将其映射为`MAP_SHARED`，则应首先将修改写入文件。`munmap`调用可能只覆盖`mmap`区域的一部分，但您可以认为它取消映射的位置**要么在区域起始位置，要么在区域结束位置，要么就是整个区域(但不会在区域中间“打洞”)**。

您应该实现足够的`mmap`和`munmap`功能，以使`mmaptest`测试程序正常工作。如果`mmaptest`不会用到某个`mmap`的特性，则不需要实现该特性。



### 提示

- 首先，向`UPROGS`添加`_mmaptest`，以及`mmap`和`munmap`系统调用，以便让***user/mmaptest.c***进行编译。现在，只需从`mmap`和`munmap`返回错误。我们在***kernel/fcntl.h***中为您定义了`PROT_READ`等。运行`mmaptest`，它将在第一次`mmap`调用时失败。
- 惰性地填写页表，以响应页错误。也就是说，`mmap`不应该分配物理内存或读取文件。相反，在`usertrap`中（或由`usertrap`调用）的页面错误处理代码中执行此操作，就像在lazy page allocation实验中一样。惰性分配的原因是确保大文件的`mmap`是快速的，并且比物理内存大的文件的`mmap`是可能的。
- 跟踪`mmap`为每个进程映射的内容。定义与第15课中描述的VMA（虚拟内存区域）对应的结构体，记录`mmap`创建的虚拟内存范围的地址、长度、权限、文件等。由于xv6内核中没有内存分配器，因此可以声明一个固定大小的VMA数组，并根据需要从该数组进行分配。大小为16应该就足够了。
- 实现`mmap`：在进程的地址空间中找到一个未使用的区域来映射文件，并将VMA添加到进程的映射区域表中。VMA应该包含指向映射文件对应`struct file`的指针；`mmap`应该增加文件的引用计数，以便在文件关闭时结构体不会消失（提示：请参阅`filedup`）。运行`mmaptest`：第一次`mmap`应该成功，但是第一次访问被`mmap`的内存将导致页面错误并终止`mmaptest`。
- 添加代码以导致在`mmap`的区域中产生页面错误，从而分配一页物理内存，将4096字节的相关文件读入该页面，并将其映射到用户地址空间。使用`readi`读取文件，它接受一个偏移量参数，在该偏移处读取文件（但必须lock/unlock传递给`readi`的索引结点）。不要忘记在页面上正确设置权限。运行`mmaptest`；它应该到达第一个`munmap`。
- 实现`munmap`：找到地址范围的VMA并取消映射指定页面（提示：使用`uvmunmap`）。如果`munmap`删除了先前`mmap`的所有页面，它应该减少相应`struct file`的引用计数。如果未映射的页面已被修改，并且文件已映射到`MAP_SHARED`，请将页面写回该文件。查看`filewrite`以获得灵感。
- 理想情况下，您的实现将只写回程序实际修改的`MAP_SHARED`页面。RISC-V PTE中的脏位（`D`）表示是否已写入页面。但是，`mmaptest`不检查非脏页是否没有回写；因此，您可以不用看`D`位就写回页面。
- 修改`exit`将进程的已映射区域取消映射，就像调用了`munmap`一样。运行`mmaptest`；`mmap_test`应该通过，但可能不会通过`fork_test`。
- 修改`fork`以确保子对象具有与父对象相同的映射区域。不要忘记增加VMA的`struct file`的引用计数。在子进程的页面错误处理程序中，可以分配新的物理页面，而不是与父级共享页面。后者会更酷，但需要更多的实施工作。运行`mmaptest`；它应该通过`mmap_test`和`fork_test`。



### 实现

首先，定义 VMA 结构

- 要在用户的地址空间中找一片空闲区域映射mmap页

  - ![User Address Space](assets/mit6s081-lab10-useraddrspace.png)

  - 用户地址空间如图，唯一可以使用的是heap

    - heap从低地址向高地址增长
    - VMA应该从高地址向低地址增长以避免冲突

  - ```c
    // kernel/memlayout.h
    #define MMAPEND TRAPFRAME
    ```

- 接着定义结构体，按照提示，需要起始地址、大小、权限、标记、偏移，以及一个指向文件的指针和标记该vma是否有效的字段

  - ```c
    struct vma {
      uint64 addr;
      uint64 len;
      int prot;
      int flags;
      int fd;
      uint64 offset;
      int valid;
      struct file *f;
    };
    ```

  - 定义了vma之后，在进程结构 proc 中要加入一个大小为16的vma空槽

  - ```c
    #define NVMA 16
    
    // Per-process state
    struct proc {
      struct spinlock lock;
    
      // p->lock must be held when using these:
      enum procstate state;        // Process state
      struct proc *parent;         // Parent process
      void *chan;                  // If non-zero, sleeping on chan
      int killed;                  // If non-zero, have been killed
      int xstate;                  // Exit status to be returned to parent's wait
      int pid;                     // Process ID
    
      // these are private to the process, so p->lock need not be held.
      uint64 kstack;               // Virtual address of kernel stack
      uint64 sz;                   // Size of process memory (bytes)
      pagetable_t pagetable;       // User page table
      struct trapframe *trapframe; // data page for trampoline.S
      struct context context;      // swtch() here to run process
      struct file *ofile[NOFILE];  // Open files
      struct inode *cwd;           // Current directory
      char name[16];               // Process name (debugging)
      struct vma vmas[NVMA];
    };
    ```

完成 `sys_mmap`

- 函数原型为

  - ```c
    void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
    ```

- 函数在16个空槽中一次寻找，如果已经是有效的vma，则略过，并记录其起始地址（作为最后要添加的vma的结尾）；如果找到一个无效的vma，则可以分配，然后退出循环；最后使用 `filedup` 增加文件计数

  - 这里要注意文件的权限，如果尝试将一个只读打开的文件映射为可写，并且开启了回盘（MAP_SHARED），则 mmap 应该失败。否则回盘的时候会出现回盘到一个只读文件的错误情况

  - ```c
    uint64
    sys_mmap(void) {
      uint64 addr;
      uint64 len;
      int prot;
      int flags;
      int fd;
      uint64 offset;
      struct file *f;
      uint64 vend = MMAPEND;
    
      if (argaddr(0, &addr) < 0 || argaddr(1, &len) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 || argfd(4, &fd, &f) < 0 || argaddr(5, &offset) < 0) {
        return -1;
      }
      // 判断是否满足提示中的默认地址为0，偏移为0
      if (addr != 0 || offset != 0)
        return -1;
      // 判断文件权限和要分配的vma的权限是否匹配，一定要加，不然 mmaptest 会失败
      if((!f->readable && (prot & (PROT_READ)))
         || (!f->writable && (prot & PROT_WRITE) && !(flags & MAP_PRIVATE)))
        return -1;
    
      for (int i = 0; i < NVMA; i++) {
        if (myproc()->vmas[i].valid == 0) {
          // addr是起始位置，vend是结束位置
          myproc()->vmas[i].addr = vend - len;
          myproc()->vmas[i].len = len;
          myproc()->vmas[i].prot = prot;
          myproc()->vmas[i].flags = flags;
          myproc()->vmas[i].fd = fd;
          myproc()->vmas[i].offset = offset;
          myproc()->vmas[i].valid = 1;
          myproc()->vmas[i].f = f;
          filedup(f);
          myproc()->sz += len;
          return vend - len;
        }
        else 
          vend = PGROUNDDOWN(myproc()->vmas[i].addr);
      }
      return -1;
    }
    ```

修改 `usertrap` 实现懒分配

- 和 lab5 类似，这里为了方便，也采用模块化设计

  - 函数 `find` 在 进程 `p` 中寻找包含了缺页地址的那个vma

  - 函数 `handleVma` 在寻找到的vma中处理缺页，为其分配物理页，然后读取文件，获取权限，并映射虚拟地址

  - ```c
    struct vma *find(uint64 va, struct proc *p){
      struct vma *vv;
      for (vv = p->vmas; vv < &p->vmas[NVMA]; vv++){
        if(vv->valid && va >= vv->addr && va < vv->addr + vv->len){
          return vv;
        }
      }
      return 0;
    }
    
    int handleVma(uint64 va, struct proc *p) {
      struct vma *vv;
      vv = find(va, p);
      if(vv == 0){
        return 0;
      }
    
       // allocate physical page
      void *pa = kalloc();
      if(pa == 0) {
        panic("handleVma: kalloc");
      }
      memset(pa, 0, PGSIZE);
      
      // read data from disk
      begin_op();
      ilock(vv->f->ip);
      readi(vv->f->ip, 0, (uint64)pa, vv->offset + PGROUNDDOWN(va - vv->addr), PGSIZE);
      iunlock(vv->f->ip);
      end_op();
    
      // set appropriate perms, then map it.
      int perm = PTE_U;
      if(vv->prot & PROT_READ)
        perm |= PTE_R;
      if(vv->prot & PROT_WRITE)
        perm |= PTE_W;
      if(vv->prot & PROT_EXEC)
        perm |= PTE_X;
    
      if(mappages(p->pagetable, va, PGSIZE, (uint64)pa, PTE_R | PTE_W | PTE_U) < 0) {
        panic("handleVma: mappages");
      }
    
      return 1;
    }
    ```

- 在 usertrap中使用

  - ```c
    else if (r_scause() == 13 || r_scause() == 15) {
        // page fault
        uint64 va = r_stval();
        if (handleVma(va, p) != 1) {
          printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
          printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
          p->killed = 1;
        }
      } 

- 因为实现了懒分配，所以在 `uvmuncopy` 和 `uvmunmap`中，不用在检查页是否有效 (PTE_V)

  - ```c
    if((*pte & PTE_V) == 0)
      continue;
    ```

实现 `sys_munmap` 

- 这里首先通过传入的地址找到对应的 vma 结构体（通过前面定义的 findvma 方法），然后检测了一下在 vma 区域中间“挖洞”释放的错误情况，计算出应该开始释放的内存地址以及应该释放的内存字节数量（由于页有可能不是完整释放，如果 addr 处于一个页的中间，则那个页的后半部分释放，但是前半部分不释放，此时该页整体不应该被释放）。

  计算出来释放内存页的开始地址以及释放的个数后，调用自定义的 vmaunmap 方法（vm.c）对物理内存页进行释放，并在需要的时候将数据写回磁盘。将该方法独立出来并写到 vm.c 中的理由是方便调用 vm.c 中的 walk 方法。

  在调用 vmaunmap 释放内存页之后，对 v->offset、v->addr 以及 v->len 作相应的修改，并在所有页释放完毕之后，关闭对文件的引用，并完全释放该 vma。

- ```c
  uint64
  sys_munmap(void)
  {
    uint64 addr, sz;
  
    if(argaddr(0, &addr) < 0 || argaddr(1, &sz) < 0 || sz == 0)
      return -1;
  
    struct proc *p = myproc();
  
    struct vma *v = find(addr, p);
    if(v == 0) {
      return -1;
    }
  
    if(addr > v->addr && addr + sz < v->addr + v->len) {
      // trying to "dig a hole" inside the memory range.
      return -1;
    }
  
    uint64 addr_aligned = addr;
    if(addr > v->addr) {
      addr_aligned = PGROUNDUP(addr);
    }
  
    int nunmap = sz - (addr_aligned-addr); // nbytes to unmap
    if(nunmap < 0)
      nunmap = 0;
    
    vmaunmap(p->pagetable, addr_aligned, nunmap, v); // custom memory page unmap routine for mmapped pages.
  
    if(addr <= v->addr && addr + sz > v->addr) { // unmap at the beginning
      v->offset += addr + sz - v->addr;
      v->addr = addr + sz;
    }
    v->len -= sz;
  
    if(v->len <= 0) {
      fileclose(v->f);
      v->valid = 0;
    }
  
    return 0;  
  }
  ```

- `vmaunmap` 函数如下

  - 实现大致上和 uvmunmap 相似，查找范围内的每一个页，检测其 dirty bit (D) 是否被设置，如果被设置，则代表该页被修改过，需要将其写回磁盘。注意不是每一个页都需要完整的写回，这里需要处理开头页不完整、结尾页不完整以及中间完整页的情况。

    xv6中本身不带有 dirty bit 的宏定义，在 riscv.h 中手动补齐：

    - ```c
      // kernel/riscv.h
      
      #define PTE_V (1L << 0) // valid
      #define PTE_R (1L << 1)
      #define PTE_W (1L << 2)
      #define PTE_X (1L << 3)
      #define PTE_U (1L << 4) // 1 -> user can access
      #define PTE_G (1L << 5) // global mapping
      #define PTE_A (1L << 6) // accessed
      #define PTE_D (1L << 7) // dirty
      ```

  - ```c
    // kernel/vm.c
    #include "fcntl.h"
    #include "spinlock.h"
    #include "sleeplock.h"
    #include "file.h"
    #include "proc.h"
    
    // Remove n BYTES (not pages) of vma mappings starting from va. va must be
    // page-aligned. The mappings NEED NOT exist.
    // Also free the physical memory and write back vma data to disk if necessary.
    void
    vmaunmap(pagetable_t pagetable, uint64 va, uint64 nbytes, struct vma *v)
    {
      uint64 a;
      pte_t *pte;
    
      // printf("unmapping %d bytes from %p\n",nbytes, va);
    
      // borrowed from "uvmunmap"
      for(a = va; a < va + nbytes; a += PGSIZE){
        if((pte = walk(pagetable, a, 0)) == 0)
          continue;
        if(PTE_FLAGS(*pte) == PTE_V)
          panic("sys_munmap: not a leaf");
        if(*pte & PTE_V){
          uint64 pa = PTE2PA(*pte);
          if((*pte & PTE_D) && (v->flags & MAP_SHARED)) { // dirty, need to write back to disk
            begin_op();
            ilock(v->f->ip);
            uint64 aoff = a - v->vastart; // offset relative to the start of memory range
            if(aoff < 0) { // if the first page is not a full 4k page
              writei(v->f->ip, 0, pa + (-aoff), v->offset, PGSIZE + aoff);
            } else if(aoff + PGSIZE > v->sz){  // if the last page is not a full 4k page
              writei(v->f->ip, 0, pa, v->offset + aoff, v->sz - aoff);
            } else { // full 4k pages
              writei(v->f->ip, 0, pa, v->offset + aoff, PGSIZE);
            }
            iunlock(v->f->ip);
            end_op();
          }
          kfree((void*)pa);
          *pte = 0;
        }
      }
    }
    ```

最后需要做的，是在 proc.c 中添加处理进程 vma 的各部分代码。

- 让 allocproc 初始化进程的时候，将 vma 槽都清空
- freeproc 释放进程时，调用 vmaunmap 将所有 vma 的内存都释放，并在需要的时候写回磁盘
- fork 时，拷贝父进程的所有 vma，但是不拷贝物理页
  - 注意，一定要要用memmove而不是=，原因在之前的实验中说了，前者是拷贝内容，后者仅仅是将指针指向了同一个地址

```c
// kernel/proc.c

static struct proc*
allocproc(void)
{
  // ......

  // Clear VMAs
  for(int i=0;i<NVMA;i++) {
    p->vmas[i].valid = 0;
  }

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  for(int i = 0; i < NVMA; i++) {
    struct vma *v = &p->vmas[i];
    vmaunmap(p->pagetable, v->vastart, v->sz, v);
  }
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  // ......

  // copy vmas created by mmap.
  // actual memory page as well as pte will not be copied over.
  for(i = 0; i < NVMA; i++) {
    struct vma *v = &p->vmas[i];
    if(v->valid) {
      memmove(&np->vmas[i], v, sizeof(*v));
      filedup(v->f);
    }
  }

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}
```



# 结果

![image-20240622172313355](assets/image-20240622172313355.png)
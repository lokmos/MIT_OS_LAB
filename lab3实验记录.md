# Chapter3：页表

## 分页硬件

RISC-V页表硬件通过将每个虚拟地址映射到物理地址来为这两种地址建立联系。

XV6基于Sv39 RISC-V运行，这意味着它只使用64位虚拟地址的**低39位**

- RISC-V页表在逻辑上是一个由 $2^{27}$​ 个页表条目（Page Table Entries/PTE）组成的数组
- 每个PTE包含一个44位的物理页码（Physical Page Number/PPN）和一些标志
- 分页硬件通过使用虚拟地址39位中的前27位索引页表，以找到该虚拟地址对应的一个PTE，然后生成一个56位的物理地址，其前44位来自PTE中的PPN，其后12位来自原始虚拟地址。
- ![img](assets/p1.png)

- 页表使操作系统能够以 4096 （offset=2^12）字节的对齐块的粒度控制虚拟地址和物理地址的转换，这样的块成为 **页**

**页的转换**

页表以三级的树型结构存储在物理内存中，每个PTE是8个字节

![img](assets/p2.png)

- 如果转换地址所需的三个PTE中任何一个不存在，页式硬件会引发 **页面故障异常**，并让内核处理

三级页表更加节省内存，在大范围的虚拟地址没有被映射的情况下，三级结构可以忽略整个页面目录

- 缺点是转换地址的时候，必须从内存中加载三个PTE。为了减少开销，使用TLB

每个PTE包含标志位，告知硬件允许如何使用关联的虚拟地址

- `PTE_V`指示PTE是否存在：如果它没有被设置，对页面的引用会导致异常（即不允许）
- `PTE_R`控制是否允许指令读取到页面
- `PTE_W`控制是否允许指令写入到页面
- `PTE_X`控制CPU是否可以将页面内容解释为指令并执行它们
- `PTE_U`控制用户模式下的指令是否被允许访问页面；如果没有设置`PTE_U`，PTE只能在管理模式下使用

为了告诉硬件使用页表，内核必须将根页表页的物理地址写入到`satp`寄存器中（`satp`的作用是存放根页表页在物理内存中的地址）

- 每个CPU都有自己的`satp`，一个CPU将使用自己的`satp`指向的页表转换后续指令生成的所有地址
- 每个CPU都有自己的`satp`，因此不同的CPU就可以运行不同的进程，每个进程都有自己的页表描述的私有地址空间



## 内核地址空间

Xv6为每个进程维护一个页表，用以描述每个进程的用户地址空间；外加一个单独描述内核地址空间的页表。

内核配置其地址空间的布局，以允许自己以可预测的虚拟地址访问物理内存和各种硬件资源

- ![img](assets/p3.png)

QEMU模拟了一台计算机，它包括从物理地址`0x80000000`开始并至少到`0x86400000`结束的RAM（物理内存），xv6称结束地址为`PHYSTOP`

QEMU将设备接口作为内存映射控制寄存器暴露给软件，这些寄存器位于物理地址空间`0x80000000`以下

- 内核使用“直接映射”获取内存和内存映射设备寄存器；也就是说，将资源映射到等于物理地址的虚拟地址。
- 直接映射简化了读取或写入物理内存的内核代码

有几个内核地址不是直接映射

- 蹦床页面(trampoline page)。它映射在虚拟地址空间的顶部；用户页表具有相同的映射。
- 内核栈页面。每个进程都有自己的内核栈，它将映射到偏高一些的地址，这样xv6在它之下就可以留下一个未映射的保护页(guard page)。
  - ***Guard page不会浪费物理内存，它只是占据了虚拟地址空间的一段靠后的地址，但并不映射到物理地址空间。***
  - 保护页的PTE是无效的（也就是说`PTE_V`没有设置），所以如果内核溢出内核栈就会引发一个异常，内核触发`panic`
  - 如果没有保护页，栈溢出将会覆盖其他内核内存，引发错误操作

内核在权限`PTE_R`和`PTE_X`下映射蹦床页面和内核文本页面

- 从这些页面读取和执行指令

内核在权限`PTE_R`和`PTE_W`下映射其他页面

- 可以读写那些页面中的内存

对于保护页面的映射是无效的



## 代码：创建一个地址空间

大多数用于操作地址空间和页表的xv6代码都写在 ***vm.c*** ([kernel/vm.c:1](https://github.com/mit-pdos/xv6-riscv/blob/riscv//kernel/vm.c#L1)) 中。

- 核心数据结构是`pagetable_t`，它实际上是指向RISC-V根页表页的指针
  - 一个`pagetable_t`可以是内核页表，也可以是一个进程页表
- 最核心的函数是`walk`和`mappages`，前者为虚拟地址找到PTE，后者为新映射装载PTE
  - `walk`(***kernel/vm.c***:72)模仿RISC-V分页硬件
  - `walk`一次从3级页表中获取9个比特位。它使用上一级的9位虚拟地址来查找下一级页表或最终页面的PTE
    - 如果PTE无效，则所需的页面还没有分配
    - 如果设置了`alloc`参数，`walk`就会分配一个新的页表页面，并将其物理地址放在PTE中
  - 返回树中最低一级的PTE地址 `return &pagetable[PX(0, va)];`
  - 上面的代码依赖于**直接映射**到内核虚拟地址空间中的物理内存
    - 当`walk`降低页表的级别时，它从PTE 中提取下一级页表的（物理）地址，然后使用该地址作为虚拟地址来获取下一级的PTE
- 名称以`kvm`开头的函数操作内核页表；以`uvm`开头的函数操作用户页表；其他函数用于二者
- `copyout`和`copyin`复制数据到用户虚拟地址或从用户虚拟地址复制数据
  - 这些虚拟地址作为系统调用参数提供; 由于它们需要显式地翻译这些地址，以便找到相应的物理内存，故将它们写在***vm.c***中

在启动序列的前期，`main` 调用 `kvminit` (***kernel/vm.c***:54) 以使用 `kvmmake` (***kernel/vm.c***:20) 创建内核的页表

- 此调用发生在 xv6 启用 RISC-V 上的分页之前，因此地址直接引用物理内存
- `kvmmake` 首先分配一个物理内存页来保存根页表页
- 然后它调用`kvmmap`来装载内核需要的转换
  - 转换包括内核的指令和数据、物理内存的上限到 `PHYSTOP`，并包括实际上是设备的内存
  - `kvmmap`(***kernel/vm.c***:127)调用`mappages`(***kernel/vm.c***:138)，
    - `mappages`将范围虚拟地址到同等范围物理地址的映射装载到一个页表中。
    - 以页面大小为间隔，为范围内的每个虚拟地址单独执行此操作
    - 对于要映射的每个虚拟地址，`mappages`调用`walk`来查找该地址的PTE地址
    - 初始化PTE以保存相关的物理页号、所需权限（`PTE_W`、`PTE_X`和/或`PTE_R`）以及用于标记PTE有效的`PTE_V`(***kernel/vm.c\***:153)
- `Proc_mapstacks` (***kernel/proc.c***:33) 为每个进程分配一个内核堆栈。
  - 它调用 kvmmap 将每个堆栈映射到由 KSTACK 生成的虚拟地址，从而为无效的堆栈保护页面留出空间

`main`调用`kvminithart` (***kernel/vm.c***:53)来安装内核页表。它将根页表页的物理地址写入寄存器`satp`。

`main`中调用的`procinit` (***kernel/proc.c***:26)为每个进程分配一个内核栈。它将每个栈映射到`KSTACK`生成的虚拟地址，这为无效的栈保护页面留下了空间。`kvmmap`将映射的PTE添加到内核页表中，对`kvminithart`的调用将内核页表重新加载到`satp`中，以便硬件知道新的PTE。



## 物理内存分配

xv6使用内核末尾到`PHYSTOP`之间的物理内存进行运行时分配。

- 一次分配和释放整个4096字节的页面
- 使用链表的数据结构将空闲页面记录下来
  - 分配时需要从链表中删除页面
  - 释放时需要将释放的页面添加到链表中



## 代码：物理内存分配

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
- 后`kfree`将页面前置（头插法）到空闲列表中：
  - 它将`pa`转换为一个指向`struct run`的指针`r`，在`r->next`中记录空闲列表的旧开始，并将空闲列表设置为等于`r`。

`kalloc`删除并返回**空闲列表**中的第一个元素。



## 进程地址空间

每个进程都有一个单独的页表，当xv6在进程之间切换时，也会更改页表。如图2.3所示，一个进程的用户内存从虚拟地址零开始，可以增长到MAXVA (***kernel/riscv.h***:348)，原则上允许一个进程内存寻址空间为256G

- ![img](assets/p5.png)



当进程向xv6请求更多的用户内存时，xv6首先使用`kalloc`来分配物理页面，然后，它将PTE添加到进程的页表中，指向新的物理页面

- 不同进程的页表将用户地址转换为物理内存的不同页面，这样每个进程都拥有私有内存
- 每个进程看到的自己的内存空间都是以0地址起始的连续虚拟地址，而进程的物理内存可以是非连续的
- 内核在用户地址空间的顶部映射一个带有蹦床（trampoline）代码的页面，这样在所有地址空间都可以看到一个单独的物理内存页面

**用户态内存布局**

![img](assets/p6.png)

- 栈是单独一个页面，显示的是由`exec`创建后的初始内容。包含命令行参数的字符串以及指向它们的指针数组位于栈的最顶部
- 再往下是允许程序在`main`处开始启动的值（即`main`的地址、`argc`、`argv`），这些值产生的效果就像刚刚调用了`main(argc, argv)`一样
- 为了检测用户栈是否溢出了所分配栈内存，xv6在栈正下方放置了一个无效的保护页（guard page）。如果用户栈溢出并且进程试图使用栈下方的地址，那么由于映射无效（`PTE_V`为0）硬件将生成一个页面故障异常



## 代码：sbrk

`sbrk`是一个用于进程减少或增长其内存的系统调用

这个系统调用由函数`growproc`实现(***kernel/proc.c***:239)

- `growproc`根据`n`是正的还是负的调用`uvmalloc`或`uvmdealloc`。
  - `uvmalloc`(***kernel/vm.c***:229)用`kalloc`分配物理内存，并用`mappages`将PTE添加到用户页表中。
  - `uvmdealloc`调用`uvmunmap`(***kernel/vm.c***:174)，`uvmunmap`使用`walk`来查找对应的PTE，并使用`kfree`来释放PTE引用的物理内存

XV6使用进程的页表，不仅是告诉硬件如何映射用户虚拟地址，也是明晰哪一个物理页面已经被分配给该进程的唯一记录。这就是为什么释放用户内存（在`uvmunmap`中）需要检查用户页表的原因。



# 实验

## Task1

### 要求

定义一个名为`vmprint()`的函数。它应当接收一个`pagetable_t`作为参数，并以下面描述的格式打印该页表。在`exec.c`中的`return argc`之前插入`if(p->pid==1) vmprint(p->pagetable)`，以打印第一个进程的页表。如果你通过了`pte printout`测试的`make grade`，你将获得此作业的满分。

现在，当您启动xv6时，它应该像这样打印输出来描述第一个进程刚刚完成`exec()`ing`init`时的页表：

```
page table 0x0000000087f6e000
..0: pte 0x0000000021fda801 pa 0x0000000087f6a000
.. ..0: pte 0x0000000021fda401 pa 0x0000000087f69000
.. .. ..0: pte 0x0000000021fdac1f pa 0x0000000087f6b000
.. .. ..1: pte 0x0000000021fda00f pa 0x0000000087f68000
.. .. ..2: pte 0x0000000021fd9c1f pa 0x0000000087f67000
..255: pte 0x0000000021fdb401 pa 0x0000000087f6d000
.. ..511: pte 0x0000000021fdb001 pa 0x0000000087f6c000
.. .. ..510: pte 0x0000000021fdd807 pa 0x0000000087f76000
.. .. ..511: pte 0x0000000020001c0b pa 0x0000000080007000
```

第一行显示`vmprint`的参数。之后的每行对应一个PTE，包含树中指向页表页的PTE。每个PTE行都有一些“`..`”的缩进表明它在树中的深度。每个PTE行显示其在页表页中的PTE索引、PTE比特位以及从PTE提取的物理地址。不要打印无效的PTE。在上面的示例中，顶级页表页具有条目0和255的映射。条目0的下一级只映射了索引0，该索引0的下一级映射了条目0、1和2。

您的代码可能会发出与上面显示的不同的物理地址。条目数和虚拟地址应相同。



### 提示

- 你可以将`vmprint()`放在***kernel/vm.c\***中
- 使用定义在***kernel/riscv.h\***末尾处的宏
- 函数`freewalk`可能会对你有所启发
- 将`vmprint`的原型定义在***kernel/defs.h\***中，这样你就可以在`exec.c`中调用它了
- 在你的`printf`调用中使用`%p`来打印像上面示例中的完成的64比特的十六进制PTE和地址



### 实现

首先先明确几个概念

- `pte` 对应的 `pagetable_t` 实际上就是一个 `uint64` 的数值，而不是一个结构，所以不存在说通过一个 `struct` 访问关于pte的一些信息；所有的信息都是靠64位中不同位数的数字来决定的；而控制位 `PTE_V` 等也只是将1左移的一些位数得到

  - ```c
    typedef uint64 pte_t;
    typedef uint64 *pagetable_t; // 512 PTEs
    ```

- 三级页表结构中，level 2是最上一层，指向level 1页表；level 1页表指向level 0页表；level 0是最低一层页表

- 通过 `pte & PTE_V` 来判断页表是否有效；

  - 0 则无效；1 则有效

- 通过 `pte & (PTE_R | PTE_W | PTE_X)` 来判断是否还有子页

  - 最后一级页表的 R、W、X中至少有一个为1
  - 0 则有子页；1 则无子页

- 通过 `PTE2PA` 函数转换虚拟地址为物理地址

实现 `vmprint` 

- 模仿 `freewalk` 的过程，通过递归调用下沉到低一级页表

- 写了一个辅助函数，用于控制开头 '..' 和 ' ' 的数量

- ```c
  void _vmprint(pagetable_t pagetable, int level) {
      // 每级页表有512个pte
      for (int i = 0; i < 512; ++i) {
        // 转换成pte
        pte_t pte = pagetable[i];
        if (pte & PTE_V) {
          // 先输出2级页表
          for (int j = 2; j >= level; --j) {
            printf("..");
            if (j > level) {
              printf(" ");
            }
          }
          // 获取物理地址
          uint64 child = PTE2PA(pte);
          printf("%d: pte %p pa %p\n", i, pte, child);
          // 如果有子页
          if ((pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            _vmprint((pagetable_t)child, level - 1);
          }
        }
      }
  }
  
  void vmprint(pagetable_t pagetable) {
    printf("page table %p\n", pagetable);
    _vmprint(pagetable, 2);
  }
  ```



## Task2

### 要求

Xv6有一个单独的用于在内核中执行程序时的内核页表。内核页表直接映射（恒等映射）到物理地址，也就是说内核虚拟地址`x`映射到物理地址仍然是`x`。Xv6还为每个进程的用户地址空间提供了一个单独的页表，只包含该进程用户内存的映射，从虚拟地址0开始。因为内核页表不包含这些映射，所以用户地址在内核中无效。因此，当内核需要使用在系统调用中传递的用户指针（例如，传递给`write()`的缓冲区指针）时，内核必须首先将指针转换为物理地址。本节和下一节的目标是允许内核直接解引用用户指针。

你的第一项工作是修改内核来让每一个进程在内核中执行时使用它自己的内核页表的副本。修改`struct proc`来为每一个进程维护一个内核页表，修改调度程序使得切换进程时也切换内核页表。对于这个步骤，每个进程的内核页表都应当与现有的的全局内核页表完全一致。如果你的`usertests`程序正确运行了，那么你就通过了这个实验。



### 提示

- 在`struct proc`中为进程的内核页表增加一个字段
- 为一个新进程生成一个内核页表的合理方案是实现一个修改版的`kvminit`，这个版本中应当创造一个新的页表而不是修改`kernel_pagetable`。你将会考虑在`allocproc`中调用这个函数
- 确保每一个进程的内核页表都关于该进程的内核栈有一个映射。在未修改的XV6中，所有的内核栈都在`procinit`中设置。你将要把这个功能部分或全部的迁移到`allocproc`中
- 修改`scheduler()`来加载进程的内核页表到核心的`satp`寄存器(参阅`kvminithart`来获取启发)。不要忘记在调用完`w_satp()`后调用`sfence_vma()`
- 没有进程运行时`scheduler()`应当使用`kernel_pagetable`
- 在`freeproc`中释放一个进程的内核页表
- 你需要一种方法来释放页表，而不必释放叶子物理内存页面。
- 调式页表时，也许`vmprint`能派上用场
- 修改XV6本来的函数或新增函数都是允许的；你或许至少需要在***kernel/vm.c***和***kernel/proc.c***中这样做（但不要修改***kernel/vmcopyin.c***, ***kernel/stats.c***, ***user/usertests.c***, 和***user/stats.c***）
- 页表映射丢失很可能导致内核遭遇页面错误。这将导致打印一段包含`sepc=0x00000000XXXXXXXX`的错误提示。你可以在***kernel/kernel.asm\***通过查询`XXXXXXXX`来定位错误。



### 实现

首先，在 `struct proc` 中为进程的内核页表增加一个字段 `pagetable_t kernelpgtbl`

然后实现一个新的 `kvminit_new()`，和原来的 `kvminit`唯一不同的地方在于，这个新的函数创建一个pte，然后初始化并返回这个pte；在原来的函数中，因为只有一个全局的内核页表，所以它在函数中使用外部的内核页表

- 注意，C语言不支持默认参数，所以要么定义一个新函数，要么重载原来的函数

- ```c
  pagetable_t kvminit_new() {
    pagetable_t kernel_pagetable = (pagetable_t) kalloc();
    memset(kernel_pagetable, 0, PGSIZE);
  
    // uart registers
    kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W, kernel_pagetable);
  
    // virtio mmio disk interface
    kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W, kernel_pagetable);
  
    // CLINT
    kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W, kernel_pagetable);
  
    // PLIC
    kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W, kernel_pagetable);
  
    // map kernel text executable and read-only.
    kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X, kernel_pagetable);
  
    // map kernel data and the physical RAM we'll make use of.
    kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W, kernel_pagetable);
  
    // map the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X, kernel_pagetable);
    
    return kernel_pagetable;
  }
  ```

- 定义这个函数以后，原来的 `kvminit` 依然保留，因为依然需要用它来初始化全局的内核页表

- 在 `kvminit_new` 中用到了 `kvmmap` ，在原始的xv6中，这个函数也直接用全局内核页表，因此要对这个函数做一些修改，传入一个新的页表参数，并在函数中替换原来使用的全局内核页表

  - ```c
    void
    kvmmap(uint64 va, uint64 pa, uint64 sz, int perm, pagetable_t kernel_pagetable)
    {
      if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
        panic("kvmmap");
    }
    ```

- 在以上所有函数中，为了方便，我没有修改变量名（和全局页表同名）

实现`kvminit`之后已经可以创建相互独立的内核页表了，但还要处理内核栈。

- 原来xv6的设计中，所有处于内核态的进程共享一个页表，即意味着共享同一个地址空间。由于 xv6 支持多核/多进程调度，同一时间可能会有多个进程处于内核态，所以需要对所有处于内核态的进程创建其独立的内核态内的栈，也就是内核栈，供给其内核态代码执行过程

- xv6启动过程中，在 `procinit` 中为所有可能的64个进程位都预分配了内核栈 `kstack`，两个 `kstack` 之间隔了一个无映射的 `guard page`

  - ```c
    for(p = proc; p < &proc[NPROC]; p++) {
          initlock(&p->lock, "proc");
    
          // Allocate a page for the process's kernel stack.
          // Map it high in memory, followed by an invalid
          // guard page.
          char *pa = kalloc();
          if(pa == 0)
             panic("kalloc");
          uint64 va = KSTACK((int) (p - proc));
          kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
          p->kstack = va;
      }
    ```

  - 这里 `uint64 va = KSTACK((int) (p - proc));` 这行代码就是为不同的进程位分配空间的过程

- 在新的设计中，因为每个进程都支持自己的内核页表，所以**不应在procinit中初始化内核栈，而应该在进程创建allocproc的时候，为进程分配独立的内核页表和内核栈**

  - ```c
    
    found:
      p->pid = allocpid();
      // ...
    
      // user kernel page table
      // 创建新的内核页表
      p->kernelpgtbl = kvminit_new();
      if (p->kernelpgtbl == 0) {
        freeproc(p);
        release(&p->lock);
        return 0;
      }
    
      // move part of procinit here to set up a new process
      // 分配一个物理页
      char *pa = kalloc();
      if(pa == 0)
        panic("kalloc");
      // 把内核栈映射到固定的逻辑地址上
      uint64 va = KSTACK((int)0);
      kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W, p->kernelpgtbl);
      // 记录内核栈的逻辑地址，其实已经是固定的了，依然这样记录是为了避免需要修改其他部分 xv6 代码
      p->kstack = va;
    
      // ...
    
      return p;
    }
    ```

  - 这里需要注意的是，因为每个进程有独立的内核栈且不能方位其他进程的内核栈，所以直接绑定到固定地址即可，不会冲突，且不需要预分配那么大的空间

此时进程独立的内核页表已经完成创建，但因为 `satp` 寄存器中此时还是全局的内核页表，所以用户进程进入内核后还是使用全局内核页表，需要修改 `scheduler()`，在调度器将CPU交给进程前，切换到对应的内核页表

- 参考 `kvminithart`，调用 `s_satp()` 来将进程的内核页表传入 `satp` 寄存器；同时调用结束后要用 `sfence_vma()`来刷新TLB以及确保页表修改的及时生效

- 另外需要注意的是，在进程调度执行之后，还要切换回全局内核页表（用 `kvminithart` 即可实现），因为按照提示，没有进程运行的时候，应当使用全局内核页表

- ```c
  void
  scheduler(void)
  {
    struct proc *p;
    struct cpu *c = mycpu();
    
    c->proc = 0;
    for(;;){
      // Avoid deadlock by ensuring that devices can interrupt.
      intr_on();
      
      int found = 0;
      for(p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if(p->state == RUNNABLE) {
          // 新加入的部分，在确定了进程是RUNNABLE之后（即将执行前），传入当前进程的内核页表
          w_satp(MAKE_SATP(p->kernelpgtbl));
          sfence_vma();
          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          p->state = RUNNING;
          c->proc = p;
          swtch(&c->context, &p->context);
  		
          // 没有进程调度时切换回全局内核页表
          kvminithart();
  
          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
  
          found = 1;
        }
        release(&p->lock);
      }
  #if !defined (LAB_FS)
      if(found == 0) {
        intr_on();
        asm volatile("wfi");
      }
  #else
      ;
  #endif
    }
  }
  ```

最后需要完成的就是在进程结束后释放其独有的页表和内核栈，回收资源，防止内存泄漏（如果不这么做，usertests的reparent2会失败，`panic:kvmmap`，内存不足导致kvmmap分配页表失败）

- ```c
  // kernel/proc.c
  static void
  freeproc(struct proc *p)
  {
    // ...
    
    // 释放进程的内核栈
    void *kstack_pa = (void *)kvmpa(p->kstack, p->kernelpgtbl);
    kfree(kstack_pa);
    p->kstack = 0;
    
    // 注意：此处不能使用 proc_freepagetable，因为其不仅会释放页表本身，还会把页表内所有的叶节点对应的物理页也释放掉。
    // 这会导致内核运行所需要的关键物理页被释放，从而导致内核崩溃。
    // 这里使用 kfree(p->kernelpgtbl) 也是不足够的，因为这只释放了**一级页表本身**，而不释放二级以及三级页表所占用的空间。
    
    // 递归释放进程独享的页表，释放页表本身所占用的空间，但**不释放页表指向的物理页**
    kvm_free_kernelpgtbl(p->kernelpgtbl);
    p->kernelpgtbl = 0;
    p->state = UNUSED;
  }
  ```

- 这里用到了修改后的 `kvmpa`，新传入了一个 `p->kernelpgtbl` 参数，和最开始一样，修改一下函数定义即可

  - ```c
    uint64
    kvmpa(uint64 va, pagetable_t kernel_pagetable)
    {
      uint64 off = va % PGSIZE;
      pte_t *pte;
      uint64 pa;
      
      pte = walk(kernel_pagetable, va, 0);
      if(pte == 0)
        panic("kvmpa");
      if((*pte & PTE_V) == 0)
        panic("kvmpa");
      pa = PTE2PA(*pte);
      return pa+off;
    }
    ```

- 另外还自定义了一个 `kvm_free_kernelpgtbl` 函数来释放页表（释放所有的mapping，但不释放其指向的物理页），这个函数模仿 `freewalk`，但做了一些修改

  - 这里本来我想直接用 `freewalk`，但该函数中在判断页表不指向更低一级页表后会 panic

    - ```c
      else if(pte & PTE_V){
            panic("freewalk: leaf");
          }
      ```

    - 这导致usertests中总是因为这个原因失败

  - `kvm_free_kernelpgtbl` 如下：

    - ```c
      void kvm_free_kernelpgtbl(pagetable_t kernelpgtbl) {
        // there are 2^9 = 512 PTEs in a page table.
        for(int i = 0; i < 512; i++){
          pte_t pte = kernelpgtbl[i];
          if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
            // this PTE points to a lower-level page table.
            uint64 child = PTE2PA(pte);
            kvm_free_kernelpgtbl((pagetable_t)child);
            kernelpgtbl[i] = 0;
          } 
        }
        // 释放当前页表所占空间
        kfree((void*)kernelpgtbl);
      }
      ```

最后的最后，在编译的过程中，因为我们的修改影响了其他代码： virtio 磁盘驱动 virtio_disk.c 中调用了 kvmpa() 用于将虚拟地址转换为物理地址，这一操作在我们修改后的版本中，需要传入进程的内核页表，修改对应的代码即可

- ```c
  // virtio_disk.c
  #include "proc.h" // 添加头文件引入
  
  // ......
  
  void
  virtio_disk_rw(struct buf *b, int write)
  {
  // ......
  disk.desc[idx[0]].addr = (uint64) kvmpa(myproc()->kernelpgtbl, (uint64) &buf0); // 调用 myproc()，获取进程内核页表
  // ......
  }
  ```



## Task3

### 要求

内核的`copyin`函数读取用户指针指向的内存。它通过将用户指针转换为内核可以直接解引用的物理地址来实现这一点。这个转换是通过在软件中遍历进程页表来执行的。在本部分的实验中，您的工作是将用户空间的映射添加到每个进程的内核页表（上一节中创建），以允许`copyin`（和相关的字符串函数`copyinstr`）直接解引用用户指针。

将定义在***kernel/vm.c***中的`copyin`的主题内容替换为对`copyin_new`的调用（在***kernel/vmcopyin.c***中定义）；对`copyinstr`和`copyinstr_new`执行相同的操作。为每个进程的内核页表添加用户地址映射，以便`copyin_new`和`copyinstr_new`工作。如果`usertests`正确运行并且所有`make grade`测试都通过，那么你就完成了此项作业。

### 提示

- 先用对`copyin_new`的调用替换`copyin()`，确保正常工作后再去修改`copyinstr`
- 在内核更改进程的用户映射的每一处，都以相同的方式更改进程的内核页表。包括`fork()`, `exec()`, 和`sbrk()`.
- 不要忘记在`userinit`的内核页表中包含第一个进程的用户页表
- 用户地址的PTE在进程的内核页表中需要什么权限？(在内核模式下，无法访问设置了`PTE_U`的页面）
- 别忘了上面提到的PLIC限制

### 实现

这个实验的目标是，在进程的内核态页表中维护一个用户态页表映射的副本，这样使得内核态也可以对用户态传进来的指针（逻辑地址）进行解引用

- 相比原来 copyin 的实现的优势是，原来的 copyin 是通过软件模拟访问页表的过程获取物理地址的，而在内核页表内维护映射副本的话，可以利用 CPU 的硬件寻址功能进行寻址，效率更高并且可以受快表加速

要实现这样的效果，我们需要在每一处内核对用户页表进行修改的时候，将同样的修改也同步应用在进程的内核页表上，使得两个页表的程序段（0 到 PLIC 段）地址空间的映射同步

首先需要一个复制函数，将一个页表的对应字段复制到另一个页表上，参考已有的`uvmcopy` 函数，定义 `kvmcopy`

- 这个函数只拷贝页表项，不拷贝实际的物理页内存
- 注意题目的提示，因为内核无法直接访问用户页，所以要将页的权限设置为非用户页（$\&~PTE\_U$）

- ```c
  int kvmcopy(pagetable_t src, pagetable_t dst, uint64 start, uint64 sz) {
  pte_t *pte;
    uint64 pa, i;
    uint flags;
  
    // PGROUNDUP: prevent re-mapping already mapped pages (eg. when doing growproc)
    for(i = PGROUNDUP(start); i < start + sz; i += PGSIZE){
      if((pte = walk(src, i, 0)) == 0)
        panic("kvmcopymappings: pte should exist");
      if((*pte & PTE_V) == 0)
        panic("kvmcopymappings: page not present");
      pa = PTE2PA(*pte);
      // `& ~PTE_U` 表示将该页的权限设置为非用户页
      // 必须设置该权限，RISC-V 中内核是无法直接访问用户页的。
      flags = PTE_FLAGS(*pte) & ~PTE_U;
      if(mappages(dst, i, PGSIZE, pa, flags) != 0){
        goto err;
      }
    }
  
    return 0;
  
   err:
    uvmunmap(dst, PGROUNDUP(start), (i - PGROUNDUP(start)) / PGSIZE, 0);
    return -1;
  }
  ```

在映射的时候，能够用于映射程序内存的地址范围是 [0,PLIC)，要确保这个地址范围没有其他的映射冲突

- ![Figure 3.3](assets/mit6s081-lab3-figure-3-3.png)

- 在内核地址空间中，PLIC前有一段CLIENT的映射，可能会造成映射冲突

  - 查阅 xv6 book 的 Chapter 5 以及 start.c 可以知道 CLINT 仅在内核启动的时候需要使用到，而用户进程在内核态中的操作并不需要使用到该映射
  - 修改上一个task中的 `kvminit_new`，去掉其中对于 CLIENT 段的映射；
    - 全局内核页表需要，且不影响，因此 `kvminit` 中关于CLIENT的映射不需要修改

- 在exec中要加入对PLIC的检查，防止内存超出

  - ```c
    // Load program into memory.
      for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
        if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
          goto bad;
        if(ph.type != ELF_PROG_LOAD)
          continue;
        if(ph.memsz < ph.filesz)
          goto bad;
        if(ph.vaddr + ph.memsz < ph.vaddr)
          goto bad;
        uint64 sz1;
        if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0)
          goto bad;
        if(sz1 >= PLIC) { // 添加检测，防止程序大小超过 PLIC
          goto bad;
        }
        sz = sz1;
        if(ph.vaddr % PGSIZE != 0)
          goto bad;
        if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
          goto bad;
      }
      iunlockput(ip);
      end_op();
      ip = 0;
    ```

接下来实现同步映射

- **fork**

  - ```c
     if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0 ||
         kvmcopy(np->pagetable, np->kernelpgtbl, 0, p->sz) < 0){
    ```

  - 在将用户态的父进程的内存拷贝给子进程的时候，顺便把用户态页表和复制给内核页表
  - **注意，这里 `kvmcopy` 中的复制的大小一定是 `p->sz` 不能是 `np->sz` **，因为父进程的页表大小和子进程可能不同
    - 如果用 `np->sz`，在 `make qemu` 初始化的时候会陷入 `panic:kerneltrap`

- **exec**

  - `exec` 系统调用的主要任务是将一个新的程序加载到当前进程的地址空间中，这意味着需要完全替换掉旧的程序代码和数据。旧的地址空间和内存映射与新程序不相关，可能会导致冲突或不一致。因此，需要清除旧的内存映射，以确保新程序能够在干净的地址空间中运行

  - ```c
    exec(char *path, char **argv)
    {
      // ......
    
      // Save program name for debugging.
      for(last=s=path; *s; s++)
        if(*s == '/')
          last = s+1;
      safestrcpy(p->name, last, sizeof(p->name));
    
      // 清除内核页表中对程序内存的旧映射，然后重新建立映射。
      uvmunmap(p->kernelpgtbl, 0, PGROUNDUP(oldsz)/PGSIZE, 0);
      kvmcopy(pagetable, p->kernelpgtbl, 0, sz);
      
      // ,,,
    }
    ```

- **sbrk**

  - sys_sbrk 中只有 growproc 这个函数涉及内存，其负责将用户内存增加或缩小

  - 定义一个新函数，`kvmdealloc`，仿照 `uvmdealloc`，缩小内存，但不释放实际内存

    - ```c
      uint64
      kvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
      {
        if(newsz >= oldsz)
          return oldsz;
      
        if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
          int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
          uvmunmap(pagetable, PGROUNDUP(newsz), npages, 0);
        }
      
        return newsz;
      }
      ```

  - 完成 `growproc`

    - ```c
       if(n > 0){
          uint64 newsz;
          if((newsz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
            return -1;
          }
          // 内核页表中的映射同步扩大
          if(kvmcopymappings(p->pagetable, p->kernelpgtbl, sz, n) != 0) {
            uvmdealloc(p->pagetable, newsz, sz);
            return -1;
          }
          sz = newsz;
        } else if(n < 0){
          uvmdealloc(p->pagetable, sz, sz + n);
          // 内核页表中的映射同步缩小
          sz = kvmdealloc(p->kernelpgtbl, sz, sz + n);
        }
        p->sz = sz;
        return 0;
      ```

修改 `userinit`

- init 进程不通过fork得到，因此也要修改以完成同步映射

- ```c
    // allocate one user page and copy init's instructions
    // and data into it.
    uvminit(p->pagetable, initcode, sizeof(initcode));
    p->sz = PGSIZE;
    kvmcopymappings(p->pagetable, p->kernelpgtbl, 0, p->sz); // 同步程序内存映射到进程内核页表中
  ```

最后用 `copyin_new,copyinstr_new`替换原来函数的部分即可

- ```c
  int
  copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
  {
    return copyin_new(pagetable, dst, srcva, len);
  }
  
  int
  copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
  {
    return copyinstr_new(pagetable, dst, srcva, max);
  }
  ```

## 结果

![image-20240615192657258](assets/image-20240615192657258.png)



# 问题与思考

1. scheduler中，在切换进程之前，为什么satp是切换到内核页表根地址，而不是切换到用户页表根地址？这时进程如何访问用户虚存空间？
   - scheduler并不是内核态转用户态的临界点，所以并不切换到用户页表，只切换到对应进程的内核页表。
     - 这是由于scheduler调度的目标进程，之前的现场**一定**是在内核代码中，而不可能是在用户代码中！
       - 这是因为一个进程交出执行权之前，要触发时钟中断，这个过程就进入内核态usertrap了，然后由usertrap执行yield让出cpu。所以自然恢复现场的时候，也是要恢复到yield中，而不是直接恢复到用户态代码中
   - 实际的内核到用户页表的切换，是在后续swtch()到目标进程后，目标进程继续执行直至usertrapret处，由usertrapret将进程页表传递给trampoline，然后由trampoline完成的。
     - 实际上所有的用户态和内核态之间的互相切换，都是由trampoline完成的，以及负责中断时的现场保护
   - scheduler和swtch()只负责内核态不同进程之间的切换，并不负责用户态和内核态之间的切换
2.  exec 里加的`uvmunmap(p->kernelpgtbl, 0, PGROUNDUP(oldsz)/PGSIZE, 0);`，作用应该只是把 `p->kernelpgtbl` 第三级页表的 pte 给置 0 了吧，没有把二级页表和三级页表占用的空间进行释放：如果使用 `exec` 系统调用的程序很大，sz 大于等于 512\*4096 byte，那么这个程序的三级页表会占用两页；如果使用 `exec` 以后替换的程序很小，sz 小于 512*4096 byte，那么只会用到两页三级页表的第一页，第二页三级页表可能就一直没被回收
   - 如果exec的程序明显小于原来的程序大小的话，确实是会导致第三级甚至第二第一级的页表本身占用的页直到进程退出（freewalk）前都不会被回收的
     - 这个其实是uvmunmap本身的实现问题，uvmunmap本身在取消映射的时候就不会检查是否页表已全空，更不会去尝试释放这个三级页表本身所占用的内存
     - 这也就意味着，其实不止我们实现的exec会有页表没被释放的问题，任何unmap了页的方法都会有，例如sbrk到很大（大于512个页，使其消耗超过一个三级页表）之后再sbrk回去
   - 这个具体这么设计的理由暂时未知，不过个人猜测的理由是
     - 首先这个并不是那么常见的问题，exec这种方法常是shell之类程序使用，而xv6里的shell编译后的EFI文件也只有44KB，远比512KB要小得多
     - 即使是像用sbrk扩大后再缩小回去这种情况，即使假设扩大sz到1GB，也只是512个3级页表，即512*4KB=2MB的三级页表占用空间,对于一台可以让用户程序随意分配1GB内存的机器来说，2MB几乎是可忽略的空间消耗
   - 假设要释放这额外的空间占用的话，有两种方法：
     - 第一种是在uvmunmap里面添加检测，在三级页表为空的时候，将该三级页表释放
       - 这种方法的弊端是很明显的，即在每次内存释放的时候，都需要扫描一次三级页表来检查其是否全为空，严重影响性能。
       - 虽然可以用额外的计数来缓解，但是这么做的话又需要保证所有对页表进行操作的地方都更新这个冗余计数，否则会出现不一致导致的欠释放/误释放。
     - 针对第一种的局限性，由于程序的虚拟内存都是连续映射的，所以并不需要每释放一个页都检查三级页表是否为空，释放的时候就可以通过释放范围知道页表在释放后是否会为空了
       - 所以可以实现一个ranged unmap的方法，在范围释放页后如果释放的页刚好覆盖一整个三级页表，则顺便释放这个三级页表。
       - 很明显如果一个三级页表中的页不是一次性被释放掉的话，依然是不会触发这个三级页表的释放的，所以实际的方案还要比这个更复杂
3. 对于lab3 prob3的理解，按照他的要求，在fork,exec等函数对kernel pgtable和user pgtable做了相同的映射操作后，为什么在copyin_new时就能够直接引用user提供的srcva呢？这个时候不是还应该从kernel pgtable中walk吗？还是说这里有硬件的支持，因此这个过程不需要代码实现呢。
   - walk的过程其实本来就是，在软件中模拟硬件访问内存地址的时候MMU所做的硬件转换过程。当当前页表（内核页表）有这个映射，就可以直接访问，直接利用硬件的地址转换能力。这样相比软件转换而言性能也会高一些。
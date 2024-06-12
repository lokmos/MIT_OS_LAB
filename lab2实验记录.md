# Chapter2：操作系统架构

## 抽象系统资源

为了实现强隔离， 最好禁止应用程序直接访问敏感的硬件资源，而是将资源抽象为服务。 

- Unix应用程序只通过文件系统的`open`、`read`、`write`和`close`系统调用与存储交互，而不是直接读写磁盘。

Unix在进程之间透明地切换硬件处理器，根据需要保存和恢复寄存器状态，这样应用程序就不必意识到分时共享的存在。

- 这种透明性允许操作系统共享处理器

## 用户态、核心态、系统调用

强隔离需要应用程序和操作系统之间的**硬边界**

- 操作系统必须保证应用程序不能修改（甚至读取）操作系统的数据结构和指令，
- 以及应用程序不能访问其他进程的内存

CPU为强隔离提供硬件支持，RISC-V有三种CPU可以执行指令的模式：机器模式(Machine Mode)、用户模式(User Mode)和管理模式(Supervisor Mode)。

- 在机器模式下执行的指令具有完全特权；CPU在机器模式下启动。Xv6在机器模式下执行很少的几行代码，然后更改为管理模式。
- 在管理模式下，CPU被允许执行特权指令：例如，启用和禁用中断、读取和写入保存页表地址的寄存器等。
  - 如果用户模式下的应用程序试图执行特权指令，那么CPU不会执行该指令，而是切换到管理模式
- 应用程序只能执行用户模式的指令（例如，数字相加等），并被称为在**用户空间**中运行，而此时处于管理模式下的软件可以执行特权指令，并被称为在**内核空间**中运行。在内核空间（或管理模式）中运行的软件被称为内核。

想要调用内核函数的应用程序，必须过渡到内核。CPU提供一个特殊的指令，将CPU从用户模式切换到管理模式，并在内核指定的入口点进入内核（RISC-V为此提供`ecall`指令）。

- 内核可以验证系统调用的参数，决定是否允许应用程序执行请求的操作，然后拒绝它或执行它。

- 由内核控制转换到管理模式的入口点是很重要的；如果应用程序可以决定内核入口点， 那么恶意应用程序可以在跳过参数验证的地方进入内核。

## 内核组织

**宏内核**

一种可能是整个操作系统都驻留在内核中，这样所有系统调用的实现都以管理模式运行。这种组织被称为**宏内核（monolithic kernel）**。

- 整个操作系统以完全的硬件特权运行
- 操作系统的不同部分更容易合作
- 一个缺点是操作系统不同部分之间的接口通常很复杂，管理模式中的错误经常会导致内核失败。如果内核失败，计算机停止工作，因此所有应用程序也会失败

**微内核**

为了降低内核出错的风险，操作系统设计者可以最大限度地减少在管理模式下运行的操作系统代码量，并在用户模式下执行大部分操作系统。这种内核组织被称为**微内核（microkernel）**

![img](assets/p1.png)

- 内核接口由一些用于启动应用程序、发送消息、访问设备硬件等的低级功能组成。这种组织允许内核相对简单，因为大多数操作系统驻留在用户级服务器中。



## 进程

Xv6使用页表（由硬件实现）为每个进程提供自己的地址空间。RISC-V页表将虚拟地址（RISC-V指令操纵的地址）转换（或“映射”）为物理地址（CPU芯片发送到主存储器的地址）。

![img](assets/p2.png)

- Xv6为每个进程维护一个单独的页表，定义了该进程的地址空间。
  - 以虚拟内存地址0开始的进程的用户内存地址空间。首先是指令，然后是全局变量，然后是栈区，最后是一个堆区域（用于`malloc`）以供进程根据需要进行扩展。
  - 许多因素限制了进程地址空间的最大范围
    - RISC-V上的指针有64位宽
    - 硬件在页表中查找虚拟地址时只使用低39位；xv6只使用这39位中的38位。因此，最大地址是2^38-1=0x3fffffffff，即`MAXVA`（定义在***kernel/riscv.h***）
  - 在地址空间的顶部，xv6为`trampoline`（用于在用户和内核之间切换）和映射进程切换到内核的`trapframe`分别保留了一个页面
- Xv6内核为每个进程维护许多状态片段，并将它们聚集到一个`proc`(***kernel/proc.h***）结构体中
  - 一个进程最重要的内核状态片段是它的页表、内核栈区和运行状态。
  - 使用符号`p->xxx`来引用`proc`结构体的元素；例如，`p->pagetable`是一个指向该进程页表的指针。
  - `p->state`表明进程是已分配、就绪态、运行态、等待I/O中（阻塞态）还是退出。
- 每个进程都有一个执行线程（或简称线程）来执行进程的指令。一个线程可以挂起并且稍后再恢复。为了透明地在进程之间切换，内核挂起当前运行的线程，并恢复另一个进程的线程。
  - 线程的大部分状态（本地变量、函数调用返回地址）存储在**线程的栈区**上。**每个进程有两个栈区：一个用户栈区和一个内核栈区**（`p->kstack`）
  - 当进程执行**用户指令**时，只有它的用户栈在使用，它的内核栈是空的。
  - 当进程**进入内核**时，内核代码在进程的**内核堆栈**上执行
    - 它的用户堆栈仍然包含保存的数据，只是不处于活动状态
  - 进程的内核栈是独立的，且不受用户代码保护
    - 即使进程破坏了其用户栈，内核依然可以正常运行

## 启动Xv6

当RISC-V计算机上电时，它会初始化自己并运行一个存储在只读内存中的引导加载程序。引导加载程序将xv6内核加载到内存中。然后，在机器模式下，中央处理器从`_entry` (***kernel/entry.S***:6)开始运行xv6。Xv6启动时页式硬件（paging hardware）处于禁用模式：也就是说虚拟地址将直接映射到物理地址。

- 加载程序将xv6内核加载到物理地址为`0x80000000`的内存中。它将内核放在`0x80000000`而不是`0x0`的原因是地址范围`0x0:0x80000000`包含I/O设备。

- ```shell
  _entry:
          # set up a stack for C.
          # stack0 is declared in start.c,
          # with a 4096-byte stack per CPU.
          # sp = stack0 + (hartid * 4096)
          la sp, stack0
          li a0, 1024*4
          csrr a1, mhartid
          addi a1, a1, 1
          mul a0, a0, a1
          add sp, sp, a0
          # jump to start() in start.c
          call start
  ```

`_entry`的指令设置了一个栈区，这样xv6就可以运行C代码。Xv6在***start. c (kernel/start.c:11)***文件中为初始栈***stack0***声明了空间。

- ```c
  // entry.S needs one stack per CPU.
  __attribute__ ((aligned (16))) char stack0[4096 * NCPU];
  ```

- RISC-V上的栈是向下扩展的，所以`_entry`的代码将栈顶地址`stack0+4096`加载到栈顶指针寄存器`sp`中。

函数`start`执行一些仅在机器模式下允许的配置，然后切换到管理模式。

- 最常用的进入管理模式的指令是 `mret`
- `start` 返回前的工作
  - 在寄存器`mstatus`中将先前的运行模式改为管理模式
  - 通过将`main`函数的地址写入寄存器`mepc`将返回地址设为`main`
  - 通过向页表寄存器`satp`写入0来在管理模式下禁用虚拟地址转换，并将所有的中断和异常委托给管理模式
  - 对时钟芯片进行编程以产生计时器中断
  - `start`通过调用`mret`“返回”到管理模式。这将导致程序计数器（PC）的值更改为`main`(***kernel/main.c***:11)函数地址

# Chapter4 的部分内容

## 代码：调用系统调用

用户代码将`exec`需要的参数放在寄存器`a0`和`a1`中，并将系统调用号放在`a7`中。

- 系统调用号与`syscalls`数组中的条目相匹配，`syscalls`数组是一个函数指针表（***kernel/syscall.c***:108）。

`syscall`（***kernel/syscall.c***:133）从陷阱帧（trapframe）中保存的`a7`中检索系统调用号（`p->trapframe->a7`），并用它索引到`syscalls`中

当系统调用接口函数返回时，`syscall`将其返回值记录在`p->trapframe->a0`中。这将导致原始用户空间对`exec()`的调用返回该值，因为RISC-V上的C调用约定将返回值放在`a0`中。

- ```c
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
      // Use num to lookup the system call function for num, call it,
      // and store its return value in p->trapframe->a0
      p->trapframe->a0 = syscalls[num]();
    } else {
      printf("%d %s: unknown sys call %d\n",
              p->pid, p->name, num);
      p->trapframe->a0 = -1;
    }
  ```

- 返回负数表示错误，返回零或正数表示成功。如果系统调用号无效，`syscall`打印错误并返回-1。



## 系统调用参数

内核中的系统调用接口需要找到用户代码传递的参数。

- 参数最初被放置在RISC-V C调用所约定的地方：寄存器

  - 内核陷阱代码将用户寄存器保存到当前进程的陷阱框架中，内核代码可以在那里找到它们

- 调用`argraw`来检索相应的保存的用户寄存器（***kernel/syscall.c***:35）

  - ```c
    static uint64
    argraw(int n)
    {
      struct proc *p = myproc();
      switch (n) {
      case 0:
        return p->trapframe->a0;
      case 1:
        return p->trapframe->a1;
      case 2:
        return p->trapframe->a2;
      case 3:
        return p->trapframe->a3;
      case 4:
        return p->trapframe->a4;
      case 5:
        return p->trapframe->a5;
      }
      panic("argraw");
      return -1;
    }
    ```

内核实现了安全地将数据传输到用户提供的地址和从用户提供的地址传输数据的功能。`fetchstr`是一个例子（***kernel/syscall.c***:25）。文件系统调用，如`exec`，使用`fetchstr`从用户空间检索字符串文件名参数。`fetchstr`调用`copyinstr`来完成这项困难的工作。

- `copyinstr`（***kernel/vm.c***:406）从用户页表页表中的虚拟地址`srcva`复制`max`字节到`dst`。它使用`walkaddr`（它又调用`walk`）在软件中遍历页表，以确定`srcva`的物理地址`pa0`。由于内核将所有物理RAM地址映射到同一个内核虚拟地址，`copyinstr`可以直接将字符串字节从`pa0`复制到`dst`。`walkaddr`（***kernel/vm.c***:95）检查用户提供的虚拟地址是否为进程用户地址空间的一部分，因此程序不能欺骗内核读取其他内存。一个类似的函数`copyout`，将数据从内核复制到用户提供的地址。

# 实验

## Task1

### 要求

- 在本作业中，您将添加一个系统调用跟踪功能，该功能可能会在以后调试实验时对您有所帮助。您将创建一个新的`trace`系统调用来控制跟踪。它应该有一个参数，这个参数是一个整数“掩码”（mask），它的比特位指定要跟踪的系统调用。例如，要跟踪`fork`系统调用，程序调用`trace(1 << SYS_fork)`，其中`SYS_fork`是***kernel/syscall.h\***中的系统调用编号。如果在掩码中设置了系统调用的编号，则必须修改xv6内核，以便在每个系统调用即将返回时打印出一行。该行应该包含进程id、系统调用的名称和返回值；您不需要打印系统调用参数。`trace`系统调用应启用对调用它的进程及其随后派生的任何子进程的跟踪，但不应影响其他进程。

### 提示

我们提供了一个用户级程序版本的`trace`，它运行另一个启用了跟踪的程序（参见***user/trace.c***）。完成后，您应该看到如下输出：

```bash
$ trace 32 grep hello README
3: syscall read -> 1023
3: syscall read -> 966
3: syscall read -> 70
3: syscall read -> 0
$
$ trace 2147483647 grep hello README
4: syscall trace -> 0
4: syscall exec -> 3
4: syscall open -> 3
4: syscall read -> 1023
4: syscall read -> 966
4: syscall read -> 70
4: syscall read -> 0
4: syscall close -> 0
$
$ grep hello README
$
$ trace 2 usertests forkforkfork
usertests starting
test forkforkfork: 407: syscall fork -> 408
408: syscall fork -> 409
409: syscall fork -> 410
410: syscall fork -> 411
409: syscall fork -> 412
410: syscall fork -> 413
409: syscall fork -> 414
411: syscall fork -> 415
...
$
```

在上面的第一个例子中，`trace`调用`grep`，仅跟踪了`read`系统调用。`32`是`1<<SYS_read`。在第二个示例中，`trace`在运行`grep`时跟踪所有系统调用；`2147483647`将所有31个低位置为1。在第三个示例中，程序没有被跟踪，因此没有打印跟踪输出。在第四个示例中，在`usertests`中测试的`forkforkfork`中所有子孙进程的`fork`系统调用都被追踪。如果程序的行为如上所示，则解决方案是正确的（尽管进程ID可能不同）

- 在***Makefile***的**UPROGS**中添加`$U/_trace`
- 运行`make qemu`，您将看到编译器无法编译***user/trace.c***，因为系统调用的用户空间存根还不存在：将系统调用的原型添加到***user/user.h***，存根添加到***user/usys.pl***，以及将系统调用编号添加到***kernel/syscall.h***，***Makefile***调用perl脚本***user/usys.pl***，它生成实际的系统调用存根***user/usys.S***，这个文件中的汇编代码使用RISC-V的`ecall`指令转换到内核。一旦修复了编译问题（*注：如果编译还未通过，尝试先`make clean`，再执行`make qemu`*），就运行`trace 32 grep hello README`；但由于您还没有在内核中实现系统调用，执行将失败。
- 在***kernel/sysproc.c***中添加一个`sys_trace()`函数，它通过将参数保存到`proc`结构体（请参见***kernel/proc.h***）里的一个新变量中来实现新的系统调用。从用户空间检索系统调用参数的函数在***kernel/syscall.c***中，您可以在***kernel/sysproc.c***中看到它们的使用示例。
- 修改`fork()`（请参阅***kernel/proc.c***）将跟踪掩码从父进程复制到子进程。
- 修改***kernel/syscall.c***中的`syscall()`函数以打印跟踪输出。您将需要添加一个系统调用名称数组以建立索引。

### 实现

将系统调用添加

- 将系统调用的原型添加到***user/user.h***

- ```c
  // system calls
  int fork(void);
  // ...
  int trace(int);

- 存根添加到***user/usys.pl***

- ```c
  entry("fork");
  // ...
  entry("trace");
  ```

- ***kernel/syscall.h***添加系统调用号

- ```c
  #define SYS_trace  22
  ```

- ***kernel/syscall.c***添加函数原型和对应的函数调用数组

- ```c
  extern uint64 sys_trace(void);
  
  static uint64 (*syscalls[])(void) = {
  [SYS_fork]    sys_fork,
  // ...
  [SYS_trace]   sys_trace,
  };
  
  ```

完成`sys_trace()`函数

- 先做一些准备工作，在`proc`结构中添加一个新的表示掩码的变量

  - ***kernel/proc.h***

  - ```c
    struct proc {
      // ...
      int mask;
    };
    ```

- 从用户空间检索系统调用参数的函数

  - 是 ***kernel/syscall.c*** 中的 `argint`，已经写好了

  - ```c
    // Fetch the nth 32-bit system call argument.
    int
    argint(int n, int *ip)
    {
      *ip = argraw(n);
      return 0;
    }
    ```

- 完成`sys_trace`

  - 函数接受一个整型参数（掩码）

  - 在 ***kernel/sysproc.c*** 中实现

  - ```c
    uint64
    sys_trace(int mask)
    {
      // 获取系统调用号并存在proc结构中
      if (argint(0, &(myproc()->mask)) < 0)
        return -1;
      return 0;
    }
    ```

修改 `fork`

- 就是在把父进程的proc复制给子进程时，多加一行，把掩码也复制

- ***kernel/proc.c***

- ```c
  int
  fork(void)
  {
    // ...
  
    np->state = RUNNABLE;
  
    // trace
    np->mask = p->mask;
  
    release(&np->lock);
  
    return pid;
  }
  ```

修改 `syscall`

- 定义系统调用名称数组以建立索引

  - ***kernel/syscall.c***

  - ```c
    static char *sysnames[] = {
    [SYS_fork]    "fork",
    [SYS_exit]    "exit",
    [SYS_wait]    "wait",
    [SYS_pipe]    "pipe",
    [SYS_read]    "read",
    [SYS_kill]    "kill",
    [SYS_exec]    "exec",
    [SYS_fstat]   "fstat",
    [SYS_chdir]   "chdir",
    [SYS_dup]     "dup",
    [SYS_getpid]  "getpid",
    [SYS_sbrk]    "sbrk",
    [SYS_sleep]   "sleep",
    [SYS_uptime]  "uptime",
    [SYS_open]    "open",
    [SYS_write]   "write",
    [SYS_mknod]   "mknod",
    [SYS_unlink]  "unlink",
    [SYS_link]    "link",
    [SYS_mkdir]   "mkdir",
    [SYS_close]   "close",
    [SYS_trace]   "trace",
    };
    ```

- 打印

  - 在**跟踪掩码和当前调用的函数掩码一致时**才打印

    - ```c
      void
      syscall(void)
      {
        int num;
        struct proc *p = myproc();
      
        num = p->trapframe->a7;
        // 下面这个if判断是必须的，否则会跟踪所有的函数调用，从而打印了一些不需要的信息
        if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
          p->trapframe->a0 = syscalls[num]();
          if (1 << num & p->mask)
            printf("%d: syscall %s -> %d\n", p->pid, sysnames[num], p->trapframe->a0);
        } else {
          printf("%d %s: unknown sys call %d\n",
                  p->pid, p->name, num);
          p->trapframe->a0 = -1;
        }
      }
      ```

    - 上述if判断中用 & 而不是 == 是因为题目要求在31为全为1时，应当跟踪所有调用

## Task2

### 要求

- 在这个作业中，您将添加一个系统调用`sysinfo`，它收集有关正在运行的系统的信息。系统调用采用一个参数：一个指向`struct sysinfo`的指针（参见***kernel/sysinfo.h***）。内核应该填写这个结构的字段：`freemem`字段应该设置为空闲内存的字节数，`nproc`字段应该设置为`state`字段不为`UNUSED`的进程数。我们提供了一个测试程序`sysinfotest`；如果输出“**sysinfotest: OK**”则通过。

### 提示

- 在***Makefile***的**UPROGS**中添加`$U/_sysinfotest`
- 当运行`make qemu`时，***user/sysinfotest.c***将会编译失败，遵循和上一个作业一样的步骤添加`sysinfo`系统调用。要在***user/user.h***中声明`sysinfo()`的原型，需要预先声明`struct sysinfo`的存在：

```c
struct sysinfo;
int sysinfo(struct sysinfo *);
```

一旦修复了编译问题，就运行`sysinfotest`；但由于您还没有在内核中实现系统调用，执行将失败。

- `sysinfo`需要将一个`struct sysinfo`复制回用户空间；请参阅`sys_fstat()`(***kernel/sysfile.c***)和`filestat()`(***kernel/file.c***)以获取如何使用`copyout()`执行此操作的示例。
- 要获取空闲内存量，请在***kernel/kalloc.c***中添加一个函数
- 要获取进程数，请在***kernel/proc.c***中添加一个函数



### 实现

添加系统调用

- 和Task1一样，不再赘述

在***kernel/kalloc.c***中添加一个函数获取空闲内存量

- xv6 中，空闲内存页的记录方式是，将空虚内存页**本身**直接用作链表节点，形成一个空闲页链表，每次需要分配，就把链表根部对应的页分配出去。每次需要回收，就把这个页作为新的根节点，把原来的 freelist 链表接到后面。注意这里是**直接使用空闲页本身**作为链表节点，所以不需要使用额外空间来存储空闲页链表，在 kalloc() 里也可以看到，分配内存的最后一个阶段，是直接将 freelist 的根节点地址（物理地址）返回出去了

- 仿照 `kalloc` 的方法，遍历 `kmem.freelist` 这个空闲列表的数据结构，每次加上 `PGSIZE` 的字节数，即可得到空闲内存量

- 注意这里需要加锁

  - ```c
    int get_free_mem(void) {
      struct run *r;
      int count = 0;
      acquire(&kmem.lock);
      r = kmem.freelist;
      while (r) {
        count += PGSIZE;
        r = r->next;
      }
      release(&kmem.lock);
      return count;
    }
    ```

在***kernel/proc.c***中添加一个函数获取进程数

- xv6的进程都保存在一个proc数组中

  - ```c
    struct proc proc[NPROC];

- 遍历这个数组，找到那些 `state != UNUSED` 的进程，即可得到

  - ```c
    int get_process(void) {
      struct proc *p;
      int count = 0;
      for (p = proc; p < &proc[NPROC]; p++) {
        if (p->state != UNUSED) {
          count++;
        }
      }
      return count;
    }
    ```

  - 这里不需要锁住proc，因为只读取进程列表而不写

实现 ***sysinfo*** 系统调用

- sysinfo 的结构体已经在 ***sysinfo.h*** 的头文件中定义好了

- ***kernel/sysproc.c***

  - ```c
    uint64
    sys_sysinfo(void)
    {
      struct sysinfo info;
      // 调用自定义的两个函数
      info.freemem = get_free_mem();
      info.nproc = get_process();
      // 获取虚拟地址（copyout需要复制到的用户空间的地址）
      uint64 det;
      argaddr(0, &det);
      // 调用copyout，将从info地址开始的info大小的内容复制给用户空间
      if (copyout(myproc()->pagetable, det, (char*)&info, sizeof(info)) < 0)
        return -1;
      return 0;
    }
    ```

  - 注意这里函数的参数是void，而题目中声明的时候参数是 struct sysinfo*，如果函数定义也用 struct sysinfo\* 测试会无法执行下去

    - 猜测是测试的方式有关，可能没有传入相应的结构，因为后续也没有用到sysinfo，在函数中定义并使用是合理的做法

# 结果

![image-20240612113817875](assets/image-20240612113817875.png)
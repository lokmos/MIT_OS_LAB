# Chapter4：陷阱指令和系统调用

## RISC-V陷入机制

每个RISC-V CPU都有一组控制寄存器，内核通过向这些寄存器写入内容来告诉CPU如何处理陷阱，内核可以读取这些寄存器来明确已经发生的陷阱。

- `stvec`：内核在这里写入其陷阱处理程序的地址；RISC-V跳转到这里处理陷阱。
- `sepc`：当发生陷阱时，RISC-V会在这里保存程序计数器`pc`（因为`pc`会被`stvec`覆盖）。
  - `sret`（从陷阱返回）指令会将`sepc`复制到`pc`。内核可以写入`sepc`来控制`sret`的去向。
- `scause`： RISC-V在这里放置一个描述陷阱原因的数字。
- `sscratch`：内核在这里放置了一个值，这个值在陷阱处理程序一开始就会派上用场。
- `sstatus`：其中的**SIE**位控制设备中断是否启用。
  - 如果内核清空**SIE**，RISC-V将推迟设备中断，直到内核重新设置**SIE**。
  - **SPP**位指示陷阱是来自用户模式还是管理模式，并控制`sret`返回的模式。

**上述寄存器都用于在管理模式下处理陷阱，用户模式下无法读写**

- 机器模式下有一组等效的控制寄存器，但xv6仅在计时器中断时使用它们
- 多核芯片上的每个CPU都有自己的这些寄存器集，并且在任何给定时间都可能有多个CPU在处理陷阱

**陷阱执行过程**（计时器中断除外）

- 如果陷阱是设备中断，并且状态**SIE**位被清空，则不执行以下任何操作
- 清除**SIE**以禁用中断。
- 将`pc`复制到`sepc`。
- 将当前模式（用户或管理）保存在状态的**SPP**位中。
- 设置`scause`以反映产生陷阱的原因。
- 将模式设置为管理模式。
- 将`stvec`复制到`pc`。
- 在新的`pc`上开始执行。

CPU不会切换到内核页表，不会切换到内核栈，也不会保存除`pc`之外的任何寄存器

- 内核软件必须执行这些任务
- CPU在陷阱期间执行尽可能少量工作的一个原因是为软件提供灵活性
  - 一些操作系统在某些情况下不需要页表切换，可以提高性能

CPU使用专门的寄存器 `stvec` 切换到内核指定的指令地址

- 如果不切换程序计数器，程序可以在仍然运行用户指令的情况下切换到管理模式
- 但打破了用户/内核的隔离机制，可以修改 `satp` 寄存器来指向允许访问所有物理内存的页表



## 从用户空间陷入

来自用户空间的陷阱的高级路径是`uservec` ，然后是`usertrap`；返回时，先是`usertrapret` ，然后是`userret`。

- 因为 `satp` 指向不映射内核的用户页表，栈指针可能包含无效甚至恶意的值
- RISC-V硬件在陷阱期间不会切换页表
  - 所以用户页表必须包括`uservec`（**stvec**指向的陷阱向量指令）的映射
  - `uservec`必须切换`satp`以指向内核页表
  - 为了在切换后继续执行指令，`uservec`必须在内核页表中与用户页表中映射相同的地址

xv6使用包含`uservec`的蹦床页面（trampoline page）来满足这些约束

- xv6将蹦床页面映射到内核页表和每个用户页表中相同的虚拟地址，这个虚拟地址是`TRAMPOLINE`
- 蹦床内容在***trampoline.S***中设置，并且（当执行用户代码时）`stvec`设置为`uservec`

当`uservec`启动时，所有32个寄存器都包含被中断代码所拥有的值，但是`uservec`需要能够修改一些寄存器，以便设置`satp`并生成保存寄存器的地址。

- RISC-V以`sscratch`寄存器的形式提供了帮助。`uservec`开始时的`csrrw`指令交换了`a0`和`sscratch`的内容。
  - 现在用户代码的`a0`被保存了：在 `sscrath` 中
  - `uservec`有一个寄存器（`a0`）可以使用
  - `a0`包含内核以前放在`sscratch`中的值

`uservec`的下一个任务是保存用户寄存器

- 在进入用户空间之前，内核先前将`sscratch`设置为指向一个每个进程的陷阱帧，该帧（除此之外）具有保存所有用户寄存器的空间
- 因为`satp`仍然指向用户页表，所以`uservec`需要将陷阱帧映射到用户地址空间中
- 每当创建一个进程时，xv6就为该进程的陷阱帧分配一个页面，并安排它始终映射在用户虚拟地址`TRAPFRAME`
  - 该地址就在`TRAMPOLINE`下面
- 尽管使用物理地址，该进程的`p->trapframe`仍指向陷阱帧，这样内核就可以通过内核页表使用它

在交换`a0`和`sscratch`之后，`a0`持有指向当前进程陷阱帧的指针。`uservec`现在保存那里的所有用户寄存器，包括从`sscratch`读取的用户的`a0`

- 陷阱帧包含指向当前进程内核栈的指针、当前CPU的`hartid`、`usertrap`的地址和内核页表的地址。

`uservec`取得这些值，将`satp`切换到内核页表，并调用`usertrap`

`usertrap`的任务是确定陷阱的原因，处理并返回(***kernel/trap.c***:37)。

- 首先改变`stvec`，这样内核中的陷阱将由`kernelvec`处理
- 保存了`sepc`（保存的用户程序计数器），再次保存是因为`usertrap`中可能有一个进程切换，可能导致`sepc`被覆盖
- 处理陷阱
  - 如果陷阱来自系统调用，`syscall`会处理它
  - 如果是设备中断，`devintr`会处理
  - 否则它是一个异常，内核会杀死错误进程
- 系统调用路径在保存的用户程序计数器`pc`上加4，因为在系统调用的情况下，RISC-V会留下指向`ecall`指令的程序指针（返回后需要执行`ecall`之后的下一条指令）
- 在退出的过程中，`usertrap`检查进程是已经被杀死还是应该让出CPU（如果这个陷阱是计时器中断）

返回用户空间的第一步是调用`usertrapret` (***kernel/trap.c***:90)。

- 设置RISC-V控制寄存器，为将来来自用户空间的陷阱做准备

- 将`stvec`更改为指向`uservec`

  - ```c
    // send syscalls, interrupts, and exceptions to uservec in trampoline.S
      uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
      w_stvec(trampoline_uservec);
    ```

- 准备`uservec`所依赖的陷阱帧字段

  - ```c
    // set up trapframe values that uservec will need when
      // the process next traps into the kernel.
      p->trapframe->kernel_satp = r_satp();         // kernel page table
      p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
      p->trapframe->kernel_trap = (uint64)usertrap;
      p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()
    ```

- `sepc`设置为之前保存的用户程序计数器

  - ```c
    // set S Exception Program Counter to the saved user pc.
      w_sepc(p->trapframe->epc);
    ```

- 最后，`usertrapret`在用户和内核页表中都映射的蹦床页面上调用`userret`；原因是`userret`中的汇编代码会切换页表。

  - ```c
    / jump to userret in trampoline.S at the top of memory, which 
      // switches to the user page table, restores user registers,
      // and switches to user mode with sret.
      uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
      ((void (*)(uint64))trampoline_userret)(satp);
    ```

`usertrapret`对`userret`的调用将指针传递到`a0`中的进程用户页表和`a1`中的`TRAPFRAME` (***kernel/trampoline.S***:107)

`userret`将`satp`切换到进程的用户页表

- 用户页表同时映射蹦床页面和`TRAPFRAME`，但没有从内核映射其他内容
- 蹦床页面映射在用户和内核页表中的同一个虚拟地址上的事实允许用户在更改`satp`后继续执行

`userret`复制陷阱帧保存的用户`a0`到`sscratch`，为以后与`TRAPFRAME`的交换做准备

- 从此刻开始，`userret`可以使用的唯一数据是寄存器内容和陷阱帧的内容
- 下一个`userret`从陷阱帧中恢复保存的用户寄存器，做`a0`与`sscratch`的最后一次交换来恢复用户`a0`并为下一个陷阱保存`TRAPFRAME`，并使用`sret`返回用户空间



## 调用系统调用

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



## 从内核空间陷入

当在CPU上执行内核时，内核将`stvec`指向`kernelvec`(***kernel/kernelvec.S***:10)的汇编代码

- 由于xv6已经在内核中，`kernelvec`可以依赖于设置为内核页表的`satp`
- 以及指向有效内核栈的栈指针
- `kernelvec`保存所有寄存器，以便被中断的代码最终可以不受干扰地恢复

`kernelvec`将寄存器保存在**被中断的内核线程的栈**上，这是有意义的，因为寄存器值属于该线程。如果陷阱导致切换到不同的线程，那这一点就显得尤为重要——在这种情况下，陷阱将实际返回到新线程的栈上，将被中断线程保存的寄存器安全地保存在其栈上。

`Kernelvec`在保存寄存器后跳转到`kerneltrap`(***kernel/trap.c***:134)。`kerneltrap`为两种类型的陷阱做好了准备：设备中断和异常。

- 调用`devintr`(***kernel/trap.c***:177)来检查和处理前者
- 如果陷阱不是设备中断，则必定是一个异常，内核中的异常将是一个致命的错误；内核调用`panic`并停止执行

如果由于计时器中断而调用了`kerneltrap`，并且一个进程的内核线程正在运行（而不是调度程序线程），`kerneltrap`会调用`yield`，给其他线程一个运行的机会。在某个时刻，其中一个线程会让步，让我们的线程和它的`kerneltrap`再次恢复。

当`kerneltrap`的工作完成后，它需要返回到任何被陷阱中断的代码。

- 因为一个`yield`可能已经破坏了保存的`sepc`和在`sstatus`中保存的前一个状态模式，因此`kerneltrap`在启动时保存它们。
- 它现在恢复这些控制寄存器并返回到`kernelvec`(***kernel/kernelvec.S***:48)。`kernelvec`从栈中弹出保存的寄存器并执行`sret`，将`sepc`复制到`pc`并恢复中断的内核代码。
- 当CPU从用户空间进入内核时，xv6将CPU的`stvec`设置为`kernelvec`；您可以在`usertrap`(***kernel/trap.c***:29)中看到这一点。

内核执行时有一个时间窗口，但`stvec`设置为`uservec`，在该窗口中禁用设备中断至关重要。幸运的是，RISC-V总是在开始设置陷阱时禁用中断，xv6在设置`stvec`之前不会再次启用中断。



## 页面错误异常

Xv6对异常的响应相当无趣: 

- 如果用户空间中发生异常，内核将终止故障进程。
- 如果内核中发生异常，则内核会崩溃。

真正的操作系统通常以更有趣的方式做出反应。

- 例如，许多内核使用页面错误来实现写时拷贝版本的`fork`——*copy on write (COW) fork*。
  - xv6的`fork`通过调用`uvmcopy`(***kernel/vm.c***:309) 为子级分配物理内存，并将父级的内存复制到其中，使子级具有与父级相同的内存内容。
    - 如果父子进程可以共享父级的物理内存，则效率会更高
    - 然而武断地实现这种方法是行不通的，因为它会导致父级和子级通过对共享栈和堆的写入来中断彼此的执行
  - 由页面错误驱动的*COW fork*可以使父级和子级安全地共享物理内存
    - 当CPU无法将虚拟地址转换为物理地址时，CPU会生成页面错误异常，Risc-v有三种不同的页面错误
      -  加载页面错误 (当加载指令无法转换其虚拟地址时)
      - 存储页面错误 (当存储指令无法转换其虚拟地址时)
      - 指令页面错误 (当指令的地址无法转换时)
      - `scause`寄存器中的值指示页面错误的类型，`stval`寄存器包含无法翻译的地址
    - COW fork中的基本计划是让父子最初共享所有物理页面，但将它们映射为只读
      - 当子级或父级执行存储指令时，risc-v CPU引发页面错误异常。为了响应此异常，内核复制了包含错误地址的页面
      - 在子级的地址空间中映射一个权限为读/写的副本
      - 在父级的地址空间中映射另一个权限为读/写的副本
    - COW策略对`fork`很有效，
      - 因为通常子进程会在`fork`之后立即调用`exec`，用新的地址空间替换其地址空间。在这种常见情况下，子级只会触发很少的页面错误，内核可以避免拷贝父进程内存完整的副本。
      - 此外，*COW fork*是透明的: 无需对应用程序进行任何修改即可使其受益。
- 另一个广泛使用的特性叫做惰性分配——*lazy allocation。*
  - 首先，当应用程序调用`sbrk`时，内核增加地址空间，但在页表中将新地址标记为无效
  - 其次，对于包含于其中的地址的页面错误，内核分配物理内存并将其映射到页表中。
  - 内核仅在应用程序实际使用它时才分配内存。像COW fork一样，内核可以对应用程序透明地实现此功能。



# 实验

## Task1

回答问题，跳过



## Task2

### 要求

回溯(Backtrace)通常对于调试很有用：它是一个存放于栈上用于指示错误发生位置的函数调用列表。

在***kernel/printf.c***中实现名为`backtrace()`的函数。在`sys_sleep`中插入一个对此函数的调用，然后运行`bttest`，它将会调用`sys_sleep`。你的输出应该如下所示：

```bash
backtrace:
0x0000000080002cda
0x0000000080002bb6
0x0000000080002898
```

### 提示

- 在***kernel/defs.h\***中添加`backtrace`的原型，那样你就能在`sys_sleep`中引用`backtrace`
- GCC编译器将当前正在执行的函数的帧指针保存在`s0`寄存器，将下面的函数添加到***kernel/riscv.h\***

```c
static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}
```

 并在`backtrace`中调用此函数来读取当前的帧指针。这个函数使用[内联汇编](https://gcc.gnu.org/onlinedocs/gcc/Using-Assembly-Language-with-C.html)来读取`s0`

- 这个[课堂笔记](https://pdos.csail.mit.edu/6.828/2020/lec/l-riscv-slides.pdf)中有张栈帧布局图。注意返回地址位于栈帧帧指针的固定偏移(-8)位置，并且保存的帧指针位于帧指针的固定偏移(-16)位置

![img](assets/p2.png)

- XV6在内核中以页面对齐的地址为每个栈分配一个页面。你可以通过`PGROUNDDOWN(fp)`和`PGROUNDUP(fp)`（参见***kernel/riscv.h***）来计算栈页面的顶部和底部地址。这些数字对于`backtrace`终止循环是有帮助的。

一旦你的`backtrace`能够运行，就在***kernel/printf.c***的`panic`中调用它，那样你就可以在`panic`发生时看到内核的`backtrace`。



### 实现

**backtrace函数**

- 先来理清一些逻辑

  - fp 是当前的帧指针，可以通过提示中给出的函数 `r_sp()` 获得
  - 在xv6（以及大多数x86结构）中，栈的地址从高往低增长，这意味着**栈顶**是低地址
  - 在这里，我们要获取所有栈中函数的返回地址，这意味着我们不关心栈顶，而关心栈底，所以应该用 `PGROUNDUP` 将地址向上对齐到页边界

- 在以上的逻辑上，代码的逻辑是

  - `fp-8` 获得当前返回地址，然后输出
  - 将 `fp` 置为 `fp-16` 对应的上一个 fp 的地址，从而到达之前的 栈帧
  - 直到到达栈底 `fp==PGROUNDUP(fp)`，退出循环

- ```c
  void
  backtrace(void)
  {
    // 获取当前fp
    uint64 fp = r_fp();
    while (fp != PGROUNDUP(fp)) {
      // 获取对应地址的方式，先转换成指向该地址的指针，然后解引用
      uint64 ra = *(uint64*)(fp - 8);
      printf("%p\n", ra);
      fp = *(uint64*)(fp - 16);
    }
  }
  ```

**修改sys_sleep**

```c
uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  // 加入backtrace
  printf("backtrace:\n");
  backtrace();

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}
```



## Task3

### 要求

在这个练习中你将向XV6添加一个特性，在进程使用CPU的时间内，XV6定期向进程发出警报。这对于那些希望限制CPU时间消耗的受计算限制的进程，或者对于那些计算的同时执行某些周期性操作的进程可能很有用。更普遍的来说，你将实现用户级中断/故障处理程序的一种初级形式。例如，你可以在应用程序中使用类似的一些东西处理页面故障。如果你的解决方案通过了`alarmtest`和`usertests`就是正确的。

你应当添加一个新的`sigalarm(interval, handler)`系统调用，如果一个程序调用了`sigalarm(n, fn)`，那么每当程序消耗了CPU时间达到n个“滴答”，内核应当使应用程序函数`fn`被调用。当`fn`返回时，应用应当在它离开的地方恢复执行。在XV6中，一个滴答是一段相当任意的时间单元，取决于硬件计时器生成中断的频率。如果一个程序调用了`sigalarm(0, 0)`，系统应当停止生成周期性的报警调用。

你将在XV6的存储库中找到名为***user/alarmtest.c***的文件。将其添加到***Makefile***。注意：你必须添加了`sigalarm`和`sigreturn`系统调用后才能正确编译（往下看）。

`alarmtest`在`test0`中调用了`sigalarm(2, periodic)`来要求内核每隔两个滴答强制调用`periodic()`，然后旋转一段时间。你可以在***user/alarmtest.asm\***中看到`alarmtest`的汇编代码，这或许会便于调试。当`alarmtest`产生如下输出并且`usertests`也能正常运行时，你的方案就是正确的：

### 步骤一

#### 提示

首先修改内核以跳转到用户空间中的报警处理程序，这将导致`test0`打印“alarm!”。不用担心输出“alarm!”之后会发生什么；如果您的程序在打印“alarm！”后崩溃，对于目前来说也是正常的。以下是一些**提示**：

- 您需要修改***Makefile***以使***alarmtest.c***被编译为xv6用户程序。
- 放入***user/user.h***的正确声明是：

```c
int sigalarm(int ticks, void (*handler)());
int sigreturn(void);
```

- 更新***user/usys.pl***（此文件生成***user/usys.S***）、***kernel/syscall.h***和***kernel/syscall.c***以允许`alarmtest`调用`sigalarm`和`sigreturn`系统调用。
- 目前来说，你的`sys_sigreturn`系统调用返回应该是零。
- 你的`sys_sigalarm()`应该将报警间隔和指向处理程序函数的指针存储在`struct proc`的新字段中（位于***kernel/proc.h***）。
- 你也需要在`struct proc`新增一个新字段。用于跟踪自上一次调用（或直到下一次调用）到进程的报警处理程序间经历了多少滴答；您可以在***proc.c***的`allocproc()`中初始化`proc`字段。
- 每一个滴答声，硬件时钟就会强制一个中断，这个中断在***kernel/trap.c***中的`usertrap()`中处理。
- 如果产生了计时器中断，您只想操纵进程的报警滴答；你需要写类似下面的代码

```c
if(which_dev == 2) ...
```

- 仅当进程有未完成的计时器时才调用报警函数。请注意，用户报警函数的地址可能是0（例如，在***user/alarmtest.asm***中，`periodic`位于地址0）。
- 您需要修改`usertrap()`，以便当进程的报警间隔期满时，用户进程执行处理程序函数。当RISC-V上的陷阱返回到用户空间时，什么决定了用户空间代码恢复执行的指令地址？
- 如果您告诉qemu只使用一个CPU，那么使用gdb查看陷阱会更容易，这可以通过运行

```bash
make CPUS=1 qemu-gdb
```

- 如果`alarmtest`打印“alarm!”，则您已成功。

#### 实现

为 `struct proc` 增加几个字段

- ```c
    int alarm_interval;          // Alarm interval
    void (*alarm_handler)();     // Alarm handler
    int alarm_ticks;             // Alarm ticks
  ```

修改 `allocproc` 和 `freeproc`

- 因为 proc 结构增加了字段，所以要修改初始化和恢复的代码

- ```c
  static struct proc*
  allocproc(void)
  {
    struct proc *p;
    // ...
  found:
    // ...
    p->context.ra = (uint64)forkret;
    p->context.sp = p->kstack + PGSIZE;
  
    // Set up alarm
    p->alarm_ticks = 0;
    p->alarm_handler = 0;
    p->alarm_interval = 0;
  
    return p;
  }
  ```

- ```c
  static void
  freeproc(struct proc *p)
  {
    // ...
    // free alarm
    p->alarm_ticks = 0;
    p->alarm_handler = 0;
    p->alarm_interval = 0;
  }
  ```

修改 `usertrap` 

- 按照提示，在 `if(which_dev == 2)` 的函数体中修改

- ```c
   // give up the CPU if this is a timer interrupt.
    if(which_dev == 2) {
      // 增加一个tick，如果达到了需要调用处理函数的间隔，则处理
      if (++p->alarm_ticks == p->alarm_interval) {
        // 重置计时
        p->alarm_ticks = 0;
        // 将陷阱帧中的程序计数器修改为sigalarm传入的处理函数
        p->trapframe->epc = (uint64)p->alarm_handler;
      }
      yield();
    }
  ```

- 注意，上面修改陷阱帧的左右赋值不要反了，否则就调用不到处理函数了（别问为什么我知道）

实现 `sigalarm` 和 `sigreturn` 系统调用

- ```c
  uint64 
  sys_sigalarm(void)
  {
    // 读入命令行参数
    if (argint(0, &myproc()->alarm_interval) < 0 || argaddr(1, (uint64*)&myproc()->alarm_handler) < 0)
      return -1;
    return 0;
  }
  
  uint64
  sys_sigreturn(void)
  {
    // 在这个阶段中，只需要返回0
    return 0;
  }
  ```



### 步骤二

#### 提示

`alarmtest`打印“alarm!”后，很可能会在`test0`或`test1`中崩溃，或者`alarmtest`（最后）打印“test1 failed”，或者`alarmtest`未打印“test1 passed”就退出。要解决此问题，必须确保完成报警处理程序后返回到用户程序最初被计时器中断的指令执行。必须确保寄存器内容恢复到中断时的值，以便用户程序在报警后可以不受干扰地继续运行。最后，您应该在每次报警计数器关闭后“重新配置”它，以便周期性地调用处理程序。

作为一个起始点，我们为您做了一个设计决策：用户报警处理程序需要在完成后调用`sigreturn`系统调用。请查看***alarmtest.c***中的`periodic`作为示例。这意味着您可以将代码添加到`usertrap`和`sys_sigreturn`中，这两个代码协同工作，以使用户进程在处理完警报后正确恢复。

**提示：**

- 您的解决方案将要求您保存和恢复寄存器——您需要保存和恢复哪些寄存器才能正确恢复中断的代码？(提示：会有很多）
- 当计时器关闭时，让`usertrap`在`struct proc`中保存足够的状态，以使`sigreturn`可以正确返回中断的用户代码。
- 防止对处理程序的重复调用——如果处理程序还没有返回，内核就不应该再次调用它。`test2`测试这个。
- 一旦通过`test0`、`test1`和`test2`，就运行`usertests`以确保没有破坏内核的任何其他部分。



#### 实现

在 `struct proc` 中再增加两个字段，一个用于记录此时是否有处理函数已经在运行，另一个用于保存用户寄存器以便函数返回后继续执行中断的用户代码

- 在Cahpetr4的‘从用户空间陷入’处可知，在从用户空间进入内核之前，进程的寄存器都保存在 `trapframe` 中

- 因此再定义一个 `stryct trapframe` 来保存这些寄存器

- ```c
    int alarm_flag;              // Alarm flag
    struct trapframe *alarm_trap_frame; // User trap frame
  ```

修改 `allocproc` 和 `freeproc`

- ```c
    // Set up alarm
    p->alarm_ticks = 0;
    p->alarm_handler = 0;
    p->alarm_interval = 0;
    // 步骤二新增
    p->alarm_flag = 0;
    p->alarm_trap_frame = (struct trapframe *)kalloc();
  ```

- ```c
    // free alarm
    p->alarm_ticks = 0;
    p->alarm_handler = 0;
    p->alarm_interval = 0;
    p->alarm_flag = 0;
    // 步骤二新增
    if(p->alarm_trap_frame)
      kfree((void*)p->alarm_trap_frame);
    p->alarm_trap_frame = 0;
  ```

修改 `sigreturn`

- 这个函数主要做两件事
  - 恢复寄存器（陷阱帧）
  - 重置标志位，表示当前处理函数已经结束，可以继续调用新处理函数

- ```c
  uint64
  sys_sigreturn(void)
  {
    memmove(myproc()->trapframe, myproc()->alarm_trap_frame, sizeof(struct trapframe));
    myproc()->alarm_flag = 0;
    return 0;
  }
  ```

修改 `usertrap`

- 加入对 `alarm_interval` 的判断，确保其大于0

- 加入对标记位的判断

- 加入陷阱帧的保存

- ```c
    // give up the CPU if this is a timer interrupt.
    if(which_dev == 2) {
      if (p->alarm_interval != 0 && ++p->alarm_ticks >= p->alarm_interval) {
        if (p->alarm_flag == 0) {
          p->alarm_flag = 1;
          p->alarm_ticks -= p->alarm_interval;
          // 保存一定要在修改当前陷阱帧的epc之前，否则用户代码的pc就被覆盖掉了
          memmove(p->alarm_trap_frame, p->trapframe, sizeof(struct trapframe));
          p->trapframe->epc = (uint64)p->alarm_handler;
        }
      }
      yield();
    }
  ```

**注意，在保存和恢复陷阱帧的时候都用`memmove`**

- 最开始，我直接用 = 赋值，但test1无法通过，报错 `test1 failed: foo() executed fewer times than it was called`

  - 查看 `alarmtest` 的代码，有一段提示

    - ```c
          // once possible source of errors is that the handler may
          // return somewhere other than where the timer interrupt
          // occurred; another is that that registers may not be
          // restored correctly, causing i or j or the address ofj
          // to get an incorrect value.
      ```

- 分析了 `usertrap` 的逻辑，我认为其能够正确地在timer interrupt时调用处理函数，所以我认为原因在第二点：没有正确恢复寄存器
- 经过查阅发现，`trapframe` 是指针，所以如果直接赋值，其实并没有保存寄存器，仅仅是将指针指向了同一个位置，所以之后 `sigreturn` 的时候，实际上还是用了当前的这些寄存器而非之前需要保存的那些
- `memmove` 可以正确赋值



# 结果

![image-20240616201022163](assets/image-20240616201022163.png)

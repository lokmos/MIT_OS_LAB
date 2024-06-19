# Chapter7 调度

## 7.1 多路复用

Xv6通过在两种情况下将每个CPU从一个进程切换到另一个进程来实现多路复用（Multiplexing）

1. 当进程等待设备或管道I/O完成，或等待子进程退出，或在`sleep`系统调用中等待时，xv6使用睡眠（sleep）和唤醒（wakeup）机制切换
2. xv6周期性地强制切换以处理长时间计算而不睡眠的进程



## 7.2 代码：上下文切换

![img](assets/p1.png)

从一个用户进程（旧进程）切换到另一个用户进程（新进程）所涉及的步骤：

- 一个到旧进程内核线程的用户-内核转换（系统调用或中断），
- 一个到当前CPU调度程序线程的上下文切换，
- 一个到新进程内核线程的上下文切换，
- 以及一个返回到用户级进程的陷阱。

调度程序在旧进程的内核栈上执行是不安全的：

- 其他一些核心可能会唤醒进程并运行它，而在两个不同的核心上使用同一个栈将是一场灾难，
- 因此xv6调度程序在每个CPU上都有一个专用线程（保存寄存器和栈）。

从一个线程切换到另一个线程需要保存旧线程的CPU寄存器，并恢复新线程先前保存的寄存器；**栈指针和程序计数器被保存和恢复的事实意味着CPU将切换栈和执行中的代码**

函数`swtch`为内核线程切换执行保存和恢复操作。

- `swtch`对线程没有直接的了解；它只是保存和恢复寄存器集，称为上下文（contexts）。

- 当某个进程要放弃CPU时，该进程的内核线程调用`swtch`来保存自己的上下文并返回到调度程序的上下文。

- 每个上下文都包含在一个`struct context`（***kernel/proc.h***:2）中，这个结构体本身包含在一个进程的`struct proc`或一个CPU的`struct cpu`中

  - ```c
    struct context {
      uint64 ra;
      uint64 sp;
    
      // callee-saved
      uint64 s0;
      uint64 s1;
      uint64 s2;
      uint64 s3;
      uint64 s4;
      uint64 s5;
      uint64 s6;
      uint64 s7;
      uint64 s8;
      uint64 s9;
      uint64 s10;
      uint64 s11;
    };
    ```

- `Swtch`接受两个参数：`struct context *old`和`struct context *new`。它将当前寄存器保存在`old`中，从`new`中加载寄存器，然后返回

中断结束时的一种可能性是`usertrap`调用了`yield`。依次地：`Yield`调用`sched`，`sched`调用`swtch`将当前上下文保存在`p->context`中，并切换到先前保存在`cpu->context`（***kernel/proc.c***:517）中的调度程序上下文。

`Swtch`（***kernel/swtch.S***:3）只保存被调用方保存的寄存器（callee-saved registers）；调用方保存的寄存器（caller-saved registers）通过调用C代码保存在栈上（如果需要）。

- `Swtch`知道`struct context`中每个寄存器字段的偏移量

- ```c
          sd ra, 0(a0)
          sd sp, 8(a0)
          sd s0, 16(a0)
          sd s1, 24(a0)
          sd s2, 32(a0)
          sd s3, 40(a0)
          sd s4, 48(a0)
          sd s5, 56(a0)
          sd s6, 64(a0)
          sd s7, 72(a0)
          sd s8, 80(a0)
          sd s9, 88(a0)
          sd s10, 96(a0)
          sd s11, 104(a0)
  ```

- 不保存程序计数器，但保存`ra`寄存器，该寄存器保存调用`swtch`的返回地址

- `swtch`从新进程的上下文中恢复寄存器，该上下文保存前一个`swtch`保存的寄存器值

-         ld ra, 0(a1)
          ld sp, 8(a1)
          ld s0, 16(a1)
          ld s1, 24(a1)
          ld s2, 32(a1)
          ld s3, 40(a1)
          ld s4, 48(a1)
          ld s5, 56(a1)
          ld s6, 64(a1)
          ld s7, 72(a1)
          ld s8, 80(a1)
          ld s9, 88(a1)
          ld s10, 96(a1)
          ld s11, 104(a1)

- 当`swtch`返回时，它返回到由`ra`寄存器指定的指令，即新线程以前调用`swtch`的指令。另外，它在新线程的栈上返回

**一个例子**

**以`cc`切换到`ls`为例，且`ls`此前运行过**

1. XV6将`cc`程序的内核线程的内核寄存器保存在一个`context`对象中

2. 因为要切换到`ls`程序的内核线程，那么`ls` 程序现在的状态必然是`RUNABLE` ，表明`ls`程序之前运行了一半。这同时也意味着：

   a. `ls`程序的用户空间状态已经保存在了对应的trapframe中

   b. `ls`程序的内核线程对应的内核寄存器已经保存在对应的`context`对象中

   所以接下来，XV6会恢复`ls`程序的内核线程的`context`对象，也就是恢复内核线程的寄存器。

3. 之后`ls`会继续在它的内核线程栈上，完成它的中断处理程序

4. 恢复`ls`程序的trapframe中的用户进程状态，返回到用户空间的`ls`程序中

5. 最后恢复执行`ls`



## 7.3 代码：调度

调度器（scheduler）以每个CPU上一个特殊线程的形式存在，每个线程都运行`scheduler`函数。此函数负责**选择下一个要运行的进程**

- 想要放弃CPU的进程必须先获得自己的进程锁`p->lock`，并释放它持有的任何其他锁，更新自己的状态（`p->state`），然后调用`sched`

  - `yield` 遵循这个约定，`sleep`和`exit`也遵循这个约定

    - ```c
      void
      yield(void)
      {
        struct proc *p = myproc();
        acquire(&p->lock);
        p->state = RUNNABLE;
        sched();
        release(&p->lock);
      }
      ```

  - `Sched`对这些条件再次进行检查（***kernel/proc.c***:489），并检查这些条件的隐含条件：由于锁被持有，中断应该被禁用

    - ```c
      void
      sched(void)
      {
        int intena;
        struct proc *p = myproc();
      
        if(!holding(&p->lock))
          panic("sched p->lock");
        if(mycpu()->noff != 1)
          panic("sched locks");
        if(p->state == RUNNING)
          panic("sched running");
        if(intr_get())
          panic("sched interruptible");
      
        intena = mycpu()->intena;
        swtch(&p->context, &mycpu()->context);
        mycpu()->intena = intena;
      }
      ```

    - 最后，`sched`调用`swtch`将当前上下文保存在`p->context`中，并切换到`cpu->context`中的调度程序上下文

  - `Swtch`在调度程序的栈上返回，就像是`scheduler`的`swtch`返回一样

上下文切换打破了锁的约定

- 通常，获取锁的线程还负责释放锁，这使得对正确性进行推理更加容易
- xv6在对`swtch`的调用中持有`p->lock`：`swtch`的调用者必须已经持有了锁，并且锁的控制权传递给切换到的代码
  - 因为`p->lock`保护进程`state`和`context`字段上的不变量，而这些不变量在`swtch`中执行时不成立
  - 如果在`swtch`期间没有保持`p->lock`，可能会出现一个问题：在`yield`将其状态设置为`RUNNABLE`之后，但在`swtch`使其停止使用自己的内核栈之前，另一个CPU可能会决定运行该进程。结果将是两个CPU在同一栈上运行，这不可能是正确的
- 其中一个不变量是：如果进程是`RUNNING`状态，计时器中断的`yield`必须能够安全地从进程中切换出去
  - 这意味着CPU寄存器必须保存进程的寄存器值（即`swtch`没有将它们移动到`context`中），并且`c->proc`必须指向进程
- 另一个不变量是：如果进程是`RUNNABLE`状态，空闲CPU的调度程序必须安全地运行它
  - 这意味着`p->context`必须保存进程的寄存器（即，它们实际上不在实际寄存器中），没有CPU在进程的内核栈上执行，并且没有CPU的`c->proc`引用进程
- 在保持`p->lock`时，这些属性通常不成立
- 维护上述不变量是xv6经常在一个线程中获取`p->lock`并在另一个线程中释放它的原因
  - 在`yield`中获取并在`scheduler`中释放
    - 一旦`yield`开始修改一个`RUNNING`进程的状态为`RUNNABLE`，锁必须保持被持有状态，直到不变量恢复
      - 最早的正确释放点是`scheduler`（在其自身栈上运行）清除`c->proc`之后
  - 一旦`scheduler`开始将`RUNNABLE`进程转换为`RUNNING`，在内核线程完全运行之前（在`swtch`之后，例如在`yield`中）绝不能释放锁
- `p->lock`还保护其他东西：`exit`和`wait`之间的相互作用，避免丢失`wakeup`的机制（参见第7.5节），以及避免一个进程退出和其他进程读写其状态之间的争用（例如，`exit`系统调用查看`p->pid`并设置`p->killed`。

内核线程总是在`sched`中放弃其CPU，并总是切换到调度程序中的同一位置，而调度程序（几乎）总是切换到以前调用`sched`的某个内核线程

- 在两个线程之间进行这种样式化切换的过程有时被称为协程（coroutines）；在本例中，`sched`和`scheduler`是彼此的协同程序。
- 存在一种情况使得调度程序对`swtch`的调用没有以`sched`结束
  - 一个新进程第一次被调度时，它从`forkret`（***kernel/proc.c***:512）开始

`scheduler`（***kernel/proc.c***:444）运行一个简单的循环：找到要运行的进程，运行它直到它让步，然后重复循环

- `scheduler`在进程表上循环查找可运行的进程，该进程具有`p->state == RUNNABLE`。
- 一旦找到一个进程，它将设置CPU当前进程变量`c->proc`，将该进程标记为`RUNINING`，然后调用`swtch`开始运行它



## 7.4 代码：mycpu和myproc

Xv6为每个CPU维护一个`struct cpu`，它记录当前在该CPU上运行的进程（如果有的话），为CPU的调度线程保存寄存器，以及管理中断禁用所需的嵌套自旋锁的计数。

- ```c
  // Return the current struct proc *, or zero if none.
  struct proc*
  myproc(void)
  {
    push_off();
    struct cpu *c = mycpu();
    struct proc *p = c->proc;
    pop_off();
    return p;
  }
  ```

- 函数`mycpu`返回一个指向当前CPU的`struct cpu`的指针

  - RISC-V给它的CPU编号，给每个CPU一个`hartid`。Xv6确保每个CPU的`hartid`在内核中存储在该CPU的`tp`寄存器中。
  - 这允许`mycpu`使用`tp`对一个cpu结构体数组（即`cpus`数组）进行索引

- 确保 `tp` 始终保存 `hartid`

  - `mstart`在CPU启动次序的早期设置`tp`寄存器，此时仍处于机器模式
  - 因为用户进程可能会修改`tp`，`usertrapret`在蹦床页面（trampoline page）中保存`tp`
  - 最后，`uservec`在从用户空间进入内核时恢复保存的`tp`
  - 编译器保证永远不会使用`tp`寄存器。如果RISC-V允许xv6`直接读取当前hartid会更方便，`但这只允许在机器模式下，而不允许在管理模式下

- `cpuid`和`mycpu`的返回值很脆弱：如果定时器中断并导致线程让步（yield），然后移动到另一个CPU，以前返回的值将不再正确。

  - 为了避免这个问题，xv6要求调用者禁用中断，并且只有在使用完返回的`struct cpu`后才重新启用。

函数`myproc` 返回当前CPU上运行进程`struct proc`的指针。

- `myproc`禁用中断，调用`mycpu`，从`struct cpu`中取出当前进程指针（`c->proc`），然后启用中断。
- 即使启用中断，`myproc`的返回值也可以安全使用：如果计时器中断将调用进程移动到另一个CPU，其`struct proc`指针不会改变。



## 7.5 sleep和wakeup

Xv6使用了一种称为`sleep`和`wakeup`的方法，它允许一个进程在等待事件时休眠，而另一个进程在事件发生后将其唤醒。

- 睡眠和唤醒通常被称为序列协调（sequence coordination）或条件同步机制（conditional synchronization mechanisms）。

**生产者消费者问题**

考虑一个称为信号量（semaphore）的同步机制，它可以协调生产者和消费者。信号量维护一个计数并提供两个操作。

- “V”操作（对于生产者）增加计数。
- “P”操作（对于使用者）等待计数为非零，然后递减并返回。

如果只有一个生产者线程和一个消费者线程，并且它们在不同的CPU上执行，并且编译器没有进行过积极的优化，那么此实现将是正确的：

```c
struct semaphore {
    struct spinlock lock;
    int count;
};

void V(struct semaphore* s) {
    acquire(&s->lock);
    s->count += 1;
    release(&s->lock);
}

void P(struct semaphore* s) {
    while (s->count == 0)
        ;
    acquire(&s->lock);
    s->count -= 1;
    release(&s->lock);
}
```

上面的实现代价昂贵。如果生产者很少采取行动，消费者将把大部分时间花在`while`循环中(反复轮询`s->count`繁忙等待)，希望得到非零计数。

想象一对调用，`sleep`和`wakeup`，工作流程如下。

- `Sleep(chan)`在任意值`chan`上睡眠，称为等待通道（wait channel）。`Sleep`将调用进程置于睡眠状态，释放CPU用于其他工作
- `Wakeup(chan)`唤醒所有在`chan`上睡眠的进程（如果有），使其`sleep`调用返回。如果没有进程在`chan`上等待，则`wakeup`不执行任何操作

```c
void V(struct semaphore* s) {
    acquire(&s->lock);
    s->count += 1;
    wakeup(s);  // !pay attention
    release(&s->lock);
}

void P(struct semaphore* s) {
    while (s->count == 0)
        sleep(s);  // !pay attention
    acquire(&s->lock);
    s->count -= 1;
    release(&s->lock);
}
```

- `P`现在放弃CPU而不是自旋，这很好。然而，事实证明，使用此接口设计`sleep`和`wakeup`而不遭受所谓的丢失唤醒（lost wake-up）问题并非易事。
  - 假设`P`在第9行发现`s->count==0`。
  - 当`P`在第9行和第10行之间时，`V`在另一个CPU上运行：它将`s->count`更改为非零，并调用`wakeup`，这样就不会发现进程处于休眠状态，因此不会执行任何操作。
  - 现在P继续在第10行执行：它调用`sleep`并进入睡眠。这会导致一个问题：`P`正在休眠，等待调用`V`，而`V`已经被调用。除非我们运气好，生产者再次呼叫`V`，否则消费者将永远等待，即使`count`为非零。

这个问题的根源是`V`在错误的时刻运行，违反了`P`仅在`s->count==0`时才休眠的不变量。保护不变量的一种不正确的方法是将锁的获取（下面以黄色突出显示）移动到`P`中，以便其检查`count`和调用`sleep`是原子的：

```c
void V(struct semaphore* s) {
    acquire(&s->lock);
    s->count += 1;
    wakeup(s);
    release(&s->lock);
}

void P(struct semaphore* s) {
    acquire(&s->lock);  // !pay attention
    while (s->count == 0)
        sleep(s);
    s->count -= 1;
    release(&s->lock);
}
```

- 人们可能希望这个版本的`P`能够避免丢失唤醒，因为锁阻止`V`在第10行和第11行之间执行。它确实这样做了，但它会导致死锁：`P`在睡眠时持有锁，因此`V`将永远阻塞等待锁。

我们将通过更改`sleep`的接口来修复前面的方案：调用方必须将条件锁（condition lock）传递给sleep，以便在调用进程被标记为asleep并在睡眠通道上等待后`sleep`可以释放锁。如果有一个并发的`V`操作，锁将强制它在`P`将自己置于睡眠状态前一直等待，因此`wakeup`将找到睡眠的消费者并将其唤醒。一旦消费者再次醒来，`sleep`会在返回前重新获得锁。我们新的正确的`sleep/wakeup`方案可用如下（更改以黄色突出显示）：

```c
void V(struct semaphore* s) {
    acquire(&s->lock);
    s->count += 1;
    wakeup(s);
    release(&s->lock);
}

void P(struct semaphore* s) {
    acquire(&s->lock);

    while (s->count == 0)
        sleep(s, &s->lock);  // !pay attention
    s->count -= 1;
    release(&s->lock);
}
```

- `P`持有`s->lock`的事实阻止`V`在`P`检查`s->count`和调用`sleep`之间试图唤醒它。然而请注意，我们需要`sleep`释放`s->lock`并使消费者进程进入睡眠状态的操作是原子的。



## 7.6 代码：sleep和wakeup

基本思想是让`sleep`将当前进程标记为`SLEEPING`，然后调用`sched`释放CPU；`wakeup`查找在给定等待通道上休眠的进程，并将其标记为`RUNNABLE`。

```c
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}
```

- `sleep`获得`p->lock`。要进入睡眠的进程现在同时持有`p->lock`和`lk`。
  - 在调用者（示例中为`P`）中持有`lk`是必要的：它确保没有其他进程（在示例中指一个运行的`V`）可以启动`wakeup(chan)`调用
  - 既然`sleep`持有`p->lock`，那么释放`lk`是安全的：其他进程可能会启动对`wakeup(chan)`的调用，但是`wakeup`将等待获取`p->lock`，因此将等待`sleep`把进程置于睡眠状态的完成，以防止`wakeup`错过`sleep`。
- 还有一个小问题：如果`lk`和`p->lock`是同一个锁，那么如果`sleep`试图获取`p->lock`就会自身死锁。
  - 但是，如果调用`sleep`的进程已经持有`p->lock`，那么它不需要做更多的事情来避免错过并发的`wakeup`。
  - 当`wait`持有`p->lock`调用`sleep`时，就会出现这种情况。
- 由于`sleep`只持有`p->lock`而无其他，它可以通过记录睡眠通道、将进程状态更改为`SLEEPING`并调用`sched`将进程置于睡眠状态。

```c
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}
```

- 在某个时刻，一个进程将获取条件锁(lk)，设置睡眠者正在等待的条件，并调用`wakeup(chan)`。
- `wakeup`遍历进程表。它获取它所检查的每个进程的`p->lock`，这既是因为它可能会操纵该进程的状态，也是因为`p->lock`确保`sleep`和`wakeup`不会彼此错过
- 当`wakeup`发现一个`SLEEPING`的进程且`chan`相匹配时，它会将该进程的状态更改为`RUNNABLE`。调度器下次运行时，将看到进程已准备好运行
- 注：严格地说，`wakeup`只需跟在`acquire`之后就足够了（也就是说，可以在`release`之后调用`wakeup`）



## 7.7 代码：Pipes

每个管道都由一个`struct pipe`表示，其中包含一个锁`lock`和一个数据缓冲区`data`。字段`nread`和`nwrite`统计从缓冲区读取和写入缓冲区的总字节数。缓冲区是环形的：在`buf[PIPESIZE-1]`之后写入的下一个字节是`buf[0]`。而计数不是环形。此约定允许实现区分完整缓冲区（`nwrite==nread+PIPESIZE`）和空缓冲区（`nwrite==nread`），但这意味着对缓冲区的索引必须使用`buf[nread%PIPESIZE]`，而不仅仅是`buf[nread]`（对于`nwrite`也是如此）。

```c
int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){ //DOC: pipewrite-full
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      char ch;
      if(copyin(pr->pagetable, &ch, addr + i, 1) == -1)
        break;
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      i++;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);

  return i;
}
```

```c
int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    if(killed(pr)){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread++ % PIPESIZE];
    if(copyout(pr->pagetable, addr + i, &ch, 1) == -1)
      break;
  }
  wakeup(&pi->nwrite);  //DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}
```

假设对`piperead`和`pipewrite`的调用同时发生在两个不同的CPU上

- `Pipewrite`从获取管道锁开始，它保护计数、数据及其相关不变量。
- `Piperead`然后也尝试获取锁，但无法实现。它在`acquire`中旋转等待锁
- 当`piperead`等待时，`pipewrite`遍历被写入的字节（`addr[0..n-1]`），依次将每个字节添加到管道中
  - 在这个循环中缓冲区可能会被填满
    - 在这种情况下，`pipewrite`调用`wakeup`来提醒所有处于睡眠状态的读进程缓冲区中有数据等待，然后在`&pi->nwrite`上睡眠，等待读进程从缓冲区中取出一些字节
    - 作为使`pipewrite`进程进入睡眠状态的一部分，`Sleep`释放`pi->lock`
- 现在`pi->lock`可用，`piperead`设法获取它并进入其临界区域：
  - 进入`for`循环，从管道中复制数据并根据复制的字节数增加`nread`。
  - 那些读出的字节就可供写入，因此`piperead`调用`wakeup`返回之前唤醒所有休眠的写进程。
- `Wakeup`寻找一个在`&pi->nwrite`上休眠的进程，该进程正在运行`pipewrite`，但在缓冲区填满时停止。它将该进程标记为`RUNNABLE`。



## 7.8 代码：wait、exit和kill



# 实验

## Task1

### 要求

在本练习中，您将为用户级线程系统设计上下文切换机制，然后实现它。为了让您开始，您的xv6有两个文件：***user/uthread.c***和***user/uthread_switch.S***，以及一个规则：运行在***Makefile***中以构建`uthread`程序。***uthread.c***包含大多数用户级线程包，以及三个简单测试线程的代码。线程包缺少一些用于创建线程和在线程之间切换的代码。

您的工作是提出一个创建线程和保存/恢复寄存器以在线程之间切换的计划，并实现该计划。完成后，`make grade`应该表明您的解决方案通过了`uthread`测试。

### 提示

需要将代码添加到***user/uthread.c***中的`thread_create()`和`thread_schedule()`，以及***user/uthread_switch.S***中的`thread_switch`。一个目标是确保当`thread_schedule()`第一次运行给定线程时，该线程在自己的栈上执行传递给`thread_create()`的函数。另一个目标是确保`thread_switch`保存被切换线程的寄存器，恢复切换到线程的寄存器，并返回到后一个线程指令中最后停止的点。您必须决定保存/恢复寄存器的位置；修改`struct thread`以保存寄存器是一个很好的计划。您需要在`thread_schedule`中添加对`thread_switch`的调用；您可以将需要的任何参数传递给`thread_switch`，但目的是将线程从`t`切换到`next_thread`。

### 实现

这个task的目标就是实现一个上下文切换

这里的线程相比现代操作系统中的线程而言，更接近一些语言中的“协程”（coroutine）。原因是这里的“线程”是完全用户态实现的，多个线程也只能运行在一个 CPU 上，并且没有时钟中断来强制执行调度，需要线程函数本身在合适的时候主动 yield 释放 CPU。这样实现起来的线程并不对线程函数透明，所以比起操作系统的线程而言更接近 coroutine。

这个实验其实相当于在用户态重新实现一遍 xv6 kernel 中的 scheduler() 和 swtch() 的功能，所以大多数代码都是可以借鉴的。

按照Chapter7.2，首先要定义一个上下文结构，然后在 `struct thread` 中加入这个结构，直接照搬 xv6中的定义即可

- ```c
  struct ucontext {
    uint64 ra;
    uint64 sp;
  
    // callee-saved
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
  };
  
  struct thread {
    char       stack[STACK_SIZE]; /* the thread's stack */
    int        state;             /* FREE, RUNNING, RUNNABLE */
    struct ucontext context;      /* context */
  ```

之后，在 `thread_create` 时，要填充上下文的一些信息：只关心返回的函数地址和栈指针，至于那些被调用方的寄存器，在 switch 的时候会被保存

- ```c
  void 
  thread_create(void (*func)())
  {
    struct thread *t;
  
    for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
      if (t->state == FREE) break;
    }
    t->state = RUNNABLE;
    // YOUR CODE HERE
    // Initialize t's context to prepare it for running the thread function.
    t->context.ra = (uint64)func;
    t->context.sp = (uint64)(t->stack + STACK_SIZE);
  }
  ```

接下来，完成 `uthread_switch.S`，这个函数完成了上下文的切换，同样模仿xv6中的实现即可

- ```c
  	.text
  
  	/*
           * save the old thread's registers,
           * restore the new thread's registers.
           */
  
  	.globl thread_switch
  thread_switch:
  	/* YOUR CODE HERE */
  	sd ra, 0(a0)
  	sd sp, 8(a0)
  	sd s0, 16(a0)
  	sd s1, 24(a0)
  	sd s2, 32(a0)
  	sd s3, 40(a0)
  	sd s4, 48(a0)
  	sd s5, 56(a0)
  	sd s6, 64(a0)
  	sd s7, 72(a0)
  	sd s8, 80(a0)
  	sd s9, 88(a0)
  	sd s10, 96(a0)
  	sd s11, 104(a0)
  
  	ld ra, 0(a1)
  	ld sp, 8(a1)
  	ld s0, 16(a1)
  	ld s1, 24(a1)
  	ld s2, 32(a1)
  	ld s3, 40(a1)
  	ld s4, 48(a1)
  	ld s5, 56(a1)
  	ld s6, 64(a1)
  	ld s7, 72(a1)
  	ld s8, 80(a1)
  	ld s9, 88(a1)
  	ld s10, 96(a1)
  	ld s11, 104(a1)
  	ret    /* return to ra */
  ```

- 引申：内核调度器无论是通过时钟中断进入（usertrap），还是线程自己主动放弃 CPU（sleep、exit），最终都会调用到 yield 进一步调用 swtch。 由于上下文切换永远都发生在函数调用的边界（swtch 调用的边界），恢复执行相当于是 swtch 的返回过程，会从堆栈中恢复 caller-saved 的寄存器， 所以用于保存上下文的 context 结构体只需保存 callee-saved 寄存器，以及 返回地址 ra、栈指针 sp 即可。恢复后执行到哪里是通过 ra 寄存器来决定的（swtch 末尾的 ret 转跳到 ra）

  而 trapframe 则不同，一个中断可能在任何地方发生，不仅仅是函数调用边界，也有可能在函数执行中途，所以恢复的时候需要靠 pc 寄存器来定位。 并且由于切换位置不一定是函数调用边界，所以几乎所有的寄存器都要保存（无论 caller-saved 还是 callee-saved），才能保证正确的恢复执行。 这也是内核代码中 `struct trapframe` 中保存的寄存器比 `struct context` 多得多的原因。

  另外一个，无论是程序主动 sleep，还是时钟中断，都是通过 trampoline 跳转到内核态 usertrap（保存 trapframe），然后再到达 swtch 保存上下文的。 恢复上下文都是恢复到 swtch 返回前（依然是内核态），然后返回跳转回 usertrap，再继续运行直到 usertrapret 跳转到 trampoline 读取 trapframe，并返回用户态。 也就是上下文恢复并不是直接恢复到用户态，而是恢复到内核态 swtch 刚执行完的状态。负责恢复用户态执行流的其实是 trampoline 以及 trapframe。

最后，在 `thread_schedule` 中调用 `thread_switch`

- ```c
      /* YOUR CODE HERE
       * Invoke thread_switch to switch from t to next_thread:
       * thread_switch(??, ??);
       */
      thread_switch((uint64)&t->context, (uint64)&next_thread->context);
  ```



## Task2

### 要求

文件***notxv6/ph.c\***包含一个简单的哈希表，如果单个线程使用，该哈希表是正确的，但是多个线程使用时，该哈希表是不正确的。在您的xv6主目录（可能是`~/xv6-labs-2020`）中，键入以下内容：

```bash
$ make ph
$ ./ph 1
```

请注意，要构建`ph`，***Makefile\***使用操作系统的gcc，而不是6.S081的工具。`ph`的参数指定在哈希表上执行`put`和`get`操作的线程数。运行一段时间后，`ph 1`将产生与以下类似的输出：

```
100000 puts, 3.991 seconds, 25056 puts/second
0: 0 keys missing
100000 gets, 3.981 seconds, 25118 gets/second
```

您看到的数字可能与此示例输出的数字相差两倍或更多，这取决于您计算机的速度、是否有多个核心以及是否正在忙于做其他事情。

`ph`运行两个基准程序。首先，它通过调用`put()`将许多键添加到哈希表中，并以每秒为单位打印puts的接收速率。之后它使用`get()`从哈希表中获取键。它打印由于puts而应该在哈希表中但丢失的键的数量（在本例中为0），并以每秒为单位打印gets的接收数量。

通过给`ph`一个大于1的参数，可以告诉它同时从多个线程使用其哈希表。试试`ph 2`：

```bash
$ ./ph 2
100000 puts, 1.885 seconds, 53044 puts/second
1: 16579 keys missing
0: 16579 keys missing
200000 gets, 4.322 seconds, 46274 gets/second
```

这个`ph 2`输出的第一行表明，当两个线程同时向哈希表添加条目时，它们达到每秒53044次插入的总速率。这大约是运行`ph 1`的单线程速度的两倍。这是一个优秀的“并行加速”，大约达到了人们希望的2倍（即两倍数量的核心每单位时间产出两倍的工作）。

然而，声明`16579 keys missing`的两行表示散列表中本应存在的大量键不存在。也就是说，puts应该将这些键添加到哈希表中，但出现了一些问题。请看一下***notxv6/ph.c\***，特别是`put()`和`insert()`。



 YOUR JOB

为什么两个线程都丢失了键，而不是一个线程？确定可能导致键丢失的具有2个线程的事件序列。在***answers-thread.txt\***中提交您的序列和简短解释。

[!TIP] 为了避免这种事件序列，请在***notxv6/ph.c\***中的`put`和`get`中插入`lock`和`unlock`语句，以便在两个线程中丢失的键数始终为0。相关的pthread调用包括：

- `pthread_mutex_t lock; // declare a lock`
- `pthread_mutex_init(&lock, NULL); // initialize the lock`
- `pthread_mutex_lock(&lock); // acquire lock`
- `pthread_mutex_unlock(&lock); // release lock`

当`make grade`说您的代码通过`ph_safe`测试时，您就完成了，该测试需要两个线程的键缺失数为0。在此时，`ph_fast`测试失败是正常的。

不要忘记调用`pthread_mutex_init()`。首先用1个线程测试代码，然后用2个线程测试代码。您主要需要测试：程序运行是否正确呢（即，您是否消除了丢失的键？）？与单线程版本相比，双线程版本是否实现了并行加速（即单位时间内的工作量更多）？

在某些情况下，并发`put()`在哈希表中读取或写入的内存中没有重叠，因此不需要锁来相互保护。您能否更改***ph.c\***以利用这种情况为某些`put()`获得并行加速？提示：每个散列桶加一个锁怎么样？



 YOUR JOB

修改代码，使某些`put`操作在保持正确性的同时并行运行。当`make grade`说你的代码通过了`ph_safe`和`ph_fast`测试时，你就完成了。`ph_fast`测试要求两个线程每秒产生的`put`数至少是一个线程的1.25倍。



### 实现

这题比较简单，两个test放在一起说了，就是一个加锁的操作，给哈希表的每个桶加锁，这样 put 和 get 不会冲突，put之间也不会冲突，而且不同的桶的 put之间是可以并行的

- ```c
  #include <stdlib.h>
  #include <unistd.h>
  #include <stdio.h>
  #include <assert.h>
  #include <pthread.h>
  #include <sys/time.h>
  
  #define NBUCKET 5
  #define NKEYS 100000
  
  struct entry {
    int key;
    int value;
    struct entry *next;
  };
  struct entry *table[NBUCKET];
  int keys[NKEYS];
  int nthread = 1;
  
  pthread_mutex_t locks[NBUCKET];
  
  
  double
  now()
  {
   struct timeval tv;
   gettimeofday(&tv, 0);
   return tv.tv_sec + tv.tv_usec / 1000000.0;
  }
  
  static void 
  insert(int key, int value, struct entry **p, struct entry *n)
  {
    struct entry *e = malloc(sizeof(struct entry));
    e->key = key;
    e->value = value;
    e->next = n;
    *p = e;
  }
  
  static 
  void put(int key, int value)
  {
    int i = key % NBUCKET;
  
    pthread_mutex_lock(&locks[i]);
  
    // is the key already present?
    struct entry *e = 0;
    for (e = table[i]; e != 0; e = e->next) {
      if (e->key == key)
        break;
    }
    if(e){
      // update the existing key.
      e->value = value;
    } else {
      // the new is new.
      insert(key, value, &table[i], table[i]);
    }
  
    pthread_mutex_unlock(&locks[i]);
  }
  
  static struct entry*
  get(int key)
  {
    int i = key % NBUCKET;
  
    pthread_mutex_lock(&locks[i]);
  
    struct entry *e = 0;
    for (e = table[i]; e != 0; e = e->next) {
      if (e->key == key) break;
    }
  
    pthread_mutex_unlock(&locks[i]);
  
    return e;
  }
  
  static void *
  put_thread(void *xa)
  {
    int n = (int) (long) xa; // thread number
    int b = NKEYS/nthread;
  
    for (int i = 0; i < b; i++) {
      put(keys[b*n + i], n);
    }
  
    return NULL;
  }
  
  static void *
  get_thread(void *xa)
  {
    int n = (int) (long) xa; // thread number
    int missing = 0;
  
    for (int i = 0; i < NKEYS; i++) {
      struct entry *e = get(keys[i]);
      if (e == 0) missing++;
    }
    printf("%d: %d keys missing\n", n, missing);
    return NULL;
  }
  
  int
  main(int argc, char *argv[])
  {
    pthread_t *tha;
    void *value;
    double t1, t0;
  
    pthread_mutex_init(&locks, NULL);
  
    if (argc < 2) {
      fprintf(stderr, "Usage: %s nthreads\n", argv[0]);
      exit(-1);
    }
    nthread = atoi(argv[1]);
    tha = malloc(sizeof(pthread_t) * nthread);
    srandom(0);
    assert(NKEYS % nthread == 0);
    for (int i = 0; i < NKEYS; i++) {
      keys[i] = random();
    }
  
    //
    // first the puts
    //
    t0 = now();
    for(int i = 0; i < nthread; i++) {
      assert(pthread_create(&tha[i], NULL, put_thread, (void *) (long) i) == 0);
    }
    for(int i = 0; i < nthread; i++) {
      assert(pthread_join(tha[i], &value) == 0);
    }
    t1 = now();
  
    printf("%d puts, %.3f seconds, %.0f puts/second\n",
           NKEYS, t1 - t0, NKEYS / (t1 - t0));
  
    //
    // now the gets
    //
    t0 = now();
    for(int i = 0; i < nthread; i++) {
      assert(pthread_create(&tha[i], NULL, get_thread, (void *) (long) i) == 0);
    }
    for(int i = 0; i < nthread; i++) {
      assert(pthread_join(tha[i], &value) == 0);
    }
    t1 = now();
  
    printf("%d gets, %.3f seconds, %.0f gets/second\n",
           NKEYS*nthread, t1 - t0, (NKEYS*nthread) / (t1 - t0));
  }
  ```

## Task3

### 要求

文件***notxv6/barrier.c\***包含一个残缺的屏障实现。

```bash
$ make barrier
$ ./barrier 2
barrier: notxv6/barrier.c:42: thread: Assertion `i == t' failed.
```

2指定在屏障上同步的线程数（***barrier.c\***中的`nthread`）。每个线程执行一个循环。在每次循环迭代中，线程都会调用`barrier()`，然后以随机微秒数休眠。如果一个线程在另一个线程到达屏障之前离开屏障将触发断言（assert）。期望的行为是每个线程在`barrier()`中阻塞，直到`nthreads`的所有线程都调用了`barrier()`。

 YOUR JOB

您的目标是实现期望的屏障行为。除了在`ph`作业中看到的lock原语外，还需要以下新的pthread原语；详情请看[这里](https://pubs.opengroup.org/onlinepubs/007908799/xsh/pthread_cond_wait.html)和[这里](https://pubs.opengroup.org/onlinepubs/007908799/xsh/pthread_cond_broadcast.html)。

- `// 在cond上进入睡眠，释放锁mutex，在醒来时重新获取`
- `pthread_cond_wait(&cond, &mutex);`
- `// 唤醒睡在cond的所有线程`
- `pthread_cond_broadcast(&cond);`

确保您的方案通过`make grade`的`barrier`测试。

`pthread_cond_wait`在调用时释放`mutex`，并在返回前重新获取`mutex`。

我们已经为您提供了`barrier_init()`。您的工作是实现`barrier()`，这样panic就不会发生。我们为您定义了`struct barrier`；它的字段供您使用。

**有两个问题使您的任务变得复杂：**

- 你必须处理一系列的`barrier`调用，我们称每一连串的调用为一轮（round）。`bstate.round`记录当前轮数。每次当所有线程都到达屏障时，都应增加`bstate.round`。
- 您必须处理这样的情况：一个线程在其他线程退出`barrier`之前进入了下一轮循环。特别是，您在前后两轮中重复使用`bstate.nthread`变量。确保在前一轮仍在使用`bstate.nthread`时，离开`barrier`并循环运行的线程不会增加`bstate.nthread`。

使用一个、两个和两个以上的线程测试代码。



### 实现

这题也比较容易

线程进入同步屏障 barrier 时，将已进入屏障的线程数量增加 1，然后再判断是否已经达到总线程数。

- 如果未达到，则进入睡眠，等待其他线程。
- 如果已经达到，则唤醒所有在 barrier 中等待的线程，所有线程继续执行；屏障轮数 + 1；

「将已进入屏障的线程数量增加 1，然后再判断是否已经达到总线程数」这一步并不是原子操作，并且这一步和后面的两种情况中的操作「睡眠」和「唤醒」之间也不是原子的，如果在这里发生 race-condition，则会导致出现 「lost wake-up 问题」（线程 1 即将睡眠前，线程 2 调用了唤醒，然后线程 1 才进入睡眠，导致线程 1 本该被唤醒而没被唤醒，详见 [xv6 book](https://pdos.csail.mit.edu/6.S081/2020/xv6/book-riscv-rev1.pdf) 中的第 72 页，Sleep and wakeup）

解决方法是，「屏障的线程数量增加 1；判断是否已经达到总线程数；进入睡眠」这三步必须原子。所以使用一个互斥锁 barrier_mutex 来保护这一部分代码。pthread_cond_wait 会在进入睡眠的时候原子性的释放 barrier_mutex，从而允许后续线程进入 barrier，防止死锁。

```c
barrier()
{
  // YOUR CODE HERE
  //
  // Block until all threads have called barrier() and
  // then increment bstate.round.
  //
  pthread_mutex_lock(&bstate.barrier_mutex);
  if (++bstate.nthread < nthread) 
    pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
  else {
    bstate.nthread = 0;
    bstate.round++;
    pthread_cond_broadcast(&bstate.barrier_cond);
  }
  pthread_mutex_unlock(&bstate.barrier_mutex);
}
```



# 结果

![image-20240619182410847](assets/image-20240619182410847.png)




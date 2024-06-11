# Chapter1摘录

## 进程和内存

Fork创建了一个新的进程，其内存内容与调用进程（称为父进程）完全相同，称其为子进程。Fork在父子进程中都返回值。

- 在父进程中，fork返回子类的PID；
- 在子进程中，fork返回零。

```c
// fork()在父进程中返回子进程的PID
// 在子进程中返回0
int pid = fork();
if(pid > 0) {
    printf("parent: child=%d\n", pid);
    pid = wait((int *) 0);
    printf("child %d is done\n", pid);
} else if(pid == 0) {
    printf("child: exiting\n");
    exit(0);
} else {
    printf("fork error\n");
}
```

- `exit`系统调用导致调用进程停止执行并释放资源（如内存和打开的文件）
  - `exit`接受一个整数状态参数，通常0表示成功，1表示失败

- `wait`系统调用返回当前进程的已退出(或已杀死)子进程的PID，并将子进程的退出状态复制到传递给`wait`的地址；

  - 如果调用方的子进程都没有退出，那么wait等待一个子进程退出。

  - 如果调用者没有子进程，`wait`立即返回-1。

  - 如果父进程不关心子进程的退出状态，它可以传递一个0地址给`wait`。

- 在这个例子中，输出

```
parent: child=1234
child: exiting
```

- 可能以任何一种顺序出来，这取决于父或子谁先到达`printf`调用。子进程退出后，父进程的`wait`返回，导致父进程打印

```
parent: child 1234 is done
```



`exec`系统调用使用从文件系统中存储的文件所加载的新内存映像替换调用进程的内存

- 根据指定的文件名找到可执行文件，并用它来取代调用进程的内容，换句话说，就是在调用进程内部执行一个可执行文件
- 该文件必须有特殊的格式，它指定文件的哪部分存放指令，哪部分是数据，以及哪一条指令用于启动等等。xv6使用ELF格式
- `exec`有两个参数：可执行文件的文件名和字符串参数数组

```c
char* argv[3];
argv[0] = "echo";
argv[1] = "hello";
argv[2] = 0;
exec("/bin/echo", argv);
printf("exec error\n");
```

- 这个代码片段将调用程序替换为了参数列表为`echo hello`的`/bin/echo`程序运行，多数程序忽略参数数组中的第一个元素，它通常是程序名。

xv6的shell使用上述调用为用户运行程序。shell的主要结构很简单，请参见`main`(***user/sh.c:145***)。

```c
int
main(void)
{
  static char buf[100];
  int fd;
   
  // ...

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    if(buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' '){
      // Chdir must be called by the parent, not the child.
      buf[strlen(buf)-1] = 0;  // chop \n
      if(chdir(buf+3) < 0)
        fprintf(2, "cannot cd %s\n", buf+3);
      continue;
    }
    if(fork1() == 0)
      runcmd(parsecmd(buf));
    wait(0);
  }
  exit(0);
}
```

- 主循环使用`getcmd`函数从用户的输入中读取一行，然后调用`fork`创建一个shell进程的副本。父进程调用`wait`，子进程执行命令。

## I/O和文件描述符

文件描述符是一个小整数(small integer)，表示进程可以读取或写入的由内核管理的对象。进程可以通过打开一个文件、目录、设备，或创建一个管道，或复制一个已存在的描述符来获得一个文件描述符。

- 常将文件描述符所指的对象称为“文件”；
- 文件描述符接口将文件、管道和设备之间的差异抽象出来，使它们看起来都像字节流。我们将输入和输出称为 I/O。

在内部，xv6内核使用文件描述符作为每个进程表的索引，这样每个进程都有一个从零开始的文件描述符的私有空间。

- 进程从文件描述符0读取（标准输入），
- 将输出写入文件描述符1（标准输出），
- 并将错误消息写入文件描述符2（标准错误）。

shell确保它始终有三个打开的文件描述符（***user/sh.c***:151），这是控制台的默认文件描述符。

```c
int
main(void)
{
  static char buf[100];
  int fd;

  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  // ...

```

`read`和`write`系统调用以字节为单位读取或写入已打开的以文件描述符命名的文件。

- `read(fd，buf，n)`从文件描述符fd读取最多n字节，将它们复制到buf，并**返回读取的字节数**，引用文件的每个文件描述符都有一个与之关联的偏移量。
  - `read`从**当前文件偏移量**开始读取数据，然后将该偏移量前进所读取的字节数：（也就是说）后续读取将返回第一次读取返回的字节之后的字节。
  - `read`返回0来表示文件的结束。
- 系统调用`write(fd，buf，n)`将buf中的n字节写入文件描述符，并返回写入的字节数。
  - 只有发生错误时才会写入小于n字节的数据。
  - 与读一样，`write`在当前文件偏移量处写入数据，然后将该偏移量向前推进写入的字节数：每个`write`从上一个偏移量停止的地方开始写入。

`close`系统调用**释放**一个文件描述符，使其可以被未来使用的`open`、`pipe`或`dup`系统调用重用（见下文）。新分配的文件描述符总是**当前进程中编号最小的未使用描述符。**

`fork`复制父进程的文件描述符表及其内存，以便子级以与父级在开始时拥有完全相同的打开文件。系统调用`exec`替换了调用进程的内存，但保留其文件表。此行为允许shell通过`fork`实现I/O重定向，在子进程中重新打开选定的文件描述符，然后调用`exec`来运行新程序。

```c
char* argv[2];
argv[0] = "cat";
argv[1] = 0;
if (fork() == 0) {
    close(0);
    open("input.txt", O_RDONLY);
    exec("cat", argv);
}
```

- 子进程关闭文件描述符0之后，`open`保证使用新打开的***input.txt***：0的文件描述符作为最小的可用文件描述符。`cat`然后执行文件描述符0(标准输入)，但引用的是***input.txt***。父进程的文件描述符不会被这个序列改变，因为它只修改子进程的描述符。

- `fork` 和 `exec` 分离的用处：在这两个调用之间，shell有机会对子进程进行I/O重定向，而不会干扰主shell的I/O设置。

- 尽管`fork`复制了文件描述符表，但是每个基础文件偏移量在父文件和子文件之间是共享的，比如下面的程序：

  ```c
  if (fork() == 0) {
      write(1, "hello ", 6);
      exit(0);
  } else {
      wait(0);
      write(1, "world\n", 6);
  }
  ```

  - 将会输出 `hello world`

`dup`系统调用复制一个现有的文件描述符，返回一个引用自同一个底层I/O对象的新文件描述符。两个文件描述符共享一个偏移量，就像fork复制的文件描述符一样。这是另一种将“hello world”写入文件的方法：

```c
fd = dup(1);
write(1, "hello ", 6);
write(fd, "world\n", 6);
```

**只有通过 `fork` 和 `dup` 系统调用从同一个原始文件派生出来的`fd` 才会共享偏移量**

- 否则，即使来自于对同一个文件的打开调用，文件描述符也不会共享偏移量

## 管道

管道是作为一对文件描述符公开给进程的小型内核缓冲区，一个用于读取，一个用于写入。将数据写入管道的一端使得这些数据可以从管道的另一端读取。

```c
int p[2];
char *argv[2];
argv[0] = "wc";
argv[1] = 0;
pipe(p);
if (fork() == 0) {
    close(0);
    // 优先使用最小的fd（也就是0），指向p[0]
    dup(p[0]);
    close(p[0]);
    close(p[1]);
    // 从标准输入0中读取时，实际上就是从管道读取
    exec("/bin/wc", argv);
} else {
    close(p[0]);
    write(p[1], "hello world\n", 12);
    close(p[1]);
}
```

- **为什么子进程在执行` wc` 之前要关闭管道的写入端**
  - 如果没有可用的数据，则管道上的`read`操作将会进入等待
    - 直到有新数据写入
    - 或所有指向写入端的文件描述符都被关闭，此时返回0，就像到达数据文件的末尾
  - 如果wc的文件描述符之一指向写入端，那么wc将永远看不到文件的结束，这意味着**在新数据到达前read会一直阻塞**，也就意味着如果没有新数据了，read也不会返回

管道看起来并不比临时文件更强大：下面的管道命令行

```bash
echo hello world | wc
```

可以不通过管道实现，如下

```bash
echo hello world > /tmp/xyz; wc < /tmp/xyz
```

- 在这种情况下，管道相比临时文件至少有四个优势
  - 首先，管道会自动清理自己；在文件重定向时，shell使用完`/tmp/xyz`后必须小心删除
  - 其次，管道可以任意传递长的数据流，而文件重定向需要磁盘上足够的空闲空间来存储所有的数据。
  - 第三，管道允许并行执行管道阶段，而文件方法要求第一个程序在第二个程序启动之前完成。
  - 第四，如果实现进程间通讯，管道的**阻塞**式读写比文件的非阻塞语义更高效。

## 文件系统

一个文件的名字和文件本身是不同的;同一个底层文件（叫做inode，索引结点）可以有多个名字（叫做link，链接）。

`fstat`系统调用从文件描述符所引用的inode中检索信息。它填充一个`stat`类型的结构体，`struct stat`在***stat.h(kernel/stat.h)***中定义为

```c
#define T_DIR 1    // Directory
#define T_FILE 2   // File
#define T_DEVICE 3 // Device
struct stat {
    int dev;     // 文件系统的磁盘设备
    uint ino;    // Inode编号
    short type;  // 文件类型
    short nlink; // 指向文件的链接数
    uint64 size; // 文件字节数
};
```

`link`系统调用创建另一个文件名，该文件名指向与现有文件相同的inode。下面的代码片段创建了一个名字既为***a***又为***b***的新文件

```c
open("a", O_CREATE | O_WRONLY);
link("a", "b");
```

- 从***a***读取或写入与从***b***读取或写入是相同的操作。每个inode由唯一的inode编号标识。在上面的代码序列之后，可以通过检查`fstat`的结果来确定a和b引用相同的底层内容:两者都将返回相同的inode号(`ino`)，并且`nlink`计数将被设置为2。

`unlink`系统调用从文件系统中删除一个名称。只有当**文件的链接数为零**且**没有文件描述符引用**时，文件的inode和包含其内容的磁盘空间才会被释放

```c
unlink("a");
```



# 实验

## Task1

### 要求

- **实现xv6的UNIX程序**`sleep`**：您的**`sleep`**应该暂停到用户指定的计时数。一个滴答(tick)是由xv6内核定义的时间概念，即来自定时器芯片的两个中断之间的时间。您的解决方案应该在文件\*user/sleep.c\*中**

### 提示

- 看看其他的一些程序（如***/user/echo.c, /user/grep.c, /user/rm.c***）查看如何获取传递给程序的命令行参数
- 如果用户忘记传递参数，`sleep`应该打印一条错误信息
- 命令行参数作为字符串传递; 您可以使用`atoi`将其转换为数字（详见***/user/ulib.c***）
- 使用系统调用`sleep`
- 请参阅***kernel/sysproc.c***以获取实现`sleep`系统调用的xv6内核代码（查找`sys_sleep`），***user/user.h***提供了`sleep`的声明以便其他程序调用，用汇编程序编写的***user/usys.S***可以帮助`sleep`从用户区跳转到内核区。
- 确保`main`函数调用`exit()`以退出程序。
- 将你的`sleep`程序添加到***Makefile***中的`UPROGS`中；完成之后，`make qemu`将编译您的程序，并且您可以从xv6的shell运行它。

### 实现

**sleep.c**

```c
int
main(int argc, char *argv[]){
    // 检查用户参数
    if (argc <= 1)
        printf("Error, no argument provided\n");
    // 转换数字
    int sleep_time = atoi(argv[1]);
    // 调用sleep系统调用
    sleep(sleep_time);
    // main函数使用exit退出
    exit(0);
}
```



## Task2

### 要求

- **编写一个使用UNIX系统调用的程序来在两个进程之间“ping-pong”一个字节，请使用两个管道，每个方向一个。父进程应该向子进程发送一个字节;子进程应该打印“`<pid>: received ping`”，其中`<pid>`是进程ID，并在管道中写入字节发送给父进程，然后退出;父级应该从读取从子进程而来的字节，打印“`<pid>: received pong`”，然后退出。您的解决方案应该在文件*user/pingpong.c*中。**

### 提示

- 使用`pipe`来创造管道
- 使用`fork`创建子进程
- 使用`read`从管道中读取数据，并且使用`write`向管道中写入数据
- 使用`getpid`获取调用进程的pid
- 将程序加入到***Makefile***的`UPROGS`

### 实现

**pingpong.c**

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main() {
    int p1[2], p2[2];
    pipe(p1); // parent->child
    pipe(p2); // child->parent

    if (fork() == 0) {
        // child
        // 关闭p1的写端和p2的读端
        close(p1[1]);
        close(p2[0]);
        
        // 读取一个字节的数据
        char buf[1];
        if (read(p1[0], buf, 1) != 1) {
            printf("child: read error\n");
            exit(1);
        }
        printf("%d: received ping\n", getpid());
        // 此时p1的读端也不需要了
        close(p1[0]);
        // 向p2中写
        write(p2[1], "1", 1);
    }
    else {
        // parent
        close(p1[0]);
        close(p2[1]);
        
        write(p1[1], "1", 1);
        // 这里写完要等待子进程返回，但wait并不必要，因为read在没有数据的时候会等待
        // wait(0);
        char buf[1];
        if (read(p2[0], buf, 1) != 1) {
            printf("parent: read error\n");
            exit(1);
        }
        printf("%d: received pong\n", getpid());
        close(p2[0]);
        exit(0);
    }
    exit(0);
}
```



## Task3

### 要求

- **使用管道编写**`prime sieve`**(筛选素数)的并发版本。这个想法是由Unix管道的发明者Doug McIlroy提出的。请查看[这个网站](http://swtch.com/~rsc/thread/)(翻译在下面)，该网页中间的图片和周围的文字解释了如何做到这一点。您的解决方案应该在*user/primes.c*文件中。**

### 提示

- 请仔细关闭进程不需要的文件描述符，否则您的程序将在第一个进程达到35之前就会导致xv6系统资源不足。
- 一旦第一个进程达到35，它应该使用`wait`等待整个管道终止，包括所有子孙进程等等。因此，主`primes`进程应该只在打印完所有输出之后，并且在所有其他`primes`进程退出之后退出。
- 提示：当管道的`write`端关闭时，`read`返回零。
- 最简单的方法是直接将32位（4字节）int写入管道，而不是使用格式化的ASCII I/O。
- 您应该仅在需要时在管线中创建进程。
- 将程序添加到***Makefile***中的`UPROGS`

### 实现

- 埃氏筛的原理是：每一个 stage 以当前数集中最小的数字作为素数输出（每个 stage 中数集中最小的数一定是一个素数，因为它没有被任何比它小的数筛掉），并筛掉输入中该素数的所有倍数（必然不是素数），然后将剩下的数传递给下一 stage。最后会形成一条子进程链，而由于每一个进程都调用了 `wait(0);` 等待其子进程，所以会在最末端也就是最后一个 stage 完成的时候，沿着链条向上依次退出各个进程。

- xv6的每个进程能够打开的文件描述符有限（16个）

  - ```c
    // kernel/param.h
    #define NOFILE       16  // open files per process
    ```

  - fork 会把父进程的所有文件描述符都复制给子进程

  - 需要注意关闭管道的文件描述符

    - 当前管道不需要的读端或写端
    - 子进程创建后关闭父进程的文件描述符

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// 创建一个递归函数，用于一轮筛选
void traversal(int p[2]) {
    close(p[1]);
    int prime;
    // 获取的第一个数字一定是素数
    read(p[0], &prime, sizeof(prime));
    // 读到哨兵节点则退出
    if (prime == -1)
        exit(0);
    printf("prime %d\n", prime);

    int pout[2];
    pipe(pout);

    if (fork() == 0) {
        close(p[0]);
        close(pout[1]);
        // 递归到下一次筛选，完成后返回
        traversal(pout);
        exit(0);
    }
    else {
        close(pout[0]);
        int buf;
        // 获取左管道剩余的数字
        while (read(p[0], &buf, sizeof(buf)) && buf != -1) {
            // 如果被当前的素数整除，则不会进入下一次筛选
            if (buf % prime != 0)
                write(pout[1], &buf, sizeof(buf));
        }
        buf = -1;
        write(pout[1], &buf, sizeof(buf));
        // 父进程等待子进程完成
        wait(0);
        exit(0);
    }
}

int main() {
    int p[2];
    pipe(p);
    
    if (fork() == 0) {
        close(p[1]);
        traversal(p);
        exit(0);
    }
    else {
        close(p[0]);
        // 输入2-35
        for (int i = 2; i <= 35; i++)
            write(p[1], &i, sizeof(i));
        int buf = -1;
        write(p[1], &buf, sizeof(buf));
    }
    wait(0);
    exit(0);
}
```

## Task4

### 要求

- **写一个简化版本的UNIX的`find`程序：查找目录树中具有特定名称的所有文件，你的解决方案应该放在*user/find.c***

### 提示

- 查看***user/ls.c***文件学习如何读取目录
- 使用递归允许`find`下降到子目录中
- 不要在“`.`”和“`..`”目录中递归

### 实现

- 基本上基于 ls.c 中的 ls函数，需要修改几个地方
  - 添加一个 target 参数表示寻找的文件名
  - 在目录不为“`.`”和“`..`”时加入递归

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

void
find(char *path, char *target)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;
  // 尝试以只读方式打开文件或目录
  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }
  // 获取文件状态
  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  // 对于单个文件，比较其最后的值和 /target
  case T_FILE:
    // strcmp函数在两者相等时返回0
    if (strcmp(path + strlen(path) - strlen(target), target) == 0)
        printf("%s\n", path);
    break;

  case T_DIR:
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("find: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    // 给每个目录前加上 /
    *p++ = '/';
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("find: cannot stat %s\n", buf);
        continue;
      }
      // 这里递归，但是要避免 . 和 .. 目录
      if (strcmp(buf + strlen(buf) - 2, "/.") != 0 && strcmp(buf + strlen(buf) - 3, "/..") != 0)
        find(buf, target);
    }
    break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc < 3){
    exit(0);
  }
    char target[512];
	target[0] = '/'; // 为查找的文件名添加 / 在开头
	strcpy(target+1, argv[2]);
	find(argv[1], target);
	exit(0);
}
```

## Task5

### 要求

- 编写一个简化版UNIX的`xargs`程序：它从标准输入中按行读取，并且为每一行执行一个命令，将行作为参数提供给命令。你的解决方案应该在***user/xargs.c***

### 提示

下面的例子解释了`xargs`的行为

```bash
$ echo hello too | xargs echo bye
bye hello too
$
```

注意，这里的命令是`echo bye`，额外的参数是`hello too`，这样就组成了命令`echo bye hello too`，此命令输出`bye hello too`

- 使用`fork`和`exec`对每行输入调用命令，在父进程中使用`wait`等待子进程完成命令。
- 要读取单个输入行，请一次读取一个字符，直到出现换行符（'\n'）。
- ***kernel/param.h***声明`MAXARG`，如果需要声明`argv`数组，这可能很有用。

### 实现

并不是很理解这道题，参考了他人的解答

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"
#include "kernel/fs.h"

// 带参数列表，调用exec执行某个程序
void run(char *program, char **args) {
    if (fork() == 0) {
        exec(program, args);
        exit(0);
    }
    return;
}

int main(int argc, char *argv[]) {
    // 读入时使用的内存池
    char buf[2048];
    // 当前参数的结束、开始指针
    char *p = buf, *last_p = buf;
    // 全部参数列表，包含argv传入的参数和从stdin读入的参数
    char *argsbuf[128];
    // 指向argsbuf中第一个从stdin读入的参数
    char **args = argsbuf;
    // 先将argv的参数读入
    for (int i = 1; i < argc; ++i) {
        *args = argv[i];
        args++;
    }
    // 开始读入参数
    char **pa = args;
    while (read(0, p, 1) != 0) {
        // 读入一个参数完成
        if (*p == ' ' || *p == '\n') {
            if (*p == ' ') {
                // 将空格替换为 '\0'（字符串终止符） 分割各个参数，这样可以直接使用内存池的字符串作为参数字符串，不用开辟额外空间
                *p = '\0';
                // 将当前参数的开始地址存储在 argsbuf中
                *(pa++) = last_p;
                // 更新为下一个参数的开始地址
                last_p = p + 1;
            }
            else {
                *p = '\0';
                *(pa++) = last_p;
                last_p = p + 1;
                // 用0终止参数列表
                *pa = 0;
                run(argv[1], argsbuf);
                // 重置参数指针
                pa = args;
            }
        }
        p++;
    }
    if (pa != args) {
        *pa = 0;
        run(argv[1], argsbuf);
    }
    while (wait(0) != -1);
    exit(0);
}
```

# 结果

![image-20240611200018936](assets/image-20240611200018936.png)
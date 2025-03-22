#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "fcntl.h"  // 确保包含头文件以使用 PROT_READ 等宏
#include "fs.h"
#include "proc.h"
#include "file.h"
#include <stddef.h>

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

static const char *
scause_desc(uint64 stval);

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
// Handle traps from user space
void 
usertrap(void)
{
  int which_dev = 0;
  struct proc *p = myproc();

  // 确保陷阱来自用户态
  if ((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // 设置陷阱处理函数为内核态处理逻辑
  w_stvec((uint64)kernelvec);

  // 保存用户程序计数器
  p->tf->epc = r_sepc();

  // 处理系统调用
  if (r_scause() == 8) {
    if (p->killed)
      exit(-1);
    
    // 系统调用处理
    p->tf->epc += 4;
    intr_on();
    syscall();
  
  } 
  // 处理页面错误（13: Load Page Fault, 15: Store/AMO Page Fault）
  else if (r_scause() == 13 || r_scause() == 15) {
    uint64 va = r_stval();
    pte_t *pte;
    uint flags;
    char *mem;

    // 1. 检查地址是否在进程地址空间内
    if (va >= p->sz || va < p->tf->sp) {
      p->killed = 1;
      goto exit;
    }

    // 2. 查找对应的 VMA（内存映射区域）
    struct vma *vma = 0;
    for (int i = 0; i < NVMA; i++) {
      if (p->vmas[i].valid && va >= p->vmas[i].addr && 
          va < p->vmas[i].addr + p->vmas[i].length) {
        vma = &p->vmas[i];
        break;
      }
    }

    // 3. 处理 COW（写时复制）页面
    pte = walk(p->pagetable, va, 0);
    if (pte && (*pte & PTE_V) && (*pte & PTE_COW)) {
      // 分配新物理页
      if ((mem = kalloc()) == 0) {
        p->killed = 1;
        goto exit;
      }
      // 复制旧页内容
      memmove(mem, (char*)PTE2PA(*pte), PGSIZE);
      // 设置新页权限：可写，清除 COW 标记
      flags = (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW;
      // 释放旧页（减少引用计数）
      kfree((void*)PTE2PA(*pte));
      // 映射新页
      *pte = PA2PTE((uint64)mem) | flags;
      goto exit;
    }

    // 4. 处理内存映射文件（首次访问）
    if (vma) {
      mem = kalloc();
      if (!mem) {
        p->killed = 1;
        goto exit;
      }
      memset(mem, 0, PGSIZE);

      // 从文件读取数据
      ilock(vma->file->ip);
      readi(vma->file->ip, 0, (uint64)mem, va - vma->addr, PGSIZE);
      iunlock(vma->file->ip);

      // 设置页表权限
      flags = PTE_U;
      if (vma->prot & PROT_READ) flags |= PTE_R;
      if (vma->prot & PROT_WRITE) {
        if (vma->flags == MAP_SHARED) {
          flags |= PTE_W; // 共享映射直接可写
        } else {
          flags |= PTE_R | PTE_COW; // 私有映射标记为 COW
        }
      }

      // 映射到页表
      if (mappages(p->pagetable, PGROUNDDOWN(va), PGSIZE, 
                  (uint64)mem, flags, 1) != 0) {
        kfree(mem);
        p->killed = 1;
      }
    } else {
      p->killed = 1; // 无对应 VMA，终止进程
    }
  } 
  // 处理设备中断
  else if ((which_dev = devintr()) != 0) {
    // 设备中断处理
  } 
  // 未知陷阱类型
  else {
    printf("usertrap(): unexpected scause %p (%s) pid=%d\n", 
           r_scause(), scause_desc(r_scause()), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

exit:
  if (p->killed)
    exit(-1);

  // 如果是时钟中断，让出 CPU
  if (which_dev == 2)
    yield();

  // 返回用户态
  usertrapret();
}
int handle_mmap_fault(struct proc *p, uint64 va) {
  struct vma *target_vma = 0;

  // 1. 遍历 VMA 查找匹配区域
  for (int i = 0; i < NVMA; i++) {
      struct vma *v = &p->vmas[i];
      if (v->valid && va >= v->addr && va < v->addr + v->length) {
          target_vma = v;
          break;
      }
  }

  if (!target_vma) {
      printf("handle_mmap_fault: va 0x%x not in any VMA\n", va);
      return 0;
  }

  // 2. 分配物理页
  char *mem = kalloc();
  if (mem == 0) {
      printf("handle_mmap_fault: kalloc failed\n");
      return 0;
  }
  memset(mem, 0, PGSIZE);

  // 3. 计算文件偏移
  uint64 file_offset = va - target_vma->addr;

  // 4. 从文件读取数据
  ilock(target_vma->file->ip);
  int bytes_read = readi(target_vma->file->ip, 0, (uint64)mem, file_offset, PGSIZE);
  iunlock(target_vma->file->ip);

  if (bytes_read < 0) {
      printf("handle_mmap_fault: readi error\n");
      kfree(mem);
      return 0;
  }

  // 5. 设置页表权限
  int perm = PTE_U;
  if (target_vma->prot & PROT_READ) perm |= PTE_R;
  if (target_vma->prot & PROT_WRITE) {
      if (target_vma->flags == MAP_SHARED) {
          perm |= PTE_W;
      } else {
          perm |= PTE_R; // MAP_PRIVATE 初始为只读（COW）
      }
  }

  // 6. 映射到用户页表
  if (mappages(p->pagetable, PGROUNDDOWN(va), PGSIZE, (uint64)mem, perm, 1) != 0) {
      printf("handle_mmap_fault: mappages failed\n");
      kfree(mem);
      return 0;
  }

  return 1;
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // turn off interrupts, since we're switching
  // now from kerneltrap() to usertrap().
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->tf->kernel_satp = r_satp();         // kernel page table
  p->tf->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->tf->kernel_trap = (uint64)usertrap;
  p->tf->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->tf->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p (%s)\n", scause, scause_desc(scause));
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ || irq == VIRTIO1_IRQ ){
      virtio_disk_intr(irq - VIRTIO0_IRQ);
    } else {
      // the PLIC sends each device interrupt to every core,
      // which generates a lot of interrupts with irq==0.
    }

    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

static const char *
scause_desc(uint64 stval)
{
  static const char *intr_desc[16] = {
    [0] "user software interrupt",
    [1] "supervisor software interrupt",
    [2] "<reserved for future standard use>",
    [3] "<reserved for future standard use>",
    [4] "user timer interrupt",
    [5] "supervisor timer interrupt",
    [6] "<reserved for future standard use>",
    [7] "<reserved for future standard use>",
    [8] "user external interrupt",
    [9] "supervisor external interrupt",
    [10] "<reserved for future standard use>",
    [11] "<reserved for future standard use>",
    [12] "<reserved for future standard use>",
    [13] "<reserved for future standard use>",
    [14] "<reserved for future standard use>",
    [15] "<reserved for future standard use>",
  };
  static const char *nointr_desc[16] = {
    [0] "instruction address misaligned",
    [1] "instruction access fault",
    [2] "illegal instruction",
    [3] "breakpoint",
    [4] "load address misaligned",
    [5] "load access fault",
    [6] "store/AMO address misaligned",
    [7] "store/AMO access fault",
    [8] "environment call from U-mode",
    [9] "environment call from S-mode",
    [10] "<reserved for future standard use>",
    [11] "<reserved for future standard use>",
    [12] "instruction page fault",
    [13] "load page fault",
    [14] "<reserved for future standard use>",
    [15] "store/AMO page fault",
  };
  uint64 interrupt = stval & 0x8000000000000000L;
  uint64 code = stval & ~0x8000000000000000L;
  if (interrupt) {
    if (code < NELEM(intr_desc)) {
      return intr_desc[code];
    } else {
      return "<reserved for platform use>";
    }
  } else {
    if (code < NELEM(nointr_desc)) {
      return nointr_desc[code];
    } else if (code <= 23) {
      return "<reserved for future standard use>";
    } else if (code <= 31) {
      return "<reserved for custom use>";
    } else if (code <= 47) {
      return "<reserved for future standard use>";
    } else if (code <= 63) {
      return "<reserved for custom use>";
    } else {
      return "<reserved for future standard use>";
    }
  }
}

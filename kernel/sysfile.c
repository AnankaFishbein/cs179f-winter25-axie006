//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"
#include <stddef.h>

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op(ROOTDEV);
  if((ip = namei(old)) == 0){
    end_op(ROOTDEV);
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op(ROOTDEV);
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

  end_op(ROOTDEV);

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op(ROOTDEV);
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op(ROOTDEV);
  if((dp = nameiparent(path, name)) == 0){
    end_op(ROOTDEV);
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op(ROOTDEV);

  return 0;

bad:
  iunlockput(dp);
  end_op(ROOTDEV);
  return -1;
}

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

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

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

  begin_op(ROOTDEV);

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op(ROOTDEV);
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op(ROOTDEV);
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op(ROOTDEV);
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op(ROOTDEV);
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op(ROOTDEV);
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
    f->minor = ip->minor;
  } else {
    f->type = FD_INODE;
  }
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  iunlock(ip);
  end_op(ROOTDEV);

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op(ROOTDEV);
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op(ROOTDEV);
    return -1;
  }
  iunlockput(ip);
  end_op(ROOTDEV);
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op(ROOTDEV);
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op(ROOTDEV);
    return -1;
  }
  iunlockput(ip);
  end_op(ROOTDEV);
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op(ROOTDEV);
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op(ROOTDEV);
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op(ROOTDEV);
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op(ROOTDEV);
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      panic("sys_exec kalloc");
    if(fetchstr(uarg, argv[i], PGSIZE) < 0){
      goto bad;
    }
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// // kernel/sysfile.c
// uint64 
// sys_mmap(void) {
//   uint64 addr;
//   int length;
//   int prot, flags, fd;
//   int offset;

//   // 参数解析（修复类型匹配问题）
//   if (argaddr(0, &addr) < 0 || 
//       argaddr(1, (uint64*)&length) < 0 || 
//       argint(2, &prot) < 0 || 
//       argint(3, &flags) < 0 || 
//       argint(4, &fd) < 0 || 
//       argint(5, (int*)&offset) < 0) {
//     return -1;
//   }

//   // 校验参数合法性（根据题目要求）
//   if (addr != 0 || offset != 0) return -1;  // 仅支持 addr=0 和 offset=0
//   if (fd < 0 || fd >= NOFILE) return -1;    // 文件描述符有效性检查

//   struct proc *p = myproc();
//   struct file *f = p->ofile[fd];
//   if (f == 0) return -1;

//   // 检查权限（PROT_READ/PROT_WRITE 是否与文件模式匹配）
//   if ((prot & PROT_READ) && !f->readable) return -1;
//   if ((prot & PROT_WRITE) && !f->writable && flags == MAP_SHARED) return -1;

//   // 查找空闲 VMA 条目
//   int vma_idx = -1;
//   for (int i = 0; i < NVMA; i++) {
//     if (!p->vmas[i].valid) {
//       vma_idx = i;
//       break;
//     }
//   }
//   if (vma_idx == -1) return -1;  // VMA 已满

//   // 分配虚拟地址空间（从用户地址空间顶部向下分配）
//   // 假设用户空间布局：text | data | heap | stack | mmap 区域（从高地址向下）
//   uint64 start_va = p->sz;  // 当前进程的 sz 字段表示未分配的地址
//   p->sz += length;          // 更新进程的虚拟地址空间大小

//   // 初始化 VMA
//   p->vmas[vma_idx].addr = start_va;
//   p->vmas[vma_idx].length = length;
//   p->vmas[vma_idx].prot = prot;
//   p->vmas[vma_idx].flags = flags;
//   p->vmas[vma_idx].file = filedup(f);  // 增加文件引用计数
//   p->vmas[vma_idx].offset = 0;
//   p->vmas[vma_idx].valid = 1;

//   return start_va;  // 返回映射的虚拟地址
// }

// uint64 
// sys_munmap(void) {
//   uint64 addr;
//   unsigned int length;
//   argaddr(0, &addr);
//   argaddr(1, (uint64*)&length);
  
//   struct proc *p = myproc();
//   for (int i = 0; i < NVMA; i++) {
//     struct vma *vma = &p->vmas[i];
//     if (vma->valid && addr == vma->addr && length == vma->length) {
//       // 完整解除映射
//       if (vma->flags == MAP_SHARED) {
//         // 写回脏页（简化：遍历所有页并写回）
//         for (uint64 va = vma->addr; va < vma->addr + vma->length; va += PGSIZE) {
//           if (walkaddr(p->pagetable, va)) {
//             filewrite(vma->file, va, PGSIZE);
//           }
//         }
//       }
//       // 释放物理页和页表项
//       uvmunmap(p->pagetable, vma->addr, vma->length / PGSIZE, 1);
//       fileclose(vma->file);
//       vma->valid = 0;
//       return 0;
//     }
//   }
//   return -1;
// }

uint64 sys_mmap(void) {
  uint64 addr, length;
  int prot, flags, fd;
  struct file *f;
  struct proc *p = myproc();

  // 参数解析
  if (argaddr(0, &addr) || argaddr(1, &length) || 
      argint(2, &prot) || argint(3, &flags) || 
      argfd(4, &fd, &f)) 
    return -1;

  // 参数校验
  if (addr != 0) return -1; // 仅支持内核选择地址
  if (length <= 0 || (prot & ~(PROT_READ | PROT_WRITE)) || 
      (flags & ~(MAP_SHARED | MAP_PRIVATE))) 
    return -1;

  // 检查文件权限
  if ((flags == MAP_SHARED) && (prot & PROT_WRITE) && !f->writable)
    return -1;

  // 查找空闲 VMA 条目
  struct vma *vma = 0;
  for (int i = 0; i < NVMA; i++) {
    if (!p->vmas[i].valid) {
      vma = &p->vmas[i];
      break;
    }
  }
  if (!vma) return -1;

  // sysfile.c (sys_mmap)
uint64 start_va = p->sz;
uint64 end_va = PGROUNDUP(start_va + length);

// 检查地址是否超过用户空间上限
if (end_va >= TRAPFRAME) {
    p->killed = 1;
    return -1;
}

  // 初始化 VMA
  vma->addr = start_va;
  vma->length = end_va - start_va;
  vma->prot = prot;
  vma->flags = flags;
  vma->file = filedup(f); // 增加文件引用计数
  vma->offset = 0;
  vma->valid = 1;

  // 更新进程堆顶
  p->sz = end_va;

  // 设置页表权限（延迟分配，仅设置虚拟地址范围）
  // 实际物理页在 Page Fault 时分配
  return start_va;
}

// sysfile.c
uint64 sys_munmap(void) {
  uint64 addr;
  uint64 length;
  if (argaddr(0, &addr) || argaddr(1, &length))
    return -1;

  struct proc *p = myproc();
  struct vma *vma = 0;

  // 查找匹配的VMA
  for (int i = 0; i < NVMA; i++) {
    if (p->vmas[i].valid && p->vmas[i].addr == addr && p->vmas[i].length == length) {
      vma = &p->vmas[i];
      break;
    }
  }
  if (!vma) return -1;

  // 写回MAP_SHARED的修改
  if (vma->flags & MAP_SHARED) {
    for (uint64 off = 0; off < vma->length; off += PGSIZE) {
      uint64 va = vma->addr + off;
      pte_t *pte = walk(p->pagetable, va, 0);
      if (pte && (*pte & PTE_V)) {
        uint64 pa = PTE2PA(*pte);
        ilock(vma->file->ip);
        writei(vma->file->ip, 0, pa, off, PGSIZE);
        iunlock(vma->file->ip);
      }
    }
  }

  // 解除映射并释放物理页
  // sysfile.c (sys_munmap)
  uint64 npages = PGROUNDUP(vma->length) / PGSIZE; // 确保页数对齐
  uvmunmap(p->pagetable, vma->addr, npages, 1);
  fileclose(vma->file);
  vma->valid = 0;

  return 0;
}

int is_region_free(struct proc *p, uint64 start, uint64 end) {
  for (int i = 0; i < NVMA; i++) {
      struct vma *v = &p->vmas[i];
      if (v->valid && !(end <= v->addr || start >= v->addr + v->length)) {
          return 0; // 区域重叠
      }
  }
  return 1; // 区域空闲
}
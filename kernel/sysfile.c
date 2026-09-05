//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

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

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
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

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
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

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
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

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
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

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
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
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
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

//
// mmap lab: memory-mapped files.
//
// mmap records the mapping in a VMA but allocates no memory; pages
// are allocated and filled from the file on demand in
// mmap_page_fault(). munmap (and exit/exec) unmap the pages,
// writing modified MAP_SHARED pages back to the file.
//

// Find the VMA containing va, or 0.
static struct vmarea*
vma_find(struct proc *p, uint64 va)
{
  for(int i = 0; i < NVMA; i++)
    if(p->vma[i].used && va >= p->vma[i].addr && va < p->vma[i].addr + p->vma[i].len)
      return &p->vma[i];
  return 0;
}

uint64
sys_mmap(void)
{
  int length, prot, flags, fd, offset;
  struct proc *p = myproc();
  struct file *f;
  uint64 len, addr, lo;
  struct vmarea *v = 0;

  // the address argument must be 0: the kernel chooses the address
  if(argint(1, &length) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 ||
     argint(4, &fd) < 0 || argint(5, &offset) < 0)
    return -1;

  if(length <= 0)
    return -1;
  if(prot & ~(PROT_READ|PROT_WRITE|PROT_EXEC))
    return -1;
  // exactly one of MAP_SHARED and MAP_PRIVATE
  if(!(flags & MAP_SHARED) == !(flags & MAP_PRIVATE))
    return -1;
  if(fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0 || f->type != FD_INODE)
    return -1;
  // a shared mapping that can be written requires a writable file
  if((prot & PROT_WRITE) && (flags & MAP_SHARED) && !f->writable)
    return -1;

  for(int i = 0; i < NVMA; i++)
    if(!p->vma[i].used){
      v = &p->vma[i];
      break;
    }
  if(v == 0)
    return -1;

  len = PGROUNDUP(length);

  // place the mapping just below the lowest existing mapping,
  // top-down from the trapframe page.
  lo = TRAPFRAME;
  for(int i = 0; i < NVMA; i++)
    if(p->vma[i].used && p->vma[i].addr < lo)
      lo = p->vma[i].addr;
  addr = lo - len;
  if(addr < p->sz)
    return -1;

  v->used = 1;
  v->addr = addr;
  v->len = len;
  v->prot = prot;
  v->flags = flags;
  v->offset = offset;
  v->f = filedup(f);

  return addr;
}

// Unmap the pages of VMA v covering [a, a+len). Frees physical pages.
// If the mapping is MAP_SHARED, writes dirty pages back to the file
// (only the part of each page that lies within the file).
// The VMA itself is not modified.
static void
vma_unmap_pages(struct proc *p, struct vmarea *v, uint64 a, uint64 len)
{
  pte_t *pte;
  uint64 pa, va, off, n;
  struct inode *ip = v->f->ip;

  for(va = a; va < a + len; va += PGSIZE){
    if((pte = walk(p->pagetable, va, 0)) == 0 || (*pte & PTE_V) == 0)
      continue;
    pa = PTE2PA(*pte);
    if((v->flags & MAP_SHARED) && (*pte & PTE_D)){
      off = v->offset + (va - v->addr);
      if(off < ip->size){
        n = ip->size - off;
        if(n > PGSIZE)
          n = PGSIZE;
        begin_op();
        ilock(ip);
        writei(ip, 0, pa, off, n);
        iunlock(ip);
        end_op();
      }
    }
    kfree((void*)pa);
    *pte = 0;
  }
  // stale TLB entries may still point at the freed pages
  sfence_vma();
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int length;
  uint64 len, n;
  struct proc *p = myproc();
  struct vmarea *v;

  if(argaddr(0, &addr) < 0 || argint(1, &length) < 0)
    return -1;
  if(addr % PGSIZE != 0 || length <= 0)
    return -1;
  len = PGROUNDUP(length);

  if((v = vma_find(p, addr)) == 0)
    return -1;

  // clamp to the VMA
  n = v->addr + v->len - addr;
  if(n > len)
    n = len;

  // unmapping a middle hole splits the VMA; find the slot first
  // so we can fail before destroying anything.
  if(addr != v->addr && addr + n != v->addr + v->len){
    int i, free = 0;
    for(i = 0; i < NVMA; i++)
      if(!p->vma[i].used){
        free = 1;
        break;
      }
    if(!free)
      return -1;
  }

  vma_unmap_pages(p, v, addr, n);

  if(addr == v->addr){
    // unmaps the beginning (or all) of the VMA
    v->addr += n;
    v->offset += n;
    v->len -= n;
  } else if(addr + n == v->addr + v->len){
    // unmaps the end of the VMA
    v->len -= n;
  } else {
    // hole in the middle: split into a new VMA for the tail
    for(int i = 0; i < NVMA; i++){
      if(!p->vma[i].used){
        struct vmarea *t = &p->vma[i];
        t->used = 1;
        t->addr = addr + n;
        t->len = v->addr + v->len - (addr + n);
        t->prot = v->prot;
        t->flags = v->flags;
        t->offset = v->offset + (addr + n - v->addr);
        t->f = filedup(v->f);
        break;
      }
    }
    v->len = addr - v->addr;
  }

  if(v->len == 0){
    fileclose(v->f);
    v->f = 0;
    v->used = 0;
  }
  return 0;
}

// Handle a load (write==0) or store (write==1) page fault at va.
// Allocates a page, fills it from the file, and maps it.
// Returns 0 if the fault was handled, -1 otherwise (not an mmap
// page, or the access is not permitted).
int
mmap_page_fault(struct proc *p, uint64 va, int write)
{
  struct vmarea *v;
  pte_t *pte;
  char *mem;
  int perm = PTE_R | PTE_U;

  if((v = vma_find(p, va)) == 0)
    return -1;
  if(write && !(v->prot & PROT_WRITE))
    return -1;
  if(!write && !(v->prot & (PROT_READ|PROT_EXEC)))
    return -1;
  if((pte = walk(p->pagetable, va, 0)) != 0 && (*pte & PTE_V) != 0)
    return -1;  // already mapped; shouldn't fault

  mem = kalloc();
  if(mem == 0)
    return -1;
  memset(mem, 0, PGSIZE);

  // fill the page from the file; bytes past the end of the file
  // read as zeros (readi clamps to the file size)
  ilock(v->f->ip);
  readi(v->f->ip, 0, (uint64)mem, v->offset + (va - v->addr), PGSIZE);
  iunlock(v->f->ip);

  if(v->prot & PROT_WRITE)
    perm |= PTE_W;
  if(v->prot & PROT_EXEC)
    perm |= PTE_X;

  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0){
    kfree(mem);
    return -1;
  }
  return 0;
}

// Release all VMAs of p: write back shared dirty pages, free the
// mapped pages, and close the files. Called from exit() and from
// exec() after the old address space is replaced.
void
exitmmap(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    struct vmarea *v = &p->vma[i];
    if(v->used){
      vma_unmap_pages(p, v, v->addr, v->len);
      fileclose(v->f);
      v->f = 0;
      v->used = 0;
    }
  }
}

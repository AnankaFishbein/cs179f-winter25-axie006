#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__
// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
  uint n;
  uint nts;
};

#endif //__SPINKLOCK_H__
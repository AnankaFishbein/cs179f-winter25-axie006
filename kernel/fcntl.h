#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREATE  0x200

#define PROT_READ  0x010  // 可读权限
#define PROT_WRITE 0x020  // 可写权限
#define MAP_SHARED 0x000  // 共享映射
#define MAP_PRIVATE 0x001 // 私有映射
//or 0x001 and 0x002??

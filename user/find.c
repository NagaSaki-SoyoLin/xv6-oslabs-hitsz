//
// Created by 18256 on 2026/9/14.
//
#include "kernel/types.h"
#include "user.h"
#include "kernel/stat.h"
#include "kernel/fs.h"

// 取出路径最后一段名字, 例如: "./a/b" -> "b"
char *fmtname(char *path) {
  char *p;
  for (p = path + strlen(path); p >= path && *p != '/'; p--);
  return p + 1;  // 指向最后一段名字的首字母地址
}

// 递归查找文件, 输出 路径/文件名
void find(char *path, char *target) {
  char buf[512], *p;  // buf 存储下一次递归的完整路径, p 指向路径最后一个字符的下一个位置
  int fd;             // 文件描述符(0: 标准输入, 1: 标准输出, 2: 标准错误)
  struct stat st;     // 文件信息结构体, stat 表示 status, 即状态
  struct dirent de;   // 目录项结构体, dirent 表示 directory entry, 即目录项

  /*
   * open函数: 打开文件或目录
   * 参数1: 路径, 参数2: 打开方式(0: 只读, 1: 只写)
   * 返回值: 文件描述符, 失败返回-1
   */
  if ((fd = open(path, 0)) < 0) {
    printf("find: cannot open %s\n", path);
    return;
  }

  /*
   * fstat函数: 获取文件信息并存储在 stat 结构体中, f 表示 file, stat 表示 status
   * 参数1: 文件描述符, 参数2: 文件信息结构体指针
   * 返回值: 0(成功), -1(失败)
   */
  if (fstat(fd, &st) < 0) {
    printf("find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  if (strcmp(fmtname(path), target) == 0) {
    // 当前路径和目标文件名相同, 输出完整路径, 可以是目录也可以是文件
    printf("%s\n", path);
  }

  // 如果当前路径是目录, 递归查找子目录是否还有相同目标文件名
  if (st.type == T_DIR) {                              // stat.type: 文件类型(T_DIR: 目录, T_FILE: 文件, T_DEVICE: 设备)
    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {  // DIRSIZ 是目录项名称的最大长度
      printf("find: path too long\n");
      close(fd);
      return;
    }
    strcpy(buf, path);
    p = buf + strlen(buf);  // 指向路径最后一个字符的下一个位置
    *p++ = '/';             // 在路径最后一个字符后添加 '/' 并指向下一个位置

    /*
     * read函数: 从文件描述符中读取数据, 并存储在缓冲区中
     * 参数1: 文件描述符, 参数2: 缓冲区, 参数3: 读取大小
     * 返回值: 每次读取的字节数, 返回0表示读取到文件末尾, 返回-1表示读取失败
     */
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {  // 准备递归处理该路径下的每一个目录项
      // 如果文件是一个目录, 读这个目录的内容里是 dirent 结构体, 每个结构体代表一个目录项
      if (de.inum == 0) {  // dirent.inum 是目录项的 inode 编号, 0 表示空目录项
        continue;          // 跳过空目录项
      }
      /*
       * memmove函数: 将数据从源地址复制到目标地址, 可以用于字符串复制
       * mem 表示 memory, move 表示 move
       * 参数1: 目标地址, 参数2: 源地址, 参数3: 复制大小
       * 返回值: 目标地址
       */
      memmove(p, de.name, DIRSIZ);  // dirent.name 是目录项的名称
      p[DIRSIZ] = 0;                // 手动补 '\0', 因为 dirent.name 不保证有结尾
      if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
        continue;  // 跳过 "." 和 ".." 目录项
      }
      find(buf, target);  // 此时 buf 存储的是("path" + "/" + "dirent.name")的完整路径, 递归查找子目录
    }
  }
  close(fd);
}

int main(int argc, char *argv[]) {
  if (argc != 3) {  // 检查参数数量是否正确
    // 命令行格式: find <path> <name>
    printf("Usage: find <path> <name>\n");
    exit(1);
  }
  find(argv[1], argv[2]);
  exit(0);
}
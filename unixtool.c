/*
 * TI/LMI SYSV disk tool
 *
 *  Copyright 2019
 *  Daniel Seagraves <dseagrav@lunar-tokyo.net>
 *
 *  Copyright 2022
 *  Modified for TI S1500 by Jeffrey H. Johnson <trnsz@pobox.com>
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int disk_fd = -1;         /* FD for band image */
int file_fd = -1;         /* FD for source/target file */
char *disk_fname = NULL;  /* Band image filename */
uint8_t DISK_BLOCK[1024]; /* One disk block */

/* SYSV (68K) SUPERBLOCK - ON-DISK STRUCTURE */
/* ALL WORDS NEED BYTE-SWAPPED */
typedef struct rSuperBlock
{
  uint16_t isize;      /* SIZE OF BLOCKS IN I-LIST */
  uint32_t fsize;      /* SIZE OF BLOCKS IN THE VOLUME */
  uint16_t nfree;      /* NUMBER OF ADDRESSES IN FREE LIST */
  uint32_t free[50];   /* FREE BLOCK LIST */
  uint16_t ninode;     /* INODES IN FREE INODE LIST */
  uint16_t inode[100]; /* FREE INODE LIST */
  uint8_t flock;       /* FREE BLOCK LIST UPDATE LOCK */
  uint8_t ilock;       /* FREE INODE LIST UPDATE LOCK */
  uint8_t fmod;        /* SUPERBLOCK MODIFIED FLAG */
  uint8_t readonly;    /* READ-ONLY MOUNT FLAG */
  uint32_t time;       /* TIMESTAMP OF LAST SUPERBLOCK UPDATE */
  uint16_t dinfo[4];   /* DEVICE INFORMATION (?) */
  uint32_t tfree;      /* TOTAL FREE BLOCKS */
  uint16_t tinode;     /* TOTAL FREE INODES */
  char fname[6];       /* FILESYSTEM NAME */
  char fpack[6];       /* DISK PACK NAME */
  uint8_t fill[572];   /* FILL BYTES */
  uint32_t magic;      /* MAGIC NUMBER */
  uint32_t type;       /* FILESYSTEM TYPE */
} __attribute__ (( packed )) SuperBlock;

uint8_t superblock_buffer[1024]; /* BUFFER FOR HOLDING SUPERBLOCK */
SuperBlock *superblock = (SuperBlock *)&superblock_buffer;

/* INODE (on-disk representation) */
/* ALL WORDS NEED BYTE-SWAPPED */
typedef struct rInodeODR
{
  uint16_t mode;    /* MODE AND TYPE OF FILE */
  uint16_t nlink;   /* NUMBER OF LINKS TO HERE */
  uint16_t uid;
  uint16_t gid;
  uint32_t size;
  uint8_t addr[40]; /* DISK ADDRESS BYTES */
  uint32_t atime;   /* LAST ACCESS TIMESTAMP */
  uint32_t mtime;   /* LAST MODIFICATION TIMESTAMP */
  uint32_t ctime;   /* CREATION TIMESTAMP */
} __attribute__ (( packed )) InodeODR;

/* Inode (in-memory representation) */
typedef struct rInode
{
  uint16_t mode;     /* MODE BITS */
  uint16_t type;     /* TYPE BITS */
  uint16_t nlink;    /* NUMBER OF LINKS TO HERE */
  uint16_t uid;
  uint16_t gid;
  uint32_t size;
  uint32_t addr[13]; /* DISK ADDRESSES */
  time_t atime;      /* LAST ACCESS TIMESTAMP */
  time_t mtime;      /* LAST MODIFICATION TIMESTAMP */
  time_t ctime;      /* CREATION TIMESTAMP */
} Inode;

/* Inode file types */
#define INODE_FT_FIFO 1
#define INODE_FT_CHAR 2
#define INODE_FT_DIR 4
#define INODE_FT_BLK 6
#define INODE_FT_FILE 8

/* DIRECTORY ENTRY */
typedef struct rDirent
{
  uint16_t inode;
  unsigned char name[14];
} __attribute__ (( packed )) Dirent;

/* SWAP BYTES OF 32-BIT WORD */
uint32_t
swap_word(uint32_t in)
{
  uint32_t out = (( in & 0xFF000000 ) >> 24 );

  out |= (( in & 0x00FF0000 ) >> 8 );
  out |= (( in & 0x0000FF00 ) << 8 );
  out |= (( in & 0x000000FF ) << 24 );
  return out;
}

uint16_t
swap_hword(uint16_t in)
{
  uint32_t out = (( in & 0x00FF ) << 8 );

  out |= (( in & 0xFF00 ) >> 8 );
  return out;
}

/* READ INODE */
int
read_inode(int number, Inode *inode)
{
  /* Read the given inode */
  ssize_t io_res;
  off_t seek_res;
  off_t inode_disk_offset = 0x7C0;
  int x = 0;
  InodeODR raw_inode;

  inode_disk_offset += ( number * 0x40 );
  /* printf("read_inode(%d): diskaddr 0x%.8llx\n",number,inode_disk_offset); */
  seek_res = lseek(disk_fd, inode_disk_offset, SEEK_SET);
  if (seek_res < 0)
    {
      /* Seek error! */
      perror("read_inode():lseek()");
      return -1;
    }

  /* Read raw inode */
  io_res = read(disk_fd, (uint8_t *)&raw_inode, 0x40);
  if (io_res < 0)
    {
      /* Read error! */
      perror("read_inode():read()");
      return -1;
    }

  /* Read in inode particulars */
  inode->mode = ( raw_inode.mode & 0xFF00 ) >> 8;
  inode->mode |= (( raw_inode.mode & 0x000F ) << 8 );
  inode->type = ( raw_inode.mode & 0x00F0 ) >> 4;
  inode->nlink = swap_hword(raw_inode.nlink);
  inode->uid = swap_hword(raw_inode.uid);
  inode->gid = swap_hword(raw_inode.gid);
  inode->size = swap_word(raw_inode.size);
  inode->atime = swap_word(raw_inode.atime);
  inode->mtime = swap_word(raw_inode.mtime);
  inode->ctime = swap_word(raw_inode.ctime);
  /*
   * printf("inode %d: type %o mode %.5o owner %.6o:%.6o size %d\n",
   *  number,inode->type,inode->mode,inode->uid,inode->gid,inode->size);
   */

   /* Read in block addresses */
  while (x < 13)
    {
      inode->addr[x] = ( raw_inode.addr[x * 3] << 16 );
      inode->addr[x] |= ( raw_inode.addr[x * 3 + 1] << 8 );
      inode->addr[x] |= raw_inode.addr[x * 3 + 2];
      /*
       * if (inode->addr[x] > 0) {
       *   printf("block %d: %.6x\n",x,inode->addr[x]); }
       */
      x++;
    }
  /* Done */
  return 0;
}

int
disk_block_read(int adr, uint8_t *buf)
{
  ssize_t io_res; /* Result of read/write operations */
  /* Reposition the file pointer. */
  off_t seek_res = lseek(disk_fd, ( adr * 0x400 ), SEEK_SET);

  if (seek_res < 0)
    {
      /* Seek error! */
      perror("unixtool: disk lseek()");
      return -1;
    }

  /* Read in a sector. */
  io_res = read(disk_fd, buf, 1024);
  if (io_res < 0)
    {
      perror("unixtool: disk read()");
      return -1;
    }

  return io_res;
}

int
inode_block_read(int adr, Inode *inode, uint8_t *buf)
{
  int block = 0;
  int rv = 0;

  if (adr < 10)
    {
      /* Direct */
      block = inode->addr[adr];
      if (block > 0)
        {
          printf("inode_block_read(%d) => disk_block_read(%d)\n", adr, block);
          return disk_block_read(block, buf);
        }
      else
        {
          return 0; /* EOF */
        }
    }
  else
    {
      if (adr < 266)
        {
          /* One-level indirection */
          uint32_t indirect_block[256];
          int indirect_offset = ( adr - 10 );
          rv = disk_block_read(inode->addr[10], (uint8_t *)indirect_block);
          if (rv < 0)
            {
              return rv;
            }

          block = swap_word(indirect_block[indirect_offset]);
          printf(
            "inode_block_read(%d) => %d(%d) => disk_block_read(%d)\n",
            adr,
            inode->addr[10],
            indirect_offset,
            block);
          return disk_block_read(block, buf);
        }

      if (adr < 65802)
        {
          /* Two-level indirection */
          uint32_t first_indirect_block[256];
          int first_indirect_offset = ( adr - 266 ) / 256;
          uint32_t second_indirect_block[256];
          int second_indirect_offset = ( adr - 266 ) - ( first_indirect_offset * 256 );
          rv = disk_block_read(inode->addr[11], (uint8_t *)first_indirect_block);
          if (rv < 0)
            {
              return rv;
            }

          rv = disk_block_read(
            swap_word(first_indirect_block[first_indirect_offset]),
            (uint8_t *)second_indirect_block);
          if (rv < 0)
            {
              return rv;
            }

          block = swap_word(second_indirect_block[second_indirect_offset]);
          printf(
            "inode_block_read(%d) => %d(%d) => %d(%d) => disk_block_read(%d)\n",
            adr,
            inode->addr[11],
            first_indirect_offset,
            swap_word(first_indirect_block[first_indirect_offset]),
            second_indirect_offset,
            block);
          return disk_block_read(block, buf);
        }

      printf("Further indirection required\n");
      return -1;
    }

  /* Shouldn't get here! */
  printf("inode_block_read(): Fell off end!\n");
  return -1;
}

int
unix_ls(char *path)
{
  /* List the given path */
  Inode dir_inode;
  Inode next_dir_inode;
  char *pathpart;
  uint8_t dir_buffer[1024];
  int x = 0;        /* index into directory */
  int dirblock = 0; /* Block index into directory inode */
  int rv = 0;
  int done = 0;

  if (path[0] != '/')
    {
      printf("unixtool: ls: Invalid path\n");
      return -1;
    }

  /* Load the root directory */
  rv = read_inode(2, &dir_inode);
  if (rv < 0)
    {
      return rv;
    }

  /* Walk path */
  pathpart = strtok(path, "/");
  while (pathpart != NULL)
    {
      uint16_t dir_inode_number = 0;
      printf("pathpart: %s\n", pathpart);
      done = 0;
      while (!done)
        {
          Dirent *dir_entry = (Dirent *)&dir_buffer;
          rv = inode_block_read(dirblock, &dir_inode, dir_buffer);
          if (rv < 0)
            {
              return rv;
            }

          if (rv == 0)
            {
              return 0;
            } /* EOF, we are done */

          x = 0;
          dir_inode_number = 0;
          while (dir_entry->inode != 0 && x < 64)
            {
              /* Stuff */
              if (strlen(pathpart) > 14)
                {
                  /* Very funny. */
                  printf("unixtool: No such file or directory (in image).\n");
                  return 0;
                }

              /* printf("ent %d: %s\n",x,dir_entry->name); */
              if (strncmp(pathpart, (const char *)dir_entry->name, 14) == 0)
                {
                  /* FOUND! */
                  rv = read_inode(swap_hword(dir_entry->inode), &next_dir_inode);
                  if (rv < 0)
                    {
                      return rv;
                    }

                  if (next_dir_inode.type == INODE_FT_DIR)
                    {
                      /* Subdirectory */
                      dir_inode_number = swap_hword(dir_entry->inode);
                      /* printf("SUBDIRECTORY at %.4X\n",dir_inode_number); */
                      rv = read_inode(dir_inode_number, &dir_inode);
                      if (rv < 0)
                        {
                          printf("subdirectory read_inode() blew it!\n");
                          return rv;
                        }

                      dirblock = 0;
                    }

                  done = 1;
                  break;
                }

              dir_entry++;
              x++;
            }
          if (x < 64)
            {
              /* We are done! */
              done = 1;
            }
          else
            {
              /* Read next block */
              dirblock++;
            }
        }
      if (dir_inode_number == 0)
        {
          printf(
            "unixtool: No such file or directory (in image, path search).\n");
          return -1;
        }

      pathpart = strtok(NULL, "/");
    }
  /* Print loaded directory */
  done = 0;
  printf("%s:\n", path);
  while (!done)
    {
      Inode file_inode;
      Dirent *dir_entry = (Dirent *)&dir_buffer;
      rv = inode_block_read(dirblock, &dir_inode, dir_buffer);
      if (rv < 0)
        {
          return rv;
        }

      if (rv == 0)
        {
          return 0;
        } /* EOF, we are done */

      x = 0;
      while (dir_entry->inode != 0 && x < 64)
        {
          char permbuf[10] = "----------";
          char modtimebuf[32] = "";
          struct tm *modtime;
          rv = read_inode(swap_hword(dir_entry->inode), &file_inode);
          if (rv < 0)
            {
              return rv;
            }

          if (file_inode.type == INODE_FT_DIR)
            {
              permbuf[0] = 'd';
            } /* Directory */

          if (file_inode.type == INODE_FT_CHAR)
            {
              permbuf[0] = 'c';
            } /* Char special */

          if (file_inode.type == INODE_FT_BLK)
            {
              permbuf[0] = 'b';
            } /* Block special */

          if (file_inode.type == INODE_FT_FIFO)
            {
              permbuf[0] = 'p';
            } /* FIFO */

          if (file_inode.mode & 0400)
            {
              permbuf[1] = 'r';
            }

          if (file_inode.mode & 0200)
            {
              permbuf[2] = 'w';
            }

          if (file_inode.mode & 0100)
            {
              permbuf[3] = 'x';
            }

          if (file_inode.mode & 0040)
            {
              permbuf[4] = 'r';
            }

          if (file_inode.mode & 0020)
            {
              permbuf[5] = 'w';
            }

          if (file_inode.mode & 0010)
            {
              permbuf[6] = 'x';
            }

          if (file_inode.mode & 0004)
            {
              permbuf[7] = 'r';
            }

          if (file_inode.mode & 0002)
            {
              permbuf[8] = 'w';
            }

          if (file_inode.mode & 0001)
            {
              permbuf[9] = 'x';
            }

          modtime = localtime(&file_inode.mtime);
          if (modtime == NULL)
            {
              printf("localtime() blew it!\n");
              return -1;
            }

          if (strftime(modtimebuf, 31, "%b %e  %Y", modtime) == 0)
            {
              printf("strftime() blew it!\n");
              return -1;
            }

          printf(
            "%s  %2d %.6o  %.6o  %7d %s %s\n",
            permbuf,
            file_inode.nlink,
            file_inode.uid,
            file_inode.gid,
            file_inode.size,
            modtimebuf,
            dir_entry->name);
          dir_entry++;
          x++;
        }
      if (x < 64)
        {
          /* We are done! */
          done = 1;
        }
      else
        {
          /* Read next block */
          dirblock++;
        }
    }
  return 0;
}

int
unix_read(char *path, char *filename)
{
  /* Read path (from image) into filename (on host) */
  Inode dir_inode;
  Inode file_inode;
  char *pathpart;
  uint8_t dir_buffer[1024];
  uint8_t file_buffer[1024];
  unsigned int x = 0;   /* index into directory */
  int dirblock = 0;     /* Block index into directory inode */
  int fileblock = 0;    /* Block index into source file */
  int rv = 0;
  int done = 0;
  uint16_t dir_inode_number = 0;
  uint16_t file_inode_number = 0;

  /* Fast checks */
  if (path[0] != '/')
    {
      printf("unixtool: read: Invalid path\n");
      return -1;
    }

  /* Open target */
  file_fd = open(filename, O_RDWR | O_CREAT, 0660);
  if (file_fd < 0)
    {
      perror("unixtool:open()");
      return -1;
    }

  /* Load the root directory */
  rv = read_inode(2, &dir_inode);
  if (rv < 0)
    {
      return rv;
    }

  /* Walk path */
  pathpart = strtok(path, "/");
  while (pathpart != NULL)
    {
      if (file_inode_number != 0)
        {
          /* We already found a regular file? Something went wrong. */
          printf(
            "unixtool: No such file or directory (found regular file early "
            "in image).\n");
          return 0;
        }

      printf("pathpart: %s\n", pathpart);
      done = 0;
      while (!done)
        {
          Dirent *dir_entry = (Dirent *)&dir_buffer;
          rv = inode_block_read(dirblock, &dir_inode, dir_buffer);
          if (rv < 0)
            {
              return rv;
            }

          if (rv == 0)
            {
              return 0;
            } /* EOF, we are done */

          x = 0;
          dir_inode_number = 0;
          while (dir_entry->inode != 0 && x < 64)
            {
              /* Stuff */
              if (strlen(pathpart) > 14)
                {
                  /* Very funny. */
                  printf("unixtool: No such file or directory (in image).\n");
                  return 0;
                }

              /* printf("ent %d: %s\n",x,dir_entry->name); */
              if (strncmp(pathpart, (const char *)dir_entry->name, 14) == 0)
                {
                  /* FOUND! */
                  rv = read_inode(swap_hword(dir_entry->inode), &file_inode);
                  if (rv < 0)
                    {
                      return rv;
                    }

                  if (file_inode.type == INODE_FT_DIR)
                    {
                      /* Subdirectory */
                      dir_inode_number = swap_hword(dir_entry->inode);
                      /* printf("SUBDIRECTORY at %.4X\n",dir_inode_number); */
                      rv = read_inode(dir_inode_number, &dir_inode);
                      if (rv < 0)
                        {
                          return rv;
                        }

                      dirblock = 0;
                    }

                  if (file_inode.type == INODE_FT_FILE)
                    {
                      /* Regular file */
                      file_inode_number = swap_hword(dir_entry->inode);
                    }

                  done = 1;
                  break;
                }

              dir_entry++;
              x++;
            }
          if (x < 64)
            {
              /* We are done! */
              done = 1;
            }
          else
            {
              /* Read next block */
              dirblock++;
            }
        }
      if (dir_inode_number == 0 && file_inode_number == 0)
        {
          printf(
            "unixtool: No such file or directory (in image, path search).\n");
          return -1;
        }

      /* Otherwise obtain next part and carry on */
      pathpart = strtok(NULL, "/");
    }
  if (file_inode_number == 0)
    {
      printf("unixtool: No such file or directory (in image, file read).\n");
      return 0;
    }

  /* Do the deed! */
  printf("Copying %d bytes\n", file_inode.size);
  x = 0;
  while (x < file_inode.size)
    {
      int osize = 1024;
      if (( file_inode.size - x ) < 1024)
        {
          osize = file_inode.size - x;
        }

      rv = inode_block_read(fileblock, &file_inode, file_buffer);
      if (rv < 0)
        {
          return rv;
        }

      if (rv == 0)
        {
          /* EOF, we are done early? */
          printf("unixtool: Unexpected end-of-file\n");
          return -1;
        }

      rv = write(file_fd, file_buffer, osize);
      if (rv < 0)
        {
          perror("unixtool:write()");
          return -1;
        }

      x += osize;
      fileblock++;
    }
  printf("Wrote %d of %d bytes\n", x, file_inode.size);
  close(file_fd);
  return 0;
}

int
main(int argc, char *argv[])
{
  int rv = 0;

  /* Sigh. */
  if (sizeof ( SuperBlock ) != 1024)
    {
      printf("SOMETHING IS BROKEN: SUPERBLOCK SIZE %ld\n", sizeof ( SuperBlock ));
      return -1;
    }

  if (argc < 2 || strncmp(argv[1], "help", 4) == 0
      || strncmp(argv[1], "-?", 2) == 0)
    {
      /* Handle command-line options */
      printf("TI/LMI unixtool v0.0.1\n\n");
      printf(
        "Usage: unixtool <command> <image file> [parameters]...\n\n");
      printf(" Commands:\n\n");
      printf("   help       Prints this information\n\n");
      printf("   ls         Lists the given directory\n");
      printf("                Parameters: <directory>\n\n");
      printf("   read       Copy path from image file to destination\n");
      printf("                Parameters: <source path> <destination>\n\n");
      return 0;
    }

  if (argc < 3)
    {
      printf(
        "unixtool: Band image file name is required; See \"unixtool "
        "help\" for usage information.\n");
      return -1;
    }

  /* We have a disk filename, so open it. */
  disk_fname = argv[2];
  disk_fd = open(disk_fname, O_RDWR);
  if (disk_fd < 0)
    {
      perror("unixtool: disk open()");
      return -1;
    }

  /* Read in and check superblock */
  rv = disk_block_read(1, superblock_buffer);
  if (rv < 0)
    {
      return rv;
    }

  if (superblock->magic != 0x207E18FD)
    {
      printf(
        "unixtool: Bad superblock magic: Expected 0x207E18FD, got 0x%.8X\n",
        superblock->magic);
      return -1;
    }

  /* Select option (or bail) */
  if (argc >= 3)
    {
      if (strncmp(argv[1], "ls", 2) == 0)
        {
          if (argc < 4)
            {
              printf("unixtool: ls: directory path is required\n");
              return -1;
            }

          return unix_ls(argv[3]);
        }

      if (strncmp(argv[1], "read", 4) == 0)
        {
          if (argc < 4)
            {
              printf("unixtool: read: source path is required\n");
              return -1;
            }

          if (argc < 5)
            {
              printf("unixtool: read: destination path is required\n");
              return -1;
            }

          return unix_read(argv[3], argv[4]);
        }

      printf(
        "unixtool: Unknown parameters; See \"unixtool help\" for usage "
        "information.\n");
      return -1;
    }
}

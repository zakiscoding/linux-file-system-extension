/* SPDX-License-Identifier: GPL-2.0-only */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "edufs_disk.h"
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s (errno=%d %s)\n",__LINE__,#x,errno,strerror(errno)); exit(1); } } while (0)
static uint32_t seed=4523;
static uint32_t random32(void) { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; }
static void remount(void) { REQUIRE(umount("/mnt")==0); REQUIRE(mount("/dev/vda","/mnt","edufs",0,NULL)==0); }
static unsigned long free_blocks(void) { struct statvfs st; REQUIRE(statvfs("/mnt",&st)==0); return st.f_bfree; }
static void verify(const char *path,const unsigned char *expected,size_t size)
{
    struct stat st; unsigned char buf[8192]; size_t offset=0;
    int fd=open(path,O_RDONLY); REQUIRE(fd>=0); REQUIRE(fstat(fd,&st)==0); REQUIRE(st.st_size==(off_t)size);
    while(offset<size) { size_t chunk=size-offset<sizeof(buf)?size-offset:sizeof(buf);
        REQUIRE(read(fd,buf,chunk)==(ssize_t)chunk); REQUIRE(!memcmp(buf,expected+offset,chunk)); offset+=chunk; }
    REQUIRE(read(fd,buf,1)==0); REQUIRE(close(fd)==0);
}
static void injection(int value)
{
    char text[32]; int fd=open("/sys/module/edufs/parameters/fail_alloc_after",O_WRONLY); REQUIRE(fd>=0);
    int len=snprintf(text,sizeof(text),"%d\n",value); REQUIRE(write(fd,text,(size_t)len)==len); REQUIRE(close(fd)==0);
}
static void boundaries(void)
{
    unsigned char *data=malloc(EDUFS_MAX_FILE+1u); REQUIRE(data);
    for(size_t n=0;n<EDUFS_MAX_FILE+1u;n++) data[n]=(unsigned char)(n*31u+n/251u);
    const size_t sizes[]={0,1,4095,4096,4097,49151,49152,49153,53248,EDUFS_MAX_FILE};
    for(size_t n=0;n<sizeof(sizes)/sizeof(sizes[0]);n++) {
        int fd=open("/mnt/boundary",O_CREAT|O_TRUNC|O_RDWR,0644); REQUIRE(fd>=0);
        REQUIRE(write(fd,data,sizes[n])==(ssize_t)sizes[n]); REQUIRE(fsync(fd)==0); REQUIRE(close(fd)==0);
        remount(); verify("/mnt/boundary",data,sizes[n]);
    }
    int fd=open("/mnt/boundary",O_RDWR); REQUIRE(fd>=0);
    errno=0; REQUIRE(pwrite(fd,data,1,EDUFS_MAX_FILE)==-1); REQUIRE(errno==EFBIG);
    REQUIRE(close(fd)==0); verify("/mnt/boundary",data,EDUFS_MAX_FILE);
    REQUIRE(unlink("/mnt/boundary")==0); free(data);
    puts("PASS: boundaries, maximum size, clean remount persistence");
}
static void rollback(void)
{
    unsigned char data[12*EDUFS_BLOCK_SIZE]; memset(data,0x53,sizeof(data));
    int fd=open("/mnt/rollback",O_CREAT|O_RDWR,0644); REQUIRE(fd>=0);
    REQUIRE(write(fd,data,sizeof(data))==(ssize_t)sizeof(data)); unsigned long before=free_blocks();
    for(int n=0;n<3;n++) {
        injection(n); errno=0; REQUIRE(pwrite(fd,data,2*EDUFS_BLOCK_SIZE,sizeof(data))==-1); REQUIRE(errno==ENOSPC);
        injection(-1); REQUIRE(free_blocks()==before); verify("/mnt/rollback",data,sizeof(data));
    }
    REQUIRE(close(fd)==0); remount(); verify("/mnt/rollback",data,sizeof(data));
    unsigned char existing[13*EDUFS_BLOCK_SIZE]; memset(existing,0x53,sizeof(existing));
    fd=open("/mnt/rollback",O_RDWR); REQUIRE(fd>=0);
    REQUIRE(pwrite(fd,data,EDUFS_BLOCK_SIZE,sizeof(data))==EDUFS_BLOCK_SIZE);
    before=free_blocks(); injection(1); errno=0;
    REQUIRE(pwrite(fd,data,2*EDUFS_BLOCK_SIZE,sizeof(existing))==-1); REQUIRE(errno==ENOSPC);
    injection(-1); REQUIRE(free_blocks()==before); REQUIRE(fsync(fd)==0); REQUIRE(close(fd)==0);
    remount(); verify("/mnt/rollback",existing,sizeof(existing)); REQUIRE(unlink("/mnt/rollback")==0);
    puts("PASS: allocation failure injection and indirect-block rollback");
}
static void randomized(void)
{
    enum { FILES=8, LIMIT=100000 };
    unsigned char *model[FILES]={0}; size_t sizes[FILES]={0}; int exists[FILES]={0};
    for(int n=0;n<FILES;n++) { model[n]=calloc(1,LIMIT); REQUIRE(model[n]); }
    for(int op=0;op<160;op++) {
        unsigned n=random32()%FILES, action=random32()%5; char path[64]; snprintf(path,sizeof(path),"/mnt/random%u",n);
        if(action==0 && exists[n]) {
            REQUIRE(unlink(path)==0); exists[n]=0; sizes[n]=0; memset(model[n],0,LIMIT);
        } else {
            int fd=open(path,O_CREAT|O_RDWR,0644); REQUIRE(fd>=0); exists[n]=1;
            if(action==1) {
                size_t target=random32()%LIMIT; REQUIRE(ftruncate(fd,(off_t)target)==0);
                if(target<sizes[n]) memset(model[n]+target,0,LIMIT-target);
                sizes[n]=target;
            } else {
                unsigned char data[6000]; size_t offset=random32()%(LIMIT-sizeof(data)), length=random32()%sizeof(data)+1;
                for(size_t j=0;j<length;j++) data[j]=(unsigned char)random32();
                REQUIRE(pwrite(fd,data,length,(off_t)offset)==(ssize_t)length);
                memcpy(model[n]+offset,data,length); if(offset+length>sizes[n]) sizes[n]=offset+length;
            }
            REQUIRE(close(fd)==0);
        }
        if(op%10==0) remount();
        for(unsigned j=0;j<FILES;j++) if(exists[j]) {
            snprintf(path,sizeof(path),"/mnt/random%u",j); verify(path,model[j],sizes[j]);
        }
    }
    for(unsigned n=0;n<FILES;n++) {
        char path[64]; snprintf(path,sizeof(path),"/mnt/random%u",n); if(exists[n]) REQUIRE(unlink(path)==0); free(model[n]);
    }
    puts("PASS: 160 seeded VFS operations with truncate, gap zeroing, and remounts");
}
static void unlink_open(void)
{
    char data[8]={0}; int fd=open("/mnt/unlinked",O_CREAT|O_RDWR,0644); REQUIRE(fd>=0); unsigned long before=free_blocks();
    REQUIRE(write(fd,"hello",5)==5); REQUIRE(unlink("/mnt/unlinked")==0);
    REQUIRE(pread(fd,data,5,0)==5); REQUIRE(!memcmp(data,"hello",5)); REQUIRE(close(fd)==0); REQUIRE(free_blocks()==before);
    puts("PASS: open-unlink lifetime and close reclamation");
}
static void inode_and_faults(void)
{
    unsigned long before=free_blocks(); char path[64];
    for(unsigned n=0;n<126;n++) {
        snprintf(path,sizeof(path),"/mnt/inode%u",n);
        int fd=open(path,O_CREAT|O_RDWR,0644); REQUIRE(fd>=0); REQUIRE(close(fd)==0);
    }
    errno=0; REQUIRE(open("/mnt/overflow",O_CREAT|O_RDWR,0644)==-1); REQUIRE(errno==ENOSPC);
    REQUIRE(unlink("/mnt/inode0")==0);
    int fd=open("/mnt/reused",O_CREAT|O_RDWR|O_APPEND,0644); REQUIRE(fd>=0);
    REQUIRE(write(fd,"hello",5)==5); REQUIRE(pwrite(fd,"!",1,0)==1);
    errno=0; REQUIRE(syscall(SYS_write,fd,(void *)(uintptr_t)-1,8)==-1); REQUIRE(errno==EFAULT);
    REQUIRE(close(fd)==0); verify("/mnt/reused",(const unsigned char *)"hello!",6);
    REQUIRE(unlink("/mnt/reused")==0);
    for(unsigned n=1;n<126;n++) { snprintf(path,sizeof(path),"/mnt/inode%u",n); REQUIRE(unlink(path)==0); }
    REQUIRE(free_blocks()==before); remount();
    puts("PASS: inode exhaustion, inode reuse, append, invalid user buffer");
}
static void disk_full(void)
{
    const size_t chunk=1024*1024; unsigned char *data=malloc(chunk); REQUIRE(data); memset(data,0x8d,chunk);
    unsigned long before=free_blocks(); int full=0,files=0;
    for(int n=0;n<40 && !full;n++) {
        char path[64]; snprintf(path,sizeof(path),"/mnt/fill%d",n); int fd=open(path,O_CREAT|O_RDWR,0644); REQUIRE(fd>=0); files++;
        for(int j=0;j<4;j++) {
            unsigned long available=free_blocks(); ssize_t result=write(fd,data,chunk);
            if(result<0) { REQUIRE(errno==ENOSPC); REQUIRE(free_blocks()==available); full=1; break; }
            REQUIRE(result==(ssize_t)chunk);
        }
        REQUIRE(close(fd)==0);
    }
    REQUIRE(full); remount();
    for(int n=0;n<files;n++) { char path[64]; snprintf(path,sizeof(path),"/mnt/fill%d",n); REQUIRE(unlink(path)==0); }
    REQUIRE(free_blocks()==before); free(data); remount();
    puts("PASS: actual disk-full writes and complete block reclamation");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0); boundaries(); rollback(); randomized(); unlink_open(); inode_and_faults(); disk_full();
    puts("EDUFS VFS TESTS PASS"); return 0;
}

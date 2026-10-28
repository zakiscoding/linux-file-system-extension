/* SPDX-License-Identifier: GPL-2.0-only */
/* Offline image tool. Never use on an image mounted by a guest. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "edufs_disk.h"

typedef struct { unsigned char *p; size_t bytes; uint32_t blocks; } image;
_Noreturn static void die(const char *s) { fprintf(stderr, "edufs: %s\n", s); exit(1); }
static uint32_t rd(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void wr(unsigned char *p, uint32_t n)
{ p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8); p[2]=(unsigned char)(n>>16); p[3]=(unsigned char)(n>>24); }
static unsigned char *blk(image *im, uint32_t n)
{ if(n>=im->blocks) die("invalid block reference"); return im->p+(size_t)n*EDUFS_BLOCK_SIZE; }
static unsigned char *ino(image *im, uint32_t n)
{ return blk(im,EDUFS_ITABLE_BLOCK)+(size_t)n*EDUFS_INODE_SIZE; }
static unsigned char *ent(image *im, uint32_t n)
{ return blk(im,EDUFS_DIRECTORY_BLOCK)+(size_t)n*EDUFS_DIRENT_SIZE; }
static int bit(const unsigned char *p, uint32_t n) { return (p[n/8]>>(n%8))&1; }
static void setbit(unsigned char *p, uint32_t n, int used)
{ if(used) p[n/8]|=(unsigned char)(1u<<(n%8)); else p[n/8]&=(unsigned char)~(1u<<(n%8)); }
static image load(const char *path)
{
    image im={0}; FILE *f=fopen(path,"rb"); long bytes;
    if(!f) die("cannot open image");
    if(fseek(f,0,SEEK_END) || (bytes=ftell(f))<0 || fseek(f,0,SEEK_SET)) die("cannot size image");
    if(bytes<(long)(EDUFS_MIN_BLOCKS*EDUFS_BLOCK_SIZE) ||
       bytes>(long)(EDUFS_VOLUME_MAX_BLOCKS*EDUFS_BLOCK_SIZE) || bytes%EDUFS_BLOCK_SIZE) die("invalid image length");
    im.bytes=(size_t)bytes; im.blocks=(uint32_t)(im.bytes/EDUFS_BLOCK_SIZE); im.p=malloc(im.bytes);
    if(!im.p || fread(im.p,1,im.bytes,f)!=im.bytes) die("cannot read image");
    if(fclose(f)) die("cannot close image");
    return im;
}
static void save(image *im, const char *path, int create)
{
    /* Exclusive creation prevents accidentally formatting an existing image. */
    FILE *f=fopen(path,create?"wbx":"r+b");
    if(!f) die(create?"image already exists or cannot be created":"cannot open image for update");
    if(fwrite(im->p,1,im->bytes,f)!=im->bytes) die("image write failed");
    if(fclose(f)) die("image close failed");
}
static void claim(image *im, unsigned char *seen, uint32_t b)
{
    if(b<EDUFS_DATA_BLOCK || b>=im->blocks) die("out-of-range data block");
    if(bit(seen,b)) die("duplicate block reference");
    setbit(seen,b,1);
}
static void check(image *im)
{
    unsigned char seen[EDUFS_BLOCK_SIZE]={0};
    const uint32_t fields[]={EDUFS_MAGIC,EDUFS_VERSION,EDUFS_BLOCK_SIZE,im->blocks,EDUFS_INODES,
        EDUFS_BITMAP_BLOCK,EDUFS_IMAP_BLOCK,EDUFS_ITABLE_BLOCK,EDUFS_DIRECTORY_BLOCK,EDUFS_DATA_BLOCK};
    for(size_t j=0;j<sizeof(fields)/sizeof(fields[0]);j++)
        if(rd(im->p+4*j)!=fields[j]) die("invalid superblock");
    for(uint32_t j=0;j<EDUFS_DATA_BLOCK;j++) setbit(seen,j,1);
    if(!bit(blk(im,EDUFS_IMAP_BLOCK),0) || !bit(blk(im,EDUFS_IMAP_BLOCK),1)) die("reserved inode is free");
    unsigned char *root=ino(im,1);
    if(rd(root)!=2 || rd(root+4)!=2*EDUFS_BLOCK_SIZE || rd(root+8)!=7 || rd(root+12)!=8) die("invalid root inode");
    for(uint32_t j=16;j<EDUFS_INODE_SIZE;j++) if(root[j]) die("invalid root inode tail");
    for(uint32_t j=0;j<EDUFS_INODE_SIZE;j++) if(ino(im,0)[j]) die("invalid reserved inode");
    for(uint32_t j=0;j<2*EDUFS_DIRENT_SIZE;j++) if(ent(im,0)[j]) die("reserved directory slot used");
    for(uint32_t n=2;n<EDUFS_INODES;n++) {
        unsigned char *ip=ino(im,n), *ep=ent(im,n);
        if(!bit(blk(im,EDUFS_IMAP_BLOCK),n)) {
            for(uint32_t j=0;j<EDUFS_INODE_SIZE;j++) if(ip[j]) die("unallocated inode has metadata");
            for(uint32_t j=0;j<EDUFS_DIRENT_SIZE;j++) if(ep[j]) die("orphan directory entry");
            continue;
        }
        if(rd(ip)!=1 || rd(ip+4)>EDUFS_MAX_FILE) die("invalid file inode");
        if(rd(ep)!=n || !ep[4] || !memchr(ep+4,0,60) || strchr((char*)ep+4,'/') ||
            !strcmp((char*)ep+4,".") || !strcmp((char*)ep+4,"..")) die("invalid directory entry");
        for(uint32_t k=2;k<n;k++) if(rd(ent(im,k)) && !strcmp((char*)ent(im,k)+4,(char*)ep+4)) die("duplicate filename");
        uint32_t count=(rd(ip+4)+EDUFS_BLOCK_SIZE-1)/EDUFS_BLOCK_SIZE;
        uint32_t indirect=rd(ip+56);
        if(count>EDUFS_DIRECT) claim(im,seen,indirect);
        else if(indirect) die("unnecessary indirect block");
        for(uint32_t j=0;j<EDUFS_DIRECT;j++) {
            uint32_t b=rd(ip+8+4*j);
            if(j<count) claim(im,seen,b); else if(b) die("pointer beyond end of file");
        }
        if(indirect) for(uint32_t j=0;j<EDUFS_INDIRECT;j++) {
            uint32_t b=rd(blk(im,indirect)+4*j);
            if(j+EDUFS_DIRECT<count) claim(im,seen,b); else if(b) die("indirect pointer beyond end of file");
        }
        for(uint32_t j=60;j<EDUFS_INODE_SIZE;j++) if(ip[j]) die("nonzero inode reserved field");
    }
    for(uint32_t b=0;b<EDUFS_VOLUME_MAX_BLOCKS;b++)
        if(bit(blk(im,EDUFS_BITMAP_BLOCK),b)!=(b<im->blocks?bit(seen,b):1)) die("block bitmap mismatch or leaked block");
    for(uint32_t n=EDUFS_INODES;n<EDUFS_BLOCK_SIZE*8;n++)
        if(!bit(blk(im,EDUFS_IMAP_BLOCK),n)) die("invalid inode bitmap padding");
}
static uint32_t lookup(image *im,const char *name)
{
    for(uint32_t n=2;n<EDUFS_INODES;n++) if(rd(ent(im,n)) && !strcmp((char*)ent(im,n)+4,name)) return n;
    return 0;
}
static uint32_t allocate(image *im)
{
    unsigned char *map=blk(im,EDUFS_BITMAP_BLOCK);
    for(uint32_t b=EDUFS_DATA_BLOCK;b<im->blocks;b++) if(!bit(map,b)) {
        setbit(map,b,1); memset(blk(im,b),0,EDUFS_BLOCK_SIZE); return b;
    }
    die("disk full; image unchanged"); return 0;
}
static uint32_t data_block(image *im,unsigned char *ip,uint32_t j)
{ return j<EDUFS_DIRECT?rd(ip+8+4*j):rd(blk(im,rd(ip+56))+4*(j-EDUFS_DIRECT)); }
static void remove_file(image *im,uint32_t n)
{
    unsigned char *ip=ino(im,n); uint32_t count=(rd(ip+4)+EDUFS_BLOCK_SIZE-1)/EDUFS_BLOCK_SIZE;
    for(uint32_t j=0;j<count;j++) setbit(blk(im,EDUFS_BITMAP_BLOCK),data_block(im,ip,j),0);
    if(rd(ip+56)) setbit(blk(im,EDUFS_BITMAP_BLOCK),rd(ip+56),0);
    memset(ip,0,EDUFS_INODE_SIZE); memset(ent(im,n),0,EDUFS_DIRENT_SIZE);
    setbit(blk(im,EDUFS_IMAP_BLOCK),n,0);
}
static void put(image *im,const char *name,const char *source)
{
    if(!*name || strlen(name)>EDUFS_NAME_MAX || strchr(name,'/') || !strcmp(name,".") || !strcmp(name,"..")) die("invalid filename");
    FILE *f=fopen(source,"rb"); long length;
    if(!f) die("cannot open input");
    if(fseek(f,0,SEEK_END) || (length=ftell(f))<0 || fseek(f,0,SEEK_SET)) die("cannot size input");
    if((unsigned long)length>EDUFS_MAX_FILE) die("file too large; image unchanged");
    uint32_t n=lookup(im,name);
    if(n) remove_file(im,n);
    else for(n=2;n<EDUFS_INODES && bit(blk(im,EDUFS_IMAP_BLOCK),n);n++) {}
    if(n==EDUFS_INODES) die("inode table full; image unchanged");
    unsigned char *ip=ino(im,n); uint32_t count=((uint32_t)length+EDUFS_BLOCK_SIZE-1)/EDUFS_BLOCK_SIZE;
    setbit(blk(im,EDUFS_IMAP_BLOCK),n,1); wr(ip,1); wr(ip+4,(uint32_t)length);
    if(count>EDUFS_DIRECT) wr(ip+56,allocate(im));
    for(uint32_t j=0;j<count;j++) {
        uint32_t b=allocate(im), remaining=(uint32_t)length-j*EDUFS_BLOCK_SIZE;
        wr(j<EDUFS_DIRECT?ip+8+4*j:blk(im,rd(ip+56))+4*(j-EDUFS_DIRECT),b);
        size_t bytes=remaining<EDUFS_BLOCK_SIZE?remaining:EDUFS_BLOCK_SIZE;
        if(fread(blk(im,b),1,bytes,f)!=bytes) die("input read failed; image unchanged");
    }
    if(fclose(f)) die("input close failed; image unchanged");
    wr(ent(im,n),n); strcpy((char*)ent(im,n)+4,name);
}
static image format(uint32_t blocks)
{
    image im={0}; im.blocks=blocks; im.bytes=(size_t)blocks*EDUFS_BLOCK_SIZE;
    im.p=calloc(1,im.bytes); if(!im.p) die("out of memory");
    const uint32_t fields[]={EDUFS_MAGIC,EDUFS_VERSION,EDUFS_BLOCK_SIZE,blocks,EDUFS_INODES,1,2,3,7,9};
    for(size_t j=0;j<sizeof(fields)/sizeof(fields[0]);j++) wr(im.p+4*j,fields[j]);
    memset(blk(&im,EDUFS_BITMAP_BLOCK),255,EDUFS_BLOCK_SIZE);
    for(uint32_t b=EDUFS_DATA_BLOCK;b<blocks;b++) setbit(blk(&im,EDUFS_BITMAP_BLOCK),b,0);
    memset(blk(&im,EDUFS_IMAP_BLOCK),255,EDUFS_BLOCK_SIZE);
    for(uint32_t n=2;n<EDUFS_INODES;n++) setbit(blk(&im,EDUFS_IMAP_BLOCK),n,0);
    wr(ino(&im,1),2); wr(ino(&im,1)+4,8192); wr(ino(&im,1)+8,7); wr(ino(&im,1)+12,8);
    return im;
}
int main(int argc,char **argv)
{
    if(argc<3) die("usage: edufs mkfs IMAGE BLOCKS | check IMAGE | ls IMAGE | put IMAGE NAME INPUT | get IMAGE NAME OUTPUT | rm IMAGE NAME");
    if(!strcmp(argv[1],"mkfs")) {
        char *end; unsigned long blocks;
        if(argc!=4) die("mkfs requires image and block count");
        errno=0; blocks=strtoul(argv[3],&end,10);
        if(errno || !*argv[3] || *end || blocks<EDUFS_MIN_BLOCKS || blocks>EDUFS_VOLUME_MAX_BLOCKS) die("invalid block count (10..32768)");
        image im=format((uint32_t)blocks); check(&im); save(&im,argv[2],1); free(im.p); return 0;
    }
    image im=load(argv[2]); check(&im);
    if(!strcmp(argv[1],"check") && argc==3) puts("EDUFS CHECK PASS");
    else if(!strcmp(argv[1],"ls") && argc==3) {
        for(uint32_t n=2;n<EDUFS_INODES;n++) if(rd(ent(&im,n))) printf("%u %s\n",rd(ino(&im,n)+4),ent(&im,n)+4);
    } else if(!strcmp(argv[1],"put") && argc==5) { put(&im,argv[3],argv[4]); check(&im); save(&im,argv[2],0); }
    else if(!strcmp(argv[1],"rm") && argc==4) {
        uint32_t n=lookup(&im,argv[3]); if(!n) die("file not found"); remove_file(&im,n); check(&im); save(&im,argv[2],0);
    } else if(!strcmp(argv[1],"get") && argc==5) {
        if(!strcmp(argv[2],argv[4])) die("output must differ from image");
        uint32_t n=lookup(&im,argv[3]); if(!n) die("file not found");
        /* Exclusive output also avoids aliases overwriting the image. */
        FILE *out=fopen(argv[4],"wbx"); if(!out) die("output exists or cannot be created");
        unsigned char *ip=ino(&im,n); uint32_t left=rd(ip+4),j=0;
        while(left) { size_t bytes=left<EDUFS_BLOCK_SIZE?left:EDUFS_BLOCK_SIZE;
            if(fwrite(blk(&im,data_block(&im,ip,j++)),1,bytes,out)!=bytes) die("output write failed");
            left-=(uint32_t)bytes;
        }
        if(fclose(out)) die("output close failed");
    } else die("unknown command or wrong argument count");
    free(im.p); return 0;
}

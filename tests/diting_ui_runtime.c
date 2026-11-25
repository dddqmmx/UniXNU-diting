/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Exercises the actual UI installer with a bounded, typed vnode fixture.
 * No device I/O. GPU identity/files/counters are fixture invariants. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <zlib.h>
#include "DitingGpuRuntimeArchive.h"

#ifndef EAUTH
#define EAUTH 80
#endif
enum vtype { VREG, VDIR };
struct Node {char path[256]; bool active; enum vtype type; uint32_t mode, uid, gid; size_t bytes; uint8_t *data;};
typedef struct Node *vnode_t;
typedef void *proc_t;
typedef uintptr_t user_addr_t;
typedef void *vfs_context_t;
static struct Node nodes[24], root_node;
static vnode_t rootvnode = &root_node;
#define NULLVP NULL
#define Z_WAITOK 1
#define Z_ZERO 2
#define FREAD 1
#define FWRITE 2
#define UIO_READ 0
#define UIO_WRITE 1
#define UIO_SYSSPACE 0
#define IO_NOAUTH 0
#define IO_UNIT 0
#define MNT_WAIT 0
#define VATTR_INIT(v) memset((v), 0, sizeof(*(v)))
#define VATTR_WANTED(v, member) ((void)(v))
#define VATTR_SET(v, member, value) ((v)->member = (value))
struct vnode_attr {uint64_t va_data_size; uint32_t va_mode, va_uid, va_gid;};
struct Stats {const char *f_fstypename;};
static struct Stats root_stats = {"apfs"};
static unsigned writes, upgrades;
static int upgrade_error;
static const char *fail_open;
static int native_owner_data_status, native_owner_data_readonly, native_owner_cache_status;
static int native_owner_cache_authorized = 1, native_owner_cache_root_recheck = 1;
static uintptr_t native_owner_cache_current_root;
static unsigned gpu_written = 73;
static const uint8_t gpu_identity[16] = {0x73,0x19,0x33};
static uint8_t candidate[64], original[64], archive[512];
static size_t archive_bytes;
static const char *library = "/diting/libditing-ui18.dylib";
static const char *stage = "/diting/libditing-ui18.dylib.ui-brightness-stage";
static const char *identity = "/diting/.diting-ui-brightness-20261115";
static char backup[256];

static struct Node *find(const char *path)
{for (unsigned i=0;i<24;i++) if(nodes[i].active && !strcmp(path,nodes[i].path)) return nodes+i;return NULL;}
static struct Node *add(const char *path, enum vtype type, const uint8_t *data, size_t bytes, uint32_t mode)
{
    struct Node *node = find(path);
    if (!node) for(unsigned i=0;i<24;i++) if(!nodes[i].active){node=nodes+i;break;}
    assert(node && strlen(path)<sizeof(node->path));
    free(node->data);memset(node,0,sizeof(*node));strcpy(node->path,path);
    node->active=true;node->type=type;node->mode=mode;node->bytes=bytes;
    if(bytes){node->data=malloc(bytes);assert(node->data);memcpy(node->data,data,bytes);}
    return node;
}
static size_t fixture_strlcpy(char *out, const char *in, size_t size)
{size_t n=strlen(in);if(size){size_t count=n<size-1?n:size-1;memcpy(out,in,count);out[count]=0;}return n;}
#define strlcpy fixture_strlcpy
static void *kalloc_data(size_t bytes, int flags)
{return flags&Z_ZERO?calloc(1,bytes):malloc(bytes);}
static void kfree_data(void *data, size_t bytes) {(void)bytes;free(data);}
static vfs_context_t vfs_context_current(void) {return NULL;}
static void *vfs_context_ucred(vfs_context_t c) {(void)c;return NULL;}
static void *vfs_context_proc(vfs_context_t c) {(void)c;return NULL;}
static void *vnode_mount(vnode_t node) {(void)node;return &root_stats;}
static struct Stats *vfs_statfs(void *mount) {return mount;}
static void PE_parse_boot_argn(const char *key, int *value, size_t bytes)
{assert(!strcmp(key,"diting_rootful") && bytes==sizeof(*value));*value=1;}
static int vnode_getattr(vnode_t node, struct vnode_attr *va, vfs_context_t ctx)
{(void)ctx;va->va_data_size=node->bytes;va->va_mode=node->mode;va->va_uid=node->uid;va->va_gid=node->gid;return 0;}
static int vnode_setattr(vnode_t node, struct vnode_attr *va, vfs_context_t ctx)
{(void)ctx;node->mode=va->va_mode;node->uid=va->va_uid;node->gid=va->va_gid;return 0;}
static int vnode_open(const char *path, int flags, uint32_t mode, int unused, vnode_t *answer, vfs_context_t ctx)
{
    (void)unused;(void)ctx;
    if(fail_open && !strcmp(path,fail_open)) return ENOSPC;
    struct Node *node=find(path);
    if(flags&O_CREAT){if(node && flags&O_EXCL)return EEXIST;if(!node){node=add(path,VREG,NULL,0,mode);++writes;}}
    if(!node)return ENOENT;*answer=node;return 0;
}
static int vnode_close(vnode_t node, int flags, vfs_context_t ctx)
{(void)node;(void)flags;(void)ctx;return 0;}
static int VNOP_FSYNC(vnode_t node, int flags, vfs_context_t ctx)
{(void)node;(void)flags;(void)ctx;return 0;}
static int vn_rdwr(int operation, vnode_t node, char *data, int amount, off_t at,
    int segment, int flags, void *credential, int *resid, void *process)
{
    (void)segment;(void)flags;(void)credential;(void)process;assert(at>=0 && amount>=0);*resid=0;
    if(operation==UIO_READ){if((uint64_t)at+(unsigned)amount>node->bytes)return EIO;memcpy(data,node->data+at,(size_t)amount);}
    else{size_t stop=(size_t)at+(unsigned)amount;if(stop>node->bytes){node->data=realloc(node->data,stop);assert(node->data);node->bytes=stop;}memcpy(node->data+at,data,(size_t)amount);++writes;}
    return 0;
}
static int dgr_stat(const char *path, enum vtype type, uint64_t bytes, uint32_t mode)
{
    struct Node *node=find(path);if(!node)return ENOENT;
    if(node->type!=type || node->uid || node->gid || (mode && (node->bytes!=bytes || node->mode!=mode)))return EINVAL;
    return 0;
}
static int dgr_id_equal(const char *path, const uint8_t *id)
{return !dgr_stat(path,VREG,16,0644) && !memcmp(find(path)->data,id,16);}
static int dgr_inflate(const struct DitingGpuRuntimeEntry *entry, uint8_t *data)
{
    uLongf output=(uLongf)entry->bytes;uLong input=(uLong)entry->packed_bytes;
    int result=uncompress2(data,&output,entry->packed,&input);
    return result==Z_OK && output==entry->bytes && input==entry->packed_bytes?0:EINVAL;
}
static int dgr_unlink(proc_t p, user_addr_t scratch, const char *path)
{(void)p;(void)scratch;struct Node *node=find(path);if(!node)return ENOENT;node->active=false;free(node->data);node->data=NULL;++writes;return 0;}
static int dgr_rename(proc_t p, user_addr_t scratch, const char *from, const char *to)
{
    (void)p;(void)scratch;struct Node *node=find(from);if(!node)return ENOENT;
    struct Node *old=find(to);if(old){old->active=false;free(old->data);old->data=NULL;}
    assert(strlen(to)<sizeof(node->path));strcpy(node->path,to);++writes;return 0;
}
static int dgr_upgrade(proc_t p, user_addr_t scratch)
{(void)p;(void)scratch;++upgrades;return upgrade_error;}

#include "DitingUiRuntimeInstall.h"
const uint8_t DitingUiRuntimeData[1]={0}, DitingUiRuntimeEnd[1]={0};

static void pack(uint8_t id)
{
    memset(archive,0,sizeof(archive));memcpy(archive,"DTUI0001",8);
    uint32_t abi=1,board=0x8475,count=1;memcpy(archive+8,&abi,4);memcpy(archive+12,&board,4);memcpy(archive+16,&count,4);
    memset(archive+32,id,16);uLongf packed=sizeof(archive)-64;
    assert(compress2(archive+64,&packed,candidate,sizeof(candidate),9)==Z_OK);
    archive_bytes=64+((packed+7)&~(size_t)7);uint64_t total=archive_bytes,raw=sizeof(candidate),bytes=packed;
    memcpy(archive+24,&total,8);memcpy(archive+48,&raw,8);memcpy(archive+56,&bytes,8);
    static const char digits[]="0123456789abcdef";
    strcpy(backup,"/diting/libditing-ui18.dylib.before-ui-");size_t at=strlen(backup);
    for(unsigned i=0;i<16;i++){backup[at++]=digits[id>>4];backup[at++]=digits[id&15];}backup[at]=0;
}
static void reset(uint32_t mode)
{
    for(unsigned i=0;i<24;i++){free(nodes[i].data);memset(nodes+i,0,sizeof(nodes[i]));}
    native_owner_data_status=native_owner_data_readonly=native_owner_cache_status=0;
    native_owner_cache_authorized=native_owner_cache_root_recheck=1;native_owner_cache_current_root=(uintptr_t)rootvnode;
    writes=upgrades=0;upgrade_error=0;fail_open=NULL;diting_ui_runtime_stage=diting_ui_runtime_error=0;diting_ui_runtime_written=0;
    memset(candidate,0,sizeof(candidate));uint32_t magic=0xfeedfacf,cpu=0x0100000c,type=6;
    memcpy(candidate,&magic,4);memcpy(candidate+4,&cpu,4);memcpy(candidate+12,&type,4);candidate[32]=0xb1;
    memcpy(original,candidate,sizeof(original));original[32]=0xa1;pack(0x11);
    add("/diting",VDIR,NULL,0,0755);add(library,VREG,original,sizeof(original),mode);
    add("/System/Library/Extensions/DitingAdrenoMetal.bundle/.diting-native-gpu-runtime-v1",VREG,gpu_identity,16,0644);
}
static void gpu_unchanged(void)
{assert(gpu_written==73);assert(!memcmp(find("/System/Library/Extensions/DitingAdrenoMetal.bundle/.diting-native-gpu-runtime-v1")->data,gpu_identity,16));}
static void equal_file(const char *path, const uint8_t *data, size_t bytes)
{assert(find(path) && find(path)->bytes==bytes && !memcmp(find(path)->data,data,bytes));}
static int install(void) {return dui_install_archive(NULL,0,archive,archive_bytes);}
static void parser_bounds(void)
{
    reset(0755);struct DitingUiRuntimeEntry entry;assert(diting_ui_runtime_parse(archive,archive_bytes,&entry));
    for(size_t n=0;n<archive_bytes;n++)assert(!diting_ui_runtime_parse(archive,n,&entry));
    for(unsigned offset=0;offset<24;offset++){archive[offset]^=1;assert(!diting_ui_runtime_parse(archive,archive_bytes,&entry));archive[offset]^=1;}
    memset(archive+32,0,16);assert(!diting_ui_runtime_parse(archive,archive_bytes,&entry));pack(0x11);
    uint64_t packed=diting_ui_runtime_u64(archive+56);if((packed&7)!=0){archive[64+packed]=1;assert(!diting_ui_runtime_parse(archive,archive_bytes,&entry));}
    pack(0x11);uint64_t huge=UINT64_MAX;memcpy(archive+48,&huge,8);assert(!diting_ui_runtime_parse(archive,archive_bytes,&entry));
    assert(install()==EINVAL && !writes && !upgrades);gpu_unchanged();
}
static void update_and_repeat(uint32_t mode)
{
    reset(mode);assert(!install() && diting_ui_runtime_stage==6);equal_file(library,candidate,64);equal_file(backup,original,64);
    assert(find(backup)->mode==mode && find(library)->mode==0755 && dgr_id_equal(identity,archive+32));
    unsigned before=writes;assert(!install() && writes==before);gpu_unchanged();
}
static void foreign_backup(void)
{
    reset(0755);uint8_t foreign[64]={0xf1};add(backup,VREG,foreign,64,0755);
    assert(install()==EEXIST);equal_file(library,original,64);equal_file(backup,foreign,64);assert(!find(identity));gpu_unchanged();
}
static void later_ui_is_preserved(void)
{
    reset(0755);assert(!install());find(library)->data[33]=0x99;unsigned before=writes;
    assert(install()==EEXIST && writes==before && find(library)->data[33]==0x99);gpu_unchanged();
}
static void candidate_upgrade(void)
{
    reset(0755);assert(!install());char first_backup[256];strcpy(first_backup,backup);uint8_t prior[64];memcpy(prior,candidate,64);
    candidate[33]=0xb2;pack(0x22);assert(!install());equal_file(backup,prior,64);equal_file(first_backup,original,64);equal_file(library,candidate,64);gpu_unchanged();
}
static void interrupted_stage(void)
{
    reset(0755);uint8_t partial[32]={0};add(stage,VREG,partial,32,0755);assert(!install());equal_file(library,candidate,64);equal_file(backup,original,64);gpu_unchanged();
}
static void foreign_stage(void)
{
    reset(0755);uint8_t foreign[32]={0xf0};add(stage,VREG,foreign,32,0755)->uid=501;
    assert(install()==EINVAL);equal_file(library,original,64);equal_file(stage,foreign,32);gpu_unchanged();
}
static void prerequisites_and_failures(void)
{
    reset(0755);native_owner_cache_current_root=0;assert(install()==EAUTH && !writes && !upgrades);gpu_unchanged();
    reset(0755);upgrade_error=EROFS;assert(install()==EROFS && !writes);equal_file(library,original,64);gpu_unchanged();
    reset(0755);fail_open=stage;assert(install()==ENOSPC);equal_file(library,original,64);assert(!find(backup) && !find(identity));gpu_unchanged();
    reset(0755);archive[64]^=0xff;assert(install()==EINVAL && !writes && !upgrades);gpu_unchanged();
    reset(0755);candidate[0]=0;pack(0x11);assert(install()==EINVAL && !writes && !upgrades);gpu_unchanged();
    reset(0755);diting_ui_runtime_prepare(NULL,0);
    assert(diting_ui_runtime_error==EINVAL && !writes && !upgrades);
    equal_file(library,original,64);gpu_unchanged();
}
int main(void)
{
    parser_bounds();puts("PASS parser bounds and padding");
    update_and_repeat(0755);puts("PASS atomic update and repeat");
    update_and_repeat(0644);puts("PASS original permissions preserved");
    foreign_backup();puts("PASS foreign backup preserved");
    later_ui_is_preserved();puts("PASS later UI update preserved");
    candidate_upgrade();puts("PASS separate backups for candidate upgrades");
    interrupted_stage();puts("PASS interrupted staging repaired");
    foreign_stage();puts("PASS foreign staging preserved");
    prerequisites_and_failures();puts("PASS mount/identity/stream/write failures");
    for(unsigned i=0;i<24;i++)free(nodes[i].data);
    return 0;
}

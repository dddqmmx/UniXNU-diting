/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
extern struct vnode *rootvnode; /* bsd/sys/vnode_internal.h */
#if defined(DITING_GUEST_24B156)
extern int diting_ufs_read_fw_blocks(uint32_t blockOffset, void *buf, uint32_t bytes) __attribute__((weak_import));
static char ufs_adsp_handle;
static uint32_t ufs_adsp_off, ufs_adsp_size;
static int adsp_ufs_open(uint64_t *size) {
 if(!&diting_ufs_read_fw_blocks)return ENOENT;
 unsigned char *hdr=IOMallocData(16384);
 if(!hdr)return ENOMEM;
 int rc=diting_ufs_read_fw_blocks(0,hdr,16384);
 if(rc){IOFreeData(hdr,16384);return EIO;}
 unsigned count=0;
 if(memcmp(hdr,"PWF2",4)){IOFreeData(hdr,16384);return ENOENT;}
 memcpy(&count,hdr+4,4);
 if(count>(16384u-8u)/40u){IOFreeData(hdr,16384);return EFTYPE;}
 uint32_t found_off=0,found_len=0;
 for(unsigned i=0;i<count;i++) {
  const unsigned char *e=hdr+8+i*40;
  if(e[31]==0 && !strcmp((const char *)e,"adsp.mbn")) {
   memcpy(&found_off,e+32,4);memcpy(&found_len,e+36,4);break;
  }
 }
 IOFreeData(hdr,16384);
 if(!found_len || (found_off&4095u) || found_len>64u*1024u*1024u)return ENOENT;
 ufs_adsp_off=found_off;ufs_adsp_size=found_len;
 *size=found_len;
 IOLog("DitingAdsp: opened signed adsp.mbn from UFS PWF2 pack off=0x%x bytes=%u\n",found_off,found_len);
 return 0;
}
static int adsp_ufs_read(uint64_t offset,void *to,size_t length) {
 if(offset>ufs_adsp_size||length>ufs_adsp_size-offset)return EINVAL;
 unsigned char *page=IOMallocData(4096);
 if(!page)return ENOMEM;
 unsigned char *dst=(unsigned char *)to;
 uint64_t cur=offset;size_t rem=length;
 int rc=0;
 while(rem) {
  uint64_t abs_pos=(uint64_t)ufs_adsp_off+cur;
  uint32_t blk=(uint32_t)(abs_pos>>12);
  uint32_t in_blk=(uint32_t)(abs_pos&4095u);
  if(in_blk==0 && rem>=4096u) {
   uint32_t chunk=(uint32_t)(rem&~4095u);
   if(chunk>256u*1024u)chunk=256u*1024u;
   rc=diting_ufs_read_fw_blocks(blk,dst,chunk);
   if(rc)break;
   dst+=chunk;cur+=chunk;rem-=chunk;
  } else {
   rc=diting_ufs_read_fw_blocks(blk,page,4096u);
   if(rc)break;
   size_t take=4096u-in_blk;if(take>rem)take=rem;
   memcpy(dst,page+in_blk,take);
   dst+=take;cur+=take;rem-=take;
  }
 }
 IOFreeData(page,4096);
 return rc?EIO:0;
}
#endif
#if DITING_ADSP_BUILTIN
#include <libkern/zlib.h>
#include "DitingAdspArchive.h"
extern const unsigned char diting_adsp_firmware_begin[], diting_adsp_firmware_end[];
static char builtin_handle;
static unsigned builtin_opened;
static unsigned char *builtin_data;
static size_t builtin_bytes;
static voidpf adsp_zalloc(voidpf opaque, uInt items, uInt size) {
 (void)opaque;
 if(size&&items>(1u<<20)/size)return Z_NULL;
 size_t bytes=(size_t)items*size+16;
 unsigned char *p=IOMallocData(bytes);if(!p)return Z_NULL;
 memset(p,0,bytes);memcpy(p,&bytes,sizeof(bytes));return p+16;
}
static void adsp_zfree(voidpf opaque,voidpf address) {
 (void)opaque;if(!address)return;
 unsigned char *p=(unsigned char *)address-16;size_t bytes=0;
 memcpy(&bytes,p,sizeof(bytes));IOFreeData(p,bytes);
}
static int adsp_builtin_open(void) {
 uint64_t begin=(uintptr_t)diting_adsp_firmware_begin,end=(uintptr_t)diting_adsp_firmware_end;
 struct DitingAdspArchive entry;
 if(end<=begin||end-begin>DitingAdspArchiveLimit+DitingAdspArchiveHeader||
    !diting_adsp_archive_parse(diting_adsp_firmware_begin,(size_t)(end-begin),&entry))return EFTYPE;
 unsigned expected=0;
 if(!__atomic_compare_exchange_n(&builtin_opened,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return EBUSY;
 unsigned char *data=IOMallocData((size_t)entry.bytes);
 int error=!data?ENOMEM:diting_adsp_archive_inflate(&entry,data,adsp_zalloc,adsp_zfree,NULL)?0:EFTYPE;
 if(error) {
  if(data)IOFreeData(data,(size_t)entry.bytes);
  __atomic_store_n(&builtin_opened,0,__ATOMIC_RELEASE);return error;
 }
 builtin_data=data;builtin_bytes=(size_t)entry.bytes;return 0;
}
#endif
int sheng_fw_open(const char *path,void **handle,uint64_t *size) {
 if(!path||!handle||!size)return EINVAL;
 *handle=NULL;*size=0;
 char directory[80]={0},selected[112]="/diting/firmware/adsp.mbn";
 if(PE_parse_boot_argn("diting_adsp-fw",directory,sizeof(directory))&&directory[0]) {
  if(directory[0]!='/')return EINVAL;
  int n=snprintf(selected,sizeof(selected),"%s/adsp.mbn",directory);
  if(n<0||n>=(int)sizeof(selected))return ENAMETOOLONG;
 }
 if(strcmp(path,selected))return EINVAL;
 // Preserve the existing post-root firmware ownership boundary.
 if(!rootvnode)return ENXIO;
 vnode_t vnode=NULLVP;vfs_context_t ctx=vfs_context_kernel();
 int rc=vnode_open(path,FREAD|O_NOFOLLOW,0,0,&vnode,ctx);
#if defined(DITING_GUEST_24B156)
 if(rc==ENOENT) {
  int uerr=adsp_ufs_open(size);
  if(!uerr){*handle=&ufs_adsp_handle;return 0;}
 }
#endif
#if DITING_ADSP_BUILTIN
 unsigned enabled=0;
 if(rc==ENOENT&&PE_parse_boot_argn("diting_adsp_builtin",&enabled,sizeof(enabled))&&enabled==1) {
  int error=adsp_builtin_open();if(error)return error;
  *handle=&builtin_handle;*size=builtin_bytes;
  IOLog("DitingAdsp: explicit signed firmware decompressed, bytes=%llu; PAS authentication required\n",*size);
  return 0;
 }
#endif
 if(rc)return rc;
 struct vnode_attr va;VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);VATTR_WANTED(&va,va_uid);VATTR_WANTED(&va,va_mode);
 rc=vnode_getattr(vnode,&va,ctx);
 if(rc||!VATTR_IS_SUPPORTED(&va,va_data_size)||!VATTR_IS_SUPPORTED(&va,va_uid)||!VATTR_IS_SUPPORTED(&va,va_mode)||vnode_vtype(vnode)!=VREG||va.va_uid||va.va_mode&0022||!va.va_data_size||va.va_data_size>64*1024*1024){vnode_close(vnode,FREAD,ctx);return rc?rc:EFTYPE;}
 *handle=vnode;*size=va.va_data_size;return 0;
}
int sheng_fw_read(void *handle,uint64_t offset,void *to,size_t length) {
 if(!handle||!to||length>1024*1024||offset>64*1024*1024||length>64*1024*1024-offset)return EINVAL;
#if defined(DITING_GUEST_24B156)
 if(handle==&ufs_adsp_handle)return adsp_ufs_read(offset,to,length);
#endif
#if DITING_ADSP_BUILTIN
 if(handle==&builtin_handle) {
  if(!builtin_data||offset>builtin_bytes||length>builtin_bytes-offset)return EINVAL;
  memcpy(to,builtin_data+offset,length);return 0;
 }
#endif
 vfs_context_t ctx=vfs_context_kernel();int resid=(int)length;
 int rc=vn_rdwr(UIO_READ,(vnode_t)handle,(caddr_t)to,(int)length,(off_t)offset,UIO_SYSSPACE,IO_NOAUTH,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
 return rc?rc:resid?EIO:0;
}
void sheng_fw_close(void *handle) {
#if defined(DITING_GUEST_24B156)
 if(handle==&ufs_adsp_handle){ufs_adsp_off=0;ufs_adsp_size=0;return;}
#endif
#if DITING_ADSP_BUILTIN
 if(handle==&builtin_handle) {
  if(builtin_data)IOFreeData(builtin_data,builtin_bytes);
  builtin_data=NULL;builtin_bytes=0;__atomic_store_n(&builtin_opened,0,__ATOMIC_RELEASE);return;
 }
#endif
 if(handle)vnode_close((vnode_t)handle,FREAD,vfs_context_kernel());
}

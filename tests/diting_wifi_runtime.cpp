/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * The production parser, inflater and installation transaction run unchanged.
 * A fault-injectable VFS models ownership, exclusive renames and crash points.
 */
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <stdint.h>

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); std::exit(1); } } while (0)
#define DITING_WIFI_RUNTIME_HOST_TEST 1
#define __unused __attribute__((unused))
#define Z_WAITOK 1
#define Z_ZERO 2
#ifndef EAUTH
#define EAUTH 80
#endif
#define RENAME_EXCL 4u
using proc_t = uintptr_t;
using user_addr_t = uintptr_t;
enum vtype { VREG, VDIR, VLNK };
static uint8_t *kalloc_data(size_t bytes, unsigned flags)
{ return (uint8_t *)(flags & Z_ZERO ? std::calloc(1,bytes) : std::malloc(bytes)); }
static void kfree_data(void *address, size_t) { std::free(address); }
static int dwr_stat(const char *, enum vtype, uint64_t, uint32_t);
static int dwr_equal(const char *, const uint8_t *, uint64_t, uint32_t);
static int dwr_id_equal(const char *, const uint8_t *);
static int dwr_write_new(proc_t, user_addr_t, const char *, const uint8_t *, uint64_t, uint32_t, int);
static int dwr_rename_flags(proc_t, user_addr_t, const char *, const char *, unsigned);
static int dwr_rename(proc_t, user_addr_t, const char *, const char *);
static int dwr_upgrade(proc_t, user_addr_t);
static int dwr_backup_job(proc_t, user_addr_t, const char *, const char *, const char *);
#include "../kits/DitingPlatform24/DitingWifiRuntimeInstall.h"

struct File {
    std::vector<uint8_t> data;
    uint32_t mode=0644, uid=0, gid=0;
    enum vtype type=VREG;
    bool root_mount=true;
    bool operator==(const File &other) const {
        return data==other.data && mode==other.mode && uid==other.uid && gid==other.gid &&
            type==other.type && root_mount==other.root_mount;
    }
};
static std::map<std::string,File> files;
static std::vector<uint8_t> archive, raw_storage[5], old_job;
static const uint8_t *raw[5], *identifier;
static struct DitingWifiRuntimeEntry entries[5];
static unsigned mutations, upgrade_calls, fail_at;
static bool fail_after, root_writable, root_binding=true, race_new_file, race_cache;
static const char *const marker="/diting/.diting-wifi-native-services-v1";
static const char *const protected_paths[]={
    "/System/Library/Extensions/DitingAdrenoMetal.bundle/DitingAdrenoMetal",
    "/diting/diting-gpu-auto", "/diting/libditing-ui18.dylib",
    "/private/var/mobile/Library/Preferences/com.apple.wifi.plist"};

static File file(const uint8_t *data, size_t bytes, uint32_t mode=0644)
{ File result; result.data.assign(data,data+bytes);result.mode=mode;return result; }
static File text_file(const char *text, uint32_t mode=0644)
{ return file((const uint8_t *)text,std::strlen(text),mode); }
static bool before_mutation() {++mutations;return fail_at==mutations && !fail_after;}
static bool after_mutation() {return fail_at==mutations && fail_after;}
static bool matches(unsigned i)
{
    auto it=files.find(dwr_files[i]);
    return it!=files.end() && it->second==file(raw[i],entries[i].bytes,dwr_modes[i]);
}
static int dwr_stat(const char *path, enum vtype type, uint64_t bytes, uint32_t mode)
{
    auto it=files.find(path);
    if(it==files.end())return ENOENT;
    const File &f=it->second;
    if(f.type!=type || !f.root_mount || f.uid || f.gid ||
        (mode && (f.data.size()!=bytes || f.mode!=mode)))return EINVAL;
    return 0;
}
static int dwr_equal(const char *path, const uint8_t *data, uint64_t bytes, uint32_t mode)
{
    int error=dwr_stat(path,VREG,bytes,mode);
    if(error)return error;
    return files.at(path).data==std::vector<uint8_t>(data,data+bytes) ? 0 : EEXIST;
}
static int dwr_id_equal(const char *path, const uint8_t *id)
{ return !dwr_equal(path,id,16,0644); }
static int dwr_upgrade(proc_t, user_addr_t)
{
    ++upgrade_calls;
    if(!root_binding)return EAUTH;
    if(root_writable)return 0;
    if(before_mutation())return EIO;
    root_writable=true;
    return after_mutation() ? EIO : 0;
}
static int dwr_write_new(proc_t, user_addr_t, const char *path,
    const uint8_t *data, uint64_t bytes, uint32_t mode, int repair)
{
    CHECK(root_writable);
    auto it=files.find(path);
    if(it!=files.end()) {
        int error=dwr_stat(path,VREG,0,0);
        if(error)return error;
        if(it->second==file(data,bytes,mode))return 0;
        if(!repair)return EEXIST;
    }
    if(before_mutation())return EIO;
    if(after_mutation()) {
        // A power interruption can leave a short private staging inode.
        files[path]=file(data,std::min<uint64_t>(bytes,4),mode);
        return EIO;
    }
    files[path]=file(data,bytes,mode);
    ++diting_wifi_runtime_written;
    return 0;
}
static int dwr_rename_flags(proc_t, user_addr_t, const char *from, const char *to, unsigned flags)
{
    CHECK(root_writable);
    if(race_new_file && !std::strcmp(to,dwr_files[0])) {
        files[to]=text_file("foreign concurrent executable",0755);race_new_file=false;
    }
    auto source=files.find(from);
    if(source==files.end())return ENOENT;
    if((flags&RENAME_EXCL) && files.count(to))return EEXIST;
    if(!std::strcmp(to,dwr_files[3]))for(unsigned i=0;i<3;++i)CHECK(matches(i));
    if(!std::strcmp(to,marker))for(unsigned i=0;i<4;++i)CHECK(matches(i));
    if(before_mutation())return EIO;
    files[to]=source->second;files.erase(source);
    if(race_cache && !std::strcmp(to,dwr_files[2])) {
        files[dwr_files[3]].data.push_back(0xaa);race_cache=false;
    }
    // Model a failed parent-directory fsync after an atomic rename.
    return after_mutation() ? EIO : 0;
}
static int dwr_rename(proc_t p, user_addr_t scratch, const char *from, const char *to)
{ return dwr_rename_flags(p,scratch,from,to,0); }
static int dwr_backup_job(proc_t p, user_addr_t scratch, const char *source,
    const char *backup, const char *temporary)
{
    int error=dwr_stat(backup,VREG,0,0);
    if(!error) {
        const File &f=files.at(backup);
        return f.data.size()<8 || f.data.size()>65536 || f.mode!=0644 ? EINVAL : 0;
    }
    if(error!=ENOENT)return error;
    error=dwr_stat(source,VREG,0,0);
    if(error==ENOENT)return 0;
    if(error)return error;
    const File saved=files.at(source);
    if(saved.data.size()<8 || saved.data.size()>65536 || saved.mode!=0644)return EINVAL;
    error=dwr_stage(p,scratch,temporary,saved.data.data(),saved.data.size(),0644);
    return error ? error : dwr_rename_flags(p,scratch,temporary,backup,RENAME_EXCL);
}

static void seed()
{
    files.clear();
    old_job=text_file("bplist00original native wifid job").data;
    files[dwr_files[2]]=file(old_job.data(),old_job.size());
    files[dwr_files[3]]=file(raw[4],entries[4].bytes);
    for(const char *path:protected_paths)files[path]=text_file("other task's installed bytes");
    files[protected_paths[3]].uid=501;files[protected_paths[3]].root_mount=false;
    mutations=upgrade_calls=fail_at=0;fail_after=root_writable=race_new_file=race_cache=false;
    root_binding=true;
    diting_wifi_runtime_stage=diting_wifi_runtime_error=0;
    diting_wifi_runtime_written=diting_wifi_runtime_step=0;
}
static void protected_intact()
{
    for(const char *path:protected_paths) {
        File expected=text_file("other task's installed bytes");
        if(path==protected_paths[3]){expected.uid=501;expected.root_mount=false;}
        CHECK(files.at(path)==expected);
    }
}
static void installed()
{
    CHECK(diting_wifi_runtime_stage==6);
    for(unsigned i=0;i<4;++i)CHECK(matches(i));
    CHECK(dwr_id_equal(marker,identifier));
    char path[256];dwr_path(dwr_files[3],identifier,".before-wifi-",path);
    CHECK(files.at(path)==file(raw[4],entries[4].bytes));
    dwr_path(dwr_files[2],identifier,".before-wifi-",path);
    CHECK(files.at(path)==file(old_job.data(),old_job.size()));
    protected_intact();
}
static int install() {return dwr_install_files(0,0,raw,entries,identifier);}
static void store64(std::vector<uint8_t> &data, size_t at, uint64_t value)
{ for(unsigned i=0;i<8;++i)data[at+i]=(uint8_t)(value>>(8*i)); }
static void parser_tests()
{
    struct DitingWifiRuntimeEntry parsed[5];
    CHECK(!diting_wifi_runtime_parse(nullptr,archive.size(),parsed));
    CHECK(!diting_wifi_runtime_parse(archive.data(),archive.size(),nullptr));
    for(size_t cut:{size_t(0),size_t(7),size_t(47),size_t(167),archive.size()-1})
        CHECK(!diting_wifi_runtime_parse(archive.data(),cut,parsed));
    for(size_t at:{size_t(0),size_t(8),size_t(12),size_t(16),size_t(20),size_t(24)}) {
        auto bad=archive;bad[at]^=1;
        CHECK(!diting_wifi_runtime_parse(bad.data(),bad.size(),parsed));
    }
    auto bad=archive;std::fill(bad.begin()+32,bad.begin()+48,0);
    CHECK(!diting_wifi_runtime_parse(bad.data(),bad.size(),parsed));
    bad=archive;bad.push_back(0);store64(bad,24,bad.size());
    CHECK(!diting_wifi_runtime_parse(bad.data(),bad.size(),parsed));
    size_t cursor=48;
    const uint64_t limits[5]={1u<<20,65536,65536,2u<<20,2u<<20};
    for(unsigned i=0;i<5;++i) {
        for(uint64_t length:{UINT64_C(0),UINT64_C(7),limits[i]+1,UINT64_MAX}) {
            bad=archive;store64(bad,cursor,length);
            CHECK(!diting_wifi_runtime_parse(bad.data(),bad.size(),parsed));
        }
        uint64_t packed=diting_wifi_runtime_u64(archive.data()+cursor+8);
        for(uint64_t length:{UINT64_C(0),UINT64_C(4194305),UINT64_MAX}) {
            bad=archive;store64(bad,cursor+8,length);
            CHECK(!diting_wifi_runtime_parse(bad.data(),bad.size(),parsed));
        }
        size_t padded=(packed+7)&~UINT64_C(7);
        if(padded>packed) {
            bad=archive;bad[cursor+16+packed]=1;
            CHECK(!diting_wifi_runtime_parse(bad.data(),bad.size(),parsed));
        }
        CHECK(!diting_wifi_runtime_parse(archive.data(),cursor+16+padded-1,parsed));
        cursor+=16+padded;
    }
    CHECK(cursor==archive.size());
    CHECK(dwr_zalloc(nullptr,UINT32_MAX,UINT32_MAX)==Z_NULL);
    void *small=dwr_zalloc(nullptr,1,128);CHECK(small!=Z_NULL);dwr_zfree(nullptr,small);
    for(unsigned i=0;i<5;++i) {
        std::vector<uint8_t> guarded(entries[i].bytes+128,0xda);
        CHECK(!dwr_inflate(entries+i,guarded.data()+64));
        CHECK(std::equal(raw_storage[i].begin(),raw_storage[i].end(),guarded.begin()+64));
        CHECK(std::all_of(guarded.begin(),guarded.begin()+64,[](uint8_t v){return v==0xda;}));
        CHECK(std::all_of(guarded.end()-64,guarded.end(),[](uint8_t v){return v==0xda;}));
        auto compressed=std::vector<uint8_t>(entries[i].packed,entries[i].packed+entries[i].packed_bytes);
        compressed.back()^=1;
        auto changed=entries[i];changed.packed=compressed.data();
        CHECK(dwr_inflate(&changed,guarded.data()+64)==EINVAL);
        changed=entries[i];--changed.packed_bytes;
        CHECK(dwr_inflate(&changed,guarded.data()+64)==EINVAL);
        changed=entries[i];--changed.bytes;
        CHECK(dwr_inflate(&changed,guarded.data()+64)==EINVAL);
        compressed.assign(entries[i].packed,entries[i].packed+entries[i].packed_bytes);
        compressed.push_back(0);changed=entries[i];changed.packed=compressed.data();++changed.packed_bytes;
        CHECK(dwr_inflate(&changed,guarded.data()+64)==EINVAL);
    }
}
static void transaction_tests()
{
    seed();CHECK(!install());installed();unsigned completed_mutations=mutations;
    auto complete=files;mutations=upgrade_calls=0;
    CHECK(!install());CHECK(!mutations);CHECK(!upgrade_calls);CHECK(files==complete);

    seed();files[dwr_files[3]].data.push_back(1);auto before=files;
    CHECK(install()==ESTALE);CHECK(!mutations);CHECK(!upgrade_calls);CHECK(files==before);
    seed();root_binding=false;before=files;
    CHECK(install()==EAUTH);CHECK(!mutations);CHECK(files==before);
    for(unsigned i=0;i<2;++i) {
        seed();files[dwr_files[i]]=text_file("foreign new destination",dwr_modes[i]);before=files;
        CHECK(install()==EEXIST);CHECK(!mutations);CHECK(files==before);
        for(unsigned attribute=0;attribute<3;++attribute) {
            seed();files[dwr_files[i]]=file(raw[i],entries[i].bytes,dwr_modes[i]);
            if(attribute==0)files[dwr_files[i]].type=VLNK;
            if(attribute==1)files[dwr_files[i]].uid=501;
            if(attribute==2)files[dwr_files[i]].root_mount=false;
            before=files;CHECK(install()==EINVAL);CHECK(!mutations);CHECK(files==before);
        }
    }
    // Every mutating operation may fail before or after its effect. The next
    // boot must converge and the preserved preimages must remain the originals.
    for(unsigned after=0;after<2;++after)for(unsigned fault=1;fault<=completed_mutations;++fault) {
        seed();fail_at=fault;fail_after=after;
        CHECK(install()==EIO);protected_intact();
        if(matches(3))for(unsigned i=0;i<3;++i)CHECK(matches(i));
        fail_at=0;CHECK(!install());installed();
    }
    seed();char path[256];dwr_path(dwr_files[0],identifier,".wifi-stage-",path);
    files[path]=text_file("short");CHECK(!install());installed();
    seed();dwr_path(dwr_files[3],identifier,".before-wifi-",path);
    files[path]=text_file("foreign backup");
    CHECK(install()==EEXIST);CHECK(files.at(path)==text_file("foreign backup"));
    CHECK(files.at(dwr_files[3])==file(raw[4],entries[4].bytes));protected_intact();
    seed();files[marker]=text_file("invalid marker");before=files;
    CHECK(install()==EEXIST);CHECK(!mutations);CHECK(files==before);
    seed();race_new_file=true;
    CHECK(install()==EEXIST);
    CHECK(files.at(dwr_files[0])==text_file("foreign concurrent executable",0755));
    CHECK(files.at(dwr_files[3])==file(raw[4],entries[4].bytes));protected_intact();
    seed();race_cache=true;
    CHECK(install()==ESTALE);CHECK(files.at(dwr_files[3]).data.back()==0xaa);
    CHECK(!files.count(marker));protected_intact();
    seed();CHECK(!install());files.erase(marker);mutations=upgrade_calls=0;
    auto committed=files;
    CHECK(!install());CHECK(diting_wifi_runtime_written>=1);installed();
    for(unsigned i=0;i<4;++i)CHECK(files.at(dwr_files[i])==committed.at(dwr_files[i]));
    seed();CHECK(!install());files.erase(dwr_files[0]);mutations=upgrade_calls=0;
    CHECK(install()==EEXIST);CHECK(!mutations);CHECK(!upgrade_calls);
}

int main(int argc, char **argv)
{
    CHECK(argc==2);
    FILE *input=std::fopen(argv[1],"rb");CHECK(input);
    CHECK(!std::fseek(input,0,SEEK_END));long size=std::ftell(input);CHECK(size>0);
    CHECK(!std::fseek(input,0,SEEK_SET));archive.resize(size);
    CHECK(std::fread(archive.data(),1,size,input)==(size_t)size);CHECK(!std::fclose(input));
    CHECK(diting_wifi_runtime_parse(archive.data(),archive.size(),entries));
    identifier=archive.data()+32;
    for(unsigned i=0;i<5;++i) {
        raw_storage[i].resize(entries[i].bytes);
        CHECK(!dwr_inflate(entries+i,raw_storage[i].data()));raw[i]=raw_storage[i].data();
    }
    parser_tests();transaction_tests();
    std::printf("PASS: %u Wi-Fi archive, inflate, isolation and crash-recovery checks\n",checks);
    return 0;
}

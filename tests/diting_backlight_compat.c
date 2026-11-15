/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Executes the real UI board guard with a read-only IOKit/CF fixture. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {unsigned type;long length;uint8_t bytes[32];} Object;
typedef Object *CFMutableDictionaryRef;
typedef const Object *CFDictionaryRef;
typedef const Object *CFTypeRef;
typedef const Object *CFDataRef;
typedef const char *CFStringRef;
typedef void *CFAllocatorRef;
typedef long CFIndex;
#define CFSTR(s) (s)
#define RTLD_DEFAULT ((void *)0)
static const int kCFTypeDictionaryKeyCallBacks=0,kCFTypeDictionaryValueCallBacks=0;
static Object dictionary={0},properties={0},data={1,32,{0}};
static bool service_ready,chosen_present=true,data_present=true;
static bool no_release,no_entry,no_property,no_matching;
static unsigned reads,releases;

static CFMutableDictionaryRef fixture_matching(const char *name) {
    assert(!strcmp(name,"IOService"));return &dictionary;
}
static uint32_t fixture_get(uint32_t port,CFDictionaryRef dict) {
    assert(!port && dict==&dictionary);++reads;return service_ready?1:0;
}
static int fixture_release(uint32_t handle) {
    assert(handle==1 || handle==2);++releases;return 0;
}
static uint32_t fixture_entry(uint32_t port,const char *path) {
    assert(!port && !strcmp(path,"IODeviceTree:/chosen"));++reads;return chosen_present?2:0;
}
static CFTypeRef fixture_property(uint32_t handle,CFStringRef key,CFAllocatorRef allocator,uint32_t options) {
    assert(handle==2 && !strcmp(key,"diting-dsi-backlight") && !allocator && !options);
    ++reads;return data_present?&data:NULL;
}
static void *dlsym(void *handle,const char *symbol) {
    assert(handle==RTLD_DEFAULT);
    if(!strcmp(symbol,"IOServiceMatching"))return no_matching?NULL:(void *)fixture_matching;
    if(!strcmp(symbol,"IOServiceGetMatchingService"))return (void *)fixture_get;
    if(!strcmp(symbol,"IOObjectRelease"))return no_release?NULL:(void *)fixture_release;
    if(!strcmp(symbol,"IORegistryEntryFromPath"))return no_entry?NULL:(void *)fixture_entry;
    if(!strcmp(symbol,"IORegistryEntryCreateCFProperty"))return no_property?NULL:(void *)fixture_property;
    assert(false);return NULL; // no hardware writes, brightness setters or GPU API
}
static CFDictionaryRef CFDictionaryCreate(void *allocator,const void *const *keys,
    const void *const *values,CFIndex count,const int *key_callbacks,const int *value_callbacks) {
    assert(!allocator && count==1 && key_callbacks && value_callbacks);
    assert(!strcmp(keys[0],"DitingBacklightProtocol") && !strcmp(values[0],"sm8475-diting-dcs-v1"));
    return &properties;
}
static void CFDictionarySetValue(CFMutableDictionaryRef dict,CFStringRef key,CFDictionaryRef value) {
    assert(dict==&dictionary && !strcmp(key,"IOPropertyMatch") && value==&properties);
}
static void CFRelease(CFTypeRef object) { assert(object); }
static unsigned CFGetTypeID(CFTypeRef object) { return object->type; }
static unsigned CFDataGetTypeID(void) { return 1; }
static CFIndex CFDataGetLength(CFDataRef object) { return object->length; }
static const uint8_t *CFDataGetBytePtr(CFDataRef object) {
    assert(object->length==32);return object->bytes;
}

#include "brightness18_guard.inc"

int main(void) {
    static const uint8_t board[32]={
        0x00,0x40,0xe9,0x0a,0,0,0,0,0x00,0x04,0,0,0xff,0x0f,0,0,
        1,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0};
    memcpy(data.bytes,board,sizeof(board));
    // The asynchronous hardware service is absent while CoreBrightness
    // loads, but this exact board still needs its startup curve guard.
    assert(brightness18_diting_board());assert(reads==3 && releases==1);
    service_ready=true;reads=releases=0;
    assert(brightness18_diting_board());assert(reads==1 && releases==1);
    service_ready=false;
    for(unsigned offset=0;offset<32;offset++) {
        data.bytes[offset]^=1;assert(!brightness18_diting_board());data.bytes[offset]^=1;
    }
    data.type=2;assert(!brightness18_diting_board());data.type=1;
    data.length=31;assert(!brightness18_diting_board());data.length=32;
    chosen_present=false;assert(!brightness18_diting_board());chosen_present=true;
    data_present=false;assert(!brightness18_diting_board());data_present=true;
    no_release=true;assert(!brightness18_diting_board());no_release=false;
    no_entry=true;assert(!brightness18_diting_board());no_entry=false;
    no_property=true;assert(!brightness18_diting_board());no_property=false;
    no_matching=true;assert(brightness18_diting_board());
    puts("PASS compatibility");
}

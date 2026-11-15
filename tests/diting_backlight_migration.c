/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Exercise the real startup migration with persisted preference fixtures. */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {unsigned type;double number;} Object;
typedef const Object *CFTypeRef;
typedef const Object *CFNumberRef;
typedef const char *CFStringRef;
#define CFSTR(value) (value)
static const char *kCFPreferencesCurrentApplication="current-application";
static const int kCFNumberFloat64Type=1;
static const Object yes={1,1},stored_number={2,0},other={3,0};
static const CFTypeRef kCFBooleanTrue=&yes;
static Object created={2,0};
static CFTypeRef saved_level=&stored_number,saved_marker;
static bool diting=true,sync_ok=true,allocation_ok=true,number_ok=true;
static unsigned writes,syncs,reads;

static bool brightness18_diting_board(void) { return diting; }
static bool CFEqual(CFTypeRef a,CFTypeRef b) { return a->type==b->type && a->number==b->number; }
static unsigned CFGetTypeID(CFTypeRef value) { return value->type; }
static unsigned CFNumberGetTypeID(void) { return 2; }
static void CFRelease(CFTypeRef value) { assert(value); }
static bool CFNumberGetValue(CFNumberRef value,int type,void *answer) {
    assert(value->type==2 && type==kCFNumberFloat64Type);
    if(!number_ok)return false;
    *(double *)answer=value->number;return true;
}
static CFNumberRef CFNumberCreate(void *allocator,int type,const void *value) {
    assert(!allocator && type==kCFNumberFloat64Type);
    if(!allocation_ok)return NULL;
    created.number=*(const double *)value;return &created;
}
static CFTypeRef CFPreferencesCopyAppValue(CFStringRef key,CFStringRef domain) {
    assert(domain==kCFPreferencesCurrentApplication);++reads;
    if(!strcmp(key,"SBBacklightLevel2"))return saved_level;
    assert(!strcmp(key,"DitingBacklight18InitialValueMigrated"));return saved_marker;
}
static void CFPreferencesSetAppValue(CFStringRef key,CFTypeRef value,CFStringRef domain) {
    assert(domain==kCFPreferencesCurrentApplication);++writes;
    if(!strcmp(key,"SBBacklightLevel2"))saved_level=value;
    else {
        assert(!strcmp(key,"DitingBacklight18InitialValueMigrated"));saved_marker=value;
    }
}
static bool CFPreferencesAppSynchronize(CFStringRef domain) {
    assert(domain==kCFPreferencesCurrentApplication);++syncs;return sync_ok;
}

#include "brightness18_migration.inc"

static void reset(CFTypeRef level) {
    saved_level=level;saved_marker=NULL;reads=writes=syncs=0;
    diting=sync_ok=allocation_ok=number_ok=true;
}
int main(void) {
    reset(&stored_number);
    brightness18_initial_preference();
    assert(saved_level->number==0.5 && saved_marker==kCFBooleanTrue && writes==2 && syncs==1);
    /* Once migrated, an intentional minimum survives every later start. */
    saved_level=&stored_number;writes=syncs=0;
    brightness18_initial_preference();
    assert(saved_level==&stored_number && writes==0 && syncs==0);
    Object positive={2,0.25};reset(&positive);
    brightness18_initial_preference();
    assert(saved_level==&positive && saved_marker==kCFBooleanTrue && writes==1);
    reset(&stored_number);diting=false;
    brightness18_initial_preference();assert(!reads && !writes && !syncs);
    const Object invalid[]={{2,-0.25},{2,NAN},{2,INFINITY},{3,0}};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        reset(&invalid[i]);brightness18_initial_preference();
        assert(saved_level==&invalid[i] && saved_marker==kCFBooleanTrue && writes==1);
    }
    reset(NULL);brightness18_initial_preference();assert(!saved_level && writes==1);
    reset(&stored_number);number_ok=false;
    brightness18_initial_preference();assert(saved_level==&stored_number && writes==1);
    reset(&stored_number);allocation_ok=false;
    brightness18_initial_preference();assert(!saved_marker && !writes && !syncs);
    reset(&stored_number);sync_ok=false;
    brightness18_initial_preference();assert(!saved_marker && syncs==1);
    reset(&stored_number);saved_marker=&other;
    brightness18_initial_preference();assert(saved_level->number==0.5 && saved_marker==kCFBooleanTrue);
    puts("PASS migration");
}

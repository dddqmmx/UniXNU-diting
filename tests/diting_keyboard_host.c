/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../userland/DitingKeyboardHost.h"
#include "keyboard-injection-fixture.h"

int main(void)
{
    const char *hosts[]={"InputUI","Spotlight","SpringBoard","Preferences","MobilePhone"};
    for(unsigned i=0;i<sizeof(hosts)/sizeof(hosts[0]);++i)assert(diting_keyboard_host(hosts[i]));
    const char *others[]={NULL,"","SpotlightHelper","kbd","backboardd","spotlight","InputUI.app"};
    for(unsigned i=0;i<sizeof(others)/sizeof(others[0]);++i)assert(!diting_keyboard_host(others[i]));
    const char *library="/diting/libditing-ui18.dylib",*word="InputUI",*name="Spotlight";
    assert(exec_keyboard_host_alias(library,strlen(library),word,7,name,9,1));
    const char *path="/Applications/Spotlight.app/Spotlight";
    size_t length=strlen(path);exec_inject_basename(&path,&length);
    assert(length==9 && !memcmp(path,"Spotlight",9));
    assert(exec_keyboard_host_alias(library,strlen(library),word,7,path,length,1));
    path="/Applications/Preferences.app/Preferences";length=strlen(path);
    exec_inject_basename(&path,&length);assert(length==11 && !memcmp(path,"Preferences",11));
    path="/Spotlight/";length=strlen(path);exec_inject_basename(&path,&length);
    assert(!length && !exec_keyboard_host_alias(library,strlen(library),word,7,path,length,1));
    for(int mode=-1;mode<=2;++mode)if(mode!=1)
        assert(!exec_keyboard_host_alias(library,strlen(library),word,7,name,9,mode));
    assert(!exec_keyboard_host_alias("/diting/other.dylib",19,word,7,name,9,1));
    assert(!exec_keyboard_host_alias(library,strlen(library),"InputUIHelper",13,name,9,1));
    assert(!exec_keyboard_host_alias(library,strlen(library),word,7,"SpotlightHelper",15,1));
    assert(!exec_keyboard_host_alias(library,strlen(library)-1,word,7,name,9,1));
    /* XNU name components have explicit lengths; a trailing NUL is optional. */
    const char component[9]={'S','p','o','t','l','i','g','h','t'};
    assert(exec_keyboard_host_alias(library,strlen(library),word,7,component,sizeof(component),1));
    path=component;length=sizeof(component);exec_inject_basename(&path,&length);
    assert(path==component && length==sizeof(component));
    puts("PASS keyboard host coverage and bounded legacy injection");
}

/* SPDX-License-Identifier: MIT
 * Included by settings_compat18.c, which runs only inside Preferences.
 * 22H374's SwiftUI list uses ISIcon/ISTypeIcon, independently of UIImage.
 * Recover failed/cached type images with the installed MobileCoreTypes declarations
 * and native IconServices/CoreUI rendering. Never synthesize icon artwork or
 * change the renderer policy of another process.
 */
#ifndef DITING_SETTINGS_ICONS18_H
#define DITING_SETTINGS_ICONS18_H

static IMP settings_type_sync_original,settings_type_async_original;
static Class settings_image_class,settings_descriptor_class;
static bool settings_type_icons_installed;
static CFDictionaryRef settings_type_declarations;
static CFURLRef settings_type_bundle_url;

static CFTypeRef settings_dictionary_value(CFTypeRef value,CFStringRef key)
{
    return value && CFGetTypeID(value)==CFDictionaryGetTypeID()?
        CFDictionaryGetValue((CFDictionaryRef)value,key):NULL;
}
static bool settings_graphic_type(CFTypeRef type)
{
    return type && CFGetTypeID(type)==CFStringGetTypeID() &&
        CFStringHasPrefix((CFStringRef)type,CFSTR("com.apple.graphic-icon."));
}

static void settings_load_type_declarations(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *path="/System/Library/CoreServices/MobileCoreTypes.bundle/Info.plist";
        int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0){record("Settings type icon declarations unavailable");return;}
        struct stat st;
        if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=0 || (st.st_mode&022) ||
           st.st_size<=0 || st.st_size>1024*1024) {close(fd);return;}
        size_t bytes=(size_t)st.st_size,used=0;
        uint8_t *buffer=malloc(bytes);
        if(!buffer){close(fd);return;}
        while(used<bytes) {
            ssize_t n=read(fd,buffer+used,bytes-used);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)break;
            used+=(size_t)n;
        }
        close(fd);
        CFDataRef data=used==bytes?CFDataCreate(NULL,buffer,(CFIndex)bytes):NULL;
        free(buffer);
        if(!data)return;
        CFPropertyListRef plist=CFPropertyListCreateWithData(NULL,data,kCFPropertyListImmutable,NULL,NULL);
        CFRelease(data);
        CFMutableDictionaryRef declarations=CFDictionaryCreateMutable(NULL,0,
            &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        const CFStringRef keys[]={CFSTR("UTImportedTypeDeclarations"),CFSTR("UTExportedTypeDeclarations")};
        for(unsigned k=0;declarations && k<2;k++) {
            CFTypeRef list=settings_dictionary_value(plist,keys[k]);
            if(!list || CFGetTypeID(list)!=CFArrayGetTypeID())continue;
            CFIndex count=CFArrayGetCount((CFArrayRef)list);
            if(count>4096)continue;
            for(CFIndex i=0;i<count;i++) {
                CFTypeRef entry=CFArrayGetValueAtIndex((CFArrayRef)list,i);
                CFTypeRef ident=settings_dictionary_value(entry,CFSTR("UTTypeIdentifier"));
                if(ident && CFGetTypeID(ident)==CFStringGetTypeID())
                    CFDictionarySetValue(declarations,ident,entry);
            }
        }
        if(plist)CFRelease(plist);
        const char *bundle="/System/Library/CoreServices/MobileCoreTypes.bundle";
        if(declarations && CFDictionaryGetCount(declarations)) {
            settings_type_declarations=CFDictionaryCreateCopy(NULL,declarations);
            settings_type_bundle_url=CFURLCreateFromFileSystemRepresentation(NULL,
                (const UInt8 *)bundle,strlen(bundle),true);
        }
        if(declarations)CFRelease(declarations);
        record(settings_type_declarations && settings_type_bundle_url?
            "installed restore type icon declarations loaded":"Settings type icon declarations invalid");
    });
}

/* Follow the actual declaration's parents, e.g. graphic-icon.passwords ->
 * application-icon.passwords. Bound recursion to reject cyclic input; do not
 * substitute a generic document or guess a symbol for an unknown type. */
static CFDictionaryRef settings_type_icon_dictionary(CFStringRef type,unsigned depth,unsigned *budget)
{
    if(!settings_type_declarations || !type || depth>=8 || !*budget)return NULL;
    --*budget;
    CFTypeRef entry=CFDictionaryGetValue(settings_type_declarations,type);
    CFTypeRef icons=settings_dictionary_value(entry,CFSTR("UTTypeIcons"));
    if(icons && CFGetTypeID(icons)==CFDictionaryGetTypeID() &&
       CFDictionaryGetCount((CFDictionaryRef)icons))return (CFDictionaryRef)icons;
    CFTypeRef parents=settings_dictionary_value(entry,CFSTR("UTTypeConformsTo"));
    if(!parents)return NULL;
    if(CFGetTypeID(parents)==CFStringGetTypeID())
        return settings_type_icon_dictionary((CFStringRef)parents,depth+1,budget);
    if(CFGetTypeID(parents)!=CFArrayGetTypeID())return NULL;
    CFIndex count=CFArrayGetCount((CFArrayRef)parents);
    if(count>16)return NULL;
    for(CFIndex i=0;i<count;i++) {
        CFTypeRef parent=CFArrayGetValueAtIndex((CFArrayRef)parents,i);
        if(!parent || CFGetTypeID(parent)!=CFStringGetTypeID())continue;
        CFDictionaryRef found=settings_type_icon_dictionary((CFStringRef)parent,depth+1,budget);
        if(found)return found;
    }
    return NULL;
}
static bool settings_icon_has_bitmap(id value)
{
    if(!value || !((BOOL(*)(id,SEL,Class))objc_msgSend)(value,
        sel_registerName("isKindOfClass:"),settings_image_class))return false;
    if(((BOOL(*)(id,SEL))objc_msgSend)(value,sel_registerName("placeholder")))return false;
    CGImageRef bitmap=((CGImageRef(*)(id,SEL))objc_msgSend)(value,sel_registerName("cgImage"));
    if(!bitmap || !CGImageGetWidth(bitmap) || !CGImageGetHeight(bitmap))return false;
    CGImageAlphaInfo alpha=CGImageGetAlphaInfo(bitmap);
    if(alpha==kCGImageAlphaNone || alpha==kCGImageAlphaNoneSkipFirst ||
       alpha==kCGImageAlphaNoneSkipLast)return true;
    // A cache hit can contain a real CGImage whose pixels are all transparent.
    // Check a small raster in a private memory context; preserve the image if
    // this optional inspection cannot allocate its context. This does not
    // change Metal/CoreImage policy or inspect application icons.
    uint8_t pixels[32*32*4]={0};
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();if(!space)return true;
    CGContextRef context=CGBitmapContextCreate(pixels,32,32,8,32*4,space,
        kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);if(!context)return true;
    CGContextDrawImage(context,CGRectMake(0,0,32,32),bitmap);
    CGContextRelease(context);
    for(size_t i=3;i<sizeof(pixels);i+=4)if(pixels[i])return true;
    return false;
}
static id settings_type_icon_recover(id icon,id descriptor,id original)
{
    id type=((id(*)(id,SEL))objc_msgSend)(icon,sel_registerName("type"));
    if(!settings_graphic_type((CFTypeRef)type))return original;
    if(settings_icon_has_bitmap(original)) {
        static unsigned reported;
        if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<4) {
            char ident[160]={0},line[256];
            (void)CFStringGetCString((CFStringRef)type,ident,sizeof(ident),kCFStringEncodingUTF8);
            snprintf(line,sizeof(line),"type icon %s native=bitmap local=kept",ident);record(line);
        }
        return original;
    }
    if(!descriptor ||
       !((BOOL(*)(id,SEL,Class))objc_msgSend)(descriptor,sel_registerName("isKindOfClass:"),settings_descriptor_class))return original;
    CGSize size=((CGSize(*)(id,SEL))objc_msgSend)(descriptor,sel_registerName("size"));
    double scale=((double(*)(id,SEL))objc_msgSend)(descriptor,sel_registerName("scale"));
    if(!isfinite(size.width) || !isfinite(size.height) || !isfinite(scale) ||
       size.width<=0 || size.height<=0 || scale<=0 ||
       !isfinite(size.width*scale) || !isfinite(size.height*scale) ||
       size.width*scale<1 || size.height*scale<1 ||
       size.width*scale>4096 || size.height*scale>4096)return original;
    settings_load_type_declarations();
    unsigned budget=64;
    CFDictionaryRef icons=settings_type_icon_dictionary((CFStringRef)type,0,&budget);
    if(!icons || !settings_type_bundle_url)return original;
    id resource=((id(*)(id,SEL,id,id,unsigned long))objc_msgSend)(
        (id)objc_lookUpClass("ISResourceProvider"),sel_registerName("resourceWithBundleURL:iconDictionary:options:"),
        (id)settings_type_bundle_url,(id)icons,0);
    id result=nil;
    Class graphic=objc_lookUpClass("ISGraphicSymbolResource");
    if(resource && ((BOOL(*)(id,SEL,Class))objc_msgSend)(resource,sel_registerName("isKindOfClass:"),graphic)) {
        // This is the same native path as ISSymbolIcon, including appearance,
        // contrast, vibrancy, direction, tint and the original recipe colors.
        ((void(*)(id,SEL,id))objc_msgSend)(resource,sel_registerName("updateDescriptorWithImageDescriptor:"),descriptor);
        result=((id(*)(id,SEL,CGSize,double))objc_msgSend)(resource,sel_registerName("imageForSize:scale:"),size,scale);
    } else if(resource) {
        result=((id(*)(id,SEL,id,id,id))objc_msgSend)((id)settings_image_class,
            sel_registerName("_applyTreatmentsAndCacheResultForResource:descriptor:description:"),resource,descriptor,type);
    }
    bool recovered=settings_icon_has_bitmap(result);
    static unsigned reported;
    if(__atomic_fetch_add(&reported,1,__ATOMIC_RELAXED)<12) {
        char ident[160]={0},line[256];
        (void)CFStringGetCString((CFStringRef)type,ident,sizeof(ident),kCFStringEncodingUTF8);
        snprintf(line,sizeof(line),"type icon %s native=%s local=%s",ident,
            original?"placeholder/empty bitmap":"nil",recovered?"bitmap":"unavailable");
        record(line);
    }
    return recovered?result:original;
}
static id settings_type_icon_sync(id self,SEL selector,id descriptor)
{
    id value=((id(*)(id,SEL,id))settings_type_sync_original)(self,selector,descriptor);
    return settings_type_icon_recover(self,descriptor,value);
}

/* C block captures need an explicit NSObject attribute. The native method
 * copies this callback; cancellation/disposal must release the copied
 * descriptor and icon even if the callback is never invoked. */
typedef id SettingsIconObject __attribute__((NSObject));
typedef void (^SettingsIconReply)(id);
static void settings_type_icon_async(id self,SEL selector,id descriptor,SettingsIconReply completion)
{
    if(!completion || !descriptor) {
        ((void(*)(id,SEL,id,SettingsIconReply))settings_type_async_original)(self,selector,descriptor,completion);
        return;
    }
    SettingsIconObject saved_icon=self;
    SettingsIconObject saved_descriptor=((id(*)(id,SEL))objc_msgSend)(descriptor,sel_registerName("copy"));
    if(!saved_descriptor) {
        ((void(*)(id,SEL,id,SettingsIconReply))settings_type_async_original)(self,selector,descriptor,completion);
        return;
    }
    // 22H374's getter completion takes one IFImage argument. It can deliver a
    // cache/store hit without calling generateImageWithDescriptor:. Keep its
    // scheduling and caching; replace only a failed/empty final image.
    ((void(*)(id,SEL,id,SettingsIconReply))settings_type_async_original)(self,selector,descriptor,^(id value) {
        completion(settings_type_icon_recover(saved_icon,saved_descriptor,value));
    });
    ((void(*)(id,SEL))objc_msgSend)(saved_descriptor,sel_registerName("release"));
}
static void settings_type_icons_install(const struct mach_header *head,intptr_t slide)
{
    if(settings_type_icons_installed)return;
    Class type=objc_lookUpClass("ISTypeIcon"),base=objc_lookUpClass("ISConcreteIcon");
    Class resource=objc_lookUpClass("ISResourceProvider"),graphic=objc_lookUpClass("ISGraphicSymbolResource");
    Class bitmap=objc_lookUpClass("IFImage"),descriptor=objc_lookUpClass("ISImageDescriptor");
    if(!type || !base || !resource || !graphic || !bitmap || !descriptor)return;
    // _IconServices_SwiftUI reads imageForDescriptor: and then uses this async
    // getter. Both native methods can bypass generation on cache/store hits.
    SEL sync=sel_registerName("imageForDescriptor:");
    SEL async=sel_registerName("getImageForImageDescriptor:completion:");
    Method sm=class_getInstanceMethod(type,sync),am=class_getInstanceMethod(type,async);
    static const uint8_t uuid[]={0x86,0x6f,0xb2,0x00,0x44,0x84,0x31,0x1e,0xb8,0x12,0x34,0x93,0x57,0x2d,0x2e,0x60};
    if(!abi(sm,"@",1,"@") || !abi(am,"v",2,"@@") ||
       sm!=class_getInstanceMethod(base,sync) || am!=class_getInstanceMethod(base,async) ||
       !image(head,slide,uuid,(uintptr_t)method_getImplementation(sm),4) ||
       !image(head,slide,uuid,(uintptr_t)method_getImplementation(am),4))return;
    if(!abi(class_getInstanceMethod(type,sel_registerName("type")),"@",0,"") ||
       !abi(class_getClassMethod(resource,sel_registerName("resourceWithBundleURL:iconDictionary:options:")),"@",3,"@@Q") ||
       !abi(class_getInstanceMethod(graphic,sel_registerName("updateDescriptorWithImageDescriptor:")),"v",1,"@") ||
       !abi(class_getInstanceMethod(graphic,sel_registerName("imageForSize:scale:")),"@",2,"{d") ||
       !abi(class_getClassMethod(bitmap,sel_registerName("_applyTreatmentsAndCacheResultForResource:descriptor:description:")),"@",3,"@@@") ||
       !abi(class_getInstanceMethod(bitmap,sel_registerName("cgImage")),"^",0,"") ||
       !abi(class_getInstanceMethod(bitmap,sel_registerName("placeholder")),"B",0,"") ||
       !abi(class_getInstanceMethod(descriptor,sel_registerName("size")),"{",0,"") ||
       !abi(class_getInstanceMethod(descriptor,sel_registerName("scale")),"d",0,""))return;
    settings_image_class=bitmap;settings_descriptor_class=descriptor;
    settings_type_sync_original=method_getImplementation(sm);
    settings_type_async_original=method_getImplementation(am);
    // Add overrides to ISTypeIcon itself. Replacing these inherited Methods
    // would also patch ISBundleIcon/application icons in Preferences.
    if(!class_addMethod(type,sync,(IMP)settings_type_icon_sync,method_getTypeEncoding(sm)))return;
    if(!class_addMethod(type,async,(IMP)settings_type_icon_async,method_getTypeEncoding(am))) {
        Method own=class_getInstanceMethod(type,sync);
        if(method_getImplementation(own)==(IMP)settings_type_icon_sync)
            method_setImplementation(own,settings_type_sync_original);
        return;
    }
    settings_type_icons_installed=true;
    record("22H374 Settings type icon cached sync/async recovery installed");
}
#endif

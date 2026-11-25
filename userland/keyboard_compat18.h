/* SPDX-License-Identifier: MIT
 * Keep the native phone keyboard's outer size consistent with its screen.
 * Reuse real dock glyphs with the same immutable native configuration.
 */
#include <pthread.h>

static IMP keyboard_default_size_original,keyboard_intrinsic_original,keyboard_glyph_original;
static IMP keyboard_points_original,keyboard_dictation_original;
static unsigned keyboard_compat_mask;
static char keyboard_glyph_cache_key;
static IMP keyboard_espresso_defaults_original,keyboard_languages_original;
static bool keyboard_espresso_installed,keyboard_languages_installed;
static const void *keyboard_espresso_image;
static unsigned keyboard_language_epoch;
static char keyboard_languages_key;
static char keyboard_dictation_key;
static bool keyboard_dictation_installed,keyboard_dictation_updating;

static bool keyboard_return_struct(Method method, const char *encoding)
{
    char *type=method?method_copyReturnType(method):NULL;
    const bool valid=type && !strcmp(type,encoding);free(type);return valid;
}
static id keyboard_object_getter(id object, const char *name)
{
    SEL selector=sel_registerName(name);
    if(!method_abi(object?class_getInstanceMethod(object_getClass(object),selector):NULL,"@",0,""))return nil;
    return ((id(*)(id,SEL))objc_msgSend)(object,selector);
}
static CGRect keyboard_compat_rect(id view, const char *name)
{
    SEL bounds=sel_registerName(name);
    Method method=view?class_getInstanceMethod(object_getClass(view),bounds):NULL;
    if(!method_abi(method,"{",0,"") || !keyboard_return_struct(method,"{CGRect={CGPoint=dd}{CGSize=dd}}"))return CGRectZero;
    return ((CGRect(*)(id,SEL))objc_msgSend)(view,bounds);
}
static CGRect keyboard_compat_bounds(id view)
{return keyboard_compat_rect(view,"bounds");}
static CGRect keyboard_compat_screen_bounds(void)
{
    Class cls=objc_lookUpClass("UIScreen");SEL main=sel_registerName("mainScreen");
    if(!method_abi(cls?class_getClassMethod(cls,main):NULL,"@",0,""))return CGRectZero;
    return keyboard_compat_bounds(((id(*)(id,SEL))objc_msgSend)((id)cls,main));
}
static double keyboard_points_per_millimeter(id self, SEL selector)
{
    const double native=((double(*)(id,SEL))keyboard_points_original)(self,selector);
    if(isfinite(native) && native>0)return native;
    const CGRect pixels=keyboard_compat_rect(self,"nativeBounds");
    SEL scale_selector=sel_registerName("nativeScale");
    if(!method_abi(class_getInstanceMethod(object_getClass(self),scale_selector),"d",0,""))return native;
    const double scale=((double(*)(id,SEL))objc_msgSend)(self,scale_selector);
    const double width=fmin(pixels.size.width,pixels.size.height),height=fmax(pixels.size.width,pixels.size.height);
    if(!isfinite(width) || !isfinite(height) || !isfinite(scale) ||
       fabs(width-1220)>0.5 || fabs(height-2712)>0.5 || fabs(scale-3)>0.001)return native;
    // The captured SM8475 panel DT records a 70 x 155 mm active panel.
    // 22H374 obtains this metric from MobileGestalt main-screen-pitch;
    // its absent answer made native dock frames and touch padding zero.
    const double points=width/(70.0*scale);
    static unsigned count;
    if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)<4)
        fprintf(stderr,"DitingKeyboardScreen process=%s native_points_per_mm=%.6f panel_points_per_mm=%.6f physical_mm=70,155\n",getprogname(),native,points);
    return points;
}
static CGSize keyboard_default_size(id self, SEL selector, long long orientation)
{
    CGSize size=((CGSize(*)(id,SEL,long long))keyboard_default_size_original)(self,selector,orientation);
    const CGRect screen=keyboard_compat_screen_bounds();
    if(size.width>0 && size.height>0 && screen.size.width>0 && screen.size.height>0 && orientation>=1 && orientation<=4) {
        const double width=orientation<=2?fmin(screen.size.width,screen.size.height):fmax(screen.size.width,screen.size.height);
        if(width>=320 && width<=1200)size.width=width;
    }
    return size;
}
static CGSize keyboard_intrinsic_size(id self, SEL selector)
{
    CGSize size=((CGSize(*)(id,SEL))keyboard_intrinsic_original)(self,selector);
    id parent=keyboard_object_getter(self,"superview");
    if(size.width>0 && parent && object_getClass(parent)==objc_lookUpClass("_UIKBCompatInputView")) {
        const CGRect bounds=keyboard_compat_bounds(parent),screen=keyboard_compat_screen_bounds();
        if(bounds.size.width>=320 && fabs(bounds.size.width-screen.size.width)<0.5)size.width=bounds.size.width;
    }
    return size;
}
static id keyboard_retained_autorelease(id object)
{
    if(!object)return nil;
    ((id(*)(id,SEL))objc_msgSend)(object,sel_registerName("retain"));
    return ((id(*)(id,SEL))objc_msgSend)(object,sel_registerName("autorelease"));
}
static id keyboard_cached_glyph(id self, SEL selector, id name)
{
    id config=keyboard_object_getter(self,"imageConfig");
    SEL equal=sel_registerName("isEqual:"),copy=sel_registerName("copy");
    const bool usable=name && config && method_abi(class_getInstanceMethod(object_getClass(name),equal),"B",1,"@") &&
        method_abi(class_getInstanceMethod(object_getClass(config),equal),"B",1,"@") &&
        method_abi(class_getInstanceMethod(object_getClass(name),copy),"@",0,"") &&
        method_abi(class_getInstanceMethod(object_getClass(config),copy),"@",0,"");
    CFMutableArrayRef cache=(CFMutableArrayRef)objc_getAssociatedObject(self,&keyboard_glyph_cache_key);
    if(usable && cache)for(CFIndex i=0,n=CFArrayGetCount(cache);i<n;i++) {
        CFArrayRef row=(CFArrayRef)CFArrayGetValueAtIndex(cache,i);
        if(((BOOL(*)(id,SEL,id))objc_msgSend)(name,equal,(id)CFArrayGetValueAtIndex(row,0)) &&
           ((BOOL(*)(id,SEL,id))objc_msgSend)(config,equal,(id)CFArrayGetValueAtIndex(row,1)))
            return keyboard_retained_autorelease((id)CFArrayGetValueAtIndex(row,2));
    }
    id result=((id(*)(id,SEL,id))keyboard_glyph_original)(self,selector,name);
    if(!usable || !result)return result;
    if(!cache) {
        cache=CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
        if(!cache)return result;
        objc_setAssociatedObject(self,&keyboard_glyph_cache_key,(id)cache,OBJC_ASSOCIATION_RETAIN_NONATOMIC);CFRelease(cache);
    }
    id saved_name=((id(*)(id,SEL))objc_msgSend)(name,copy),saved_config=((id(*)(id,SEL))objc_msgSend)(config,copy);
    if(saved_name && saved_config) {
        const void *values[]={saved_name,saved_config,result};
        CFArrayRef row=CFArrayCreate(NULL,values,3,&kCFTypeArrayCallBacks);
        if(row) {
            if(CFArrayGetCount(cache)>=16)CFArrayRemoveValueAtIndex(cache,0);
            CFArrayAppendValue(cache,row);CFRelease(row);
        }
    }
    if(saved_name)((void(*)(id,SEL))objc_msgSend)(saved_name,sel_registerName("release"));
    if(saved_config)((void(*)(id,SEL))objc_msgSend)(saved_config,sel_registerName("release"));
    return result;
}
static id keyboard_espresso_defaults(id self, SEL selector, id domain)
{
    const void *caller=__builtin_return_address(0);
    id original=((id(*)(id,SEL,id))keyboard_espresso_defaults_original)(self,selector,domain);
    Dl_info location={0};
    if(!domain || !CFEqual((CFTypeRef)domain,CFSTR("NSGlobalDomain")) || !dladdr(caller,&location) ||
       location.dli_fbase!=keyboard_espresso_image)return original;
    if(original && CFGetTypeID((CFTypeRef)original)!=CFDictionaryGetTypeID())return original;
    CFMutableDictionaryRef settings=original?CFDictionaryCreateMutableCopy(NULL,0,(CFDictionaryRef)original):
        CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if(!settings)return original;
    const int cpu_engine=1;
    CFNumberRef value=CFNumberCreate(NULL,kCFNumberIntType,&cpu_engine);
    if(!value){CFRelease(settings);return original;}
    CFDictionarySetValue(settings,CFSTR("espresso.force_engine"),value);CFRelease(value);
    static unsigned count;
    if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)<8)
        fprintf(stderr,"DitingKeyboardModel process=%s native Espresso engine=CPU preference view; persistent defaults retained\n",getprogname());
    return ((id(*)(id,SEL))objc_msgSend)((id)settings,sel_registerName("autorelease"));
}
static void keyboard_languages_changed(CFNotificationCenterRef center, void *observer, CFStringRef name,
    const void *object, CFDictionaryRef information)
{
    (void)center;(void)observer;(void)name;(void)object;(void)information;
    __atomic_fetch_add(&keyboard_language_epoch,1,__ATOMIC_RELAXED);
}
static id keyboard_cached_languages(id self, SEL selector)
{
    const unsigned epoch=__atomic_load_n(&keyboard_language_epoch,__ATOMIC_RELAXED);
    CFArrayRef cache=(CFArrayRef)objc_getAssociatedObject(self,&keyboard_languages_key);
    int saved=-1;
    if(cache)CFNumberGetValue((CFNumberRef)CFArrayGetValueAtIndex(cache,0),kCFNumberIntType,&saved);
    if(cache && (unsigned)saved==epoch)return keyboard_retained_autorelease((id)CFArrayGetValueAtIndex(cache,1));
    id result=((id(*)(id,SEL))keyboard_languages_original)(self,selector);
    if(result && CFGetTypeID((CFTypeRef)result)==CFArrayGetTypeID() &&
       epoch==__atomic_load_n(&keyboard_language_epoch,__ATOMIC_RELAXED)) {
        CFNumberRef stamp=CFNumberCreate(NULL,kCFNumberIntType,&epoch);
        CFArrayRef languages=CFArrayCreateCopy(NULL,(CFArrayRef)result);
        if(stamp && languages) {
            const void *values[]={stamp,languages};CFArrayRef row=CFArrayCreate(NULL,values,2,&kCFTypeArrayCallBacks);
            if(row){objc_setAssociatedObject(self,&keyboard_languages_key,(id)row,OBJC_ASSOCIATION_RETAIN_NONATOMIC);CFRelease(row);}
        }
        if(stamp)CFRelease(stamp);if(languages)CFRelease(languages);
    }
    return result;
}
static CFDictionaryRef keyboard_dictation_inputs(id self)
{
    id preferences=keyboard_object_getter((id)objc_lookUpClass("UIKeyboardPreferencesController"),"sharedPreferencesController");
    id actions=keyboard_object_getter(preferences,"preferencesActions");
    SEL query=sel_registerName("valueForPreferenceKey:");
    if(!actions || !method_abi(class_getInstanceMethod(object_getClass(actions),query),"@",1,"@"))return NULL;
    id enabled=((id(*)(id,SEL,id))objc_msgSend)(actions,query,(id)CFSTR("DictationLanguagesEnabled"));
    id sls=keyboard_object_getter(self,"getDictationSLSLanguagesEnabled");
    id modes=keyboard_object_getter(self,"activeInputModeIdentifiers");
    id languages=keyboard_object_getter(self,"activeDictationLanguages");
    id suggested=keyboard_object_getter(self,"suggestedDictationLanguagesForDeviceLanguage");
    id disallowed=keyboard_object_getter((id)objc_lookUpClass("UIKeyboardInputModeController"),"disallowedDictationLanguagesForDeviceLanguage");
    CFLocaleRef locale=CFLocaleCopyCurrent();
    if(!locale)return NULL;
    const void *keys[]={CFSTR("enabled"),CFSTR("sls"),CFSTR("modes"),CFSTR("languages"),CFSTR("suggested"),CFSTR("disallowed"),CFSTR("locale")};
    const void *values[]={enabled?(CFTypeRef)enabled:kCFNull,sls?(CFTypeRef)sls:kCFNull,
        modes?(CFTypeRef)modes:kCFNull,languages?(CFTypeRef)languages:kCFNull,
        suggested?(CFTypeRef)suggested:kCFNull,disallowed?(CFTypeRef)disallowed:kCFNull,CFLocaleGetIdentifier(locale)};
    CFMutableDictionaryRef inputs=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if(inputs)for(unsigned i=0;i<7;i++)if(values[i]!=kCFNull)CFDictionarySetValue(inputs,keys[i],values[i]);
    CFDictionaryRef result=inputs?(CFDictionaryRef)CFPropertyListCreateDeepCopy(NULL,inputs,kCFPropertyListImmutable):NULL;
    if(inputs)CFRelease(inputs);CFRelease(locale);
    return result;
}
static id keyboard_cached_dictation(id self, SEL selector, BOOL force)
{
    // Only the native synchronous lookup (force=NO) is memoized. Explicit
    // updates retain their side effects, and live availability is still
    // queried by isDictationLanguageEnabled: after reading this dictionary.
    if(!pthread_main_np() || keyboard_dictation_updating)
        return ((id(*)(id,SEL,BOOL))keyboard_dictation_original)(self,selector,force);
    keyboard_dictation_updating=true;
    CFDictionaryRef inputs=keyboard_dictation_inputs(self);
    CFArrayRef cache=(CFArrayRef)objc_getAssociatedObject(self,&keyboard_dictation_key);
    if(!force && inputs && cache && CFEqual(inputs,CFArrayGetValueAtIndex(cache,0))) {
        CFMutableDictionaryRef result=CFDictionaryCreateMutableCopy(NULL,0,(CFDictionaryRef)CFArrayGetValueAtIndex(cache,1));
        CFRelease(inputs);
        if(result) {
            keyboard_dictation_updating=false;
            static unsigned count;
            if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)<8)
                fprintf(stderr,"DitingKeyboardDictation process=%s reused_native_refresh=1 inputs_unchanged=1 live_availability_retained=1\n",getprogname());
            return ((id(*)(id,SEL))objc_msgSend)((id)result,sel_registerName("autorelease"));
        }
        inputs=NULL;
    }
    if(force)objc_setAssociatedObject(self,&keyboard_dictation_key,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    id result=((id(*)(id,SEL,BOOL))keyboard_dictation_original)(self,selector,force);
    CFDictionaryRef after=keyboard_dictation_inputs(self);
    if(inputs && after && CFEqual(inputs,after) && result && CFGetTypeID((CFTypeRef)result)==CFDictionaryGetTypeID()) {
        CFDictionaryRef saved=(CFDictionaryRef)CFPropertyListCreateDeepCopy(NULL,(CFTypeRef)result,kCFPropertyListImmutable);
        if(saved) {
            const void *values[]={after,saved};
            CFArrayRef row=CFArrayCreate(NULL,values,2,&kCFTypeArrayCallBacks);
            if(row){objc_setAssociatedObject(self,&keyboard_dictation_key,(id)row,OBJC_ASSOCIATION_RETAIN_NONATOMIC);CFRelease(row);}
            CFRelease(saved);
        }
    }
    if(inputs)CFRelease(inputs);if(after)CFRelease(after);
    keyboard_dictation_updating=false;
    return result;
}
static void keyboard_compat_image(const struct mach_header *header, intptr_t slide)
{
    (void)header;(void)slide;
    const char *classes[]={"UIKeyboardImpl","UIKeyboardAutomatic","UISystemKeyboardDockController","UIScreen"};
    const char *selectors[]={"defaultSizeForInterfaceOrientation:","intrinsicContentSize","configuredGlyphWithName:","_nativePointsPerMillimeter"};
    IMP *originals[]={&keyboard_default_size_original,&keyboard_intrinsic_original,&keyboard_glyph_original,&keyboard_points_original};
    const IMP replacements[]={(IMP)keyboard_default_size,(IMP)keyboard_intrinsic_size,(IMP)keyboard_cached_glyph,(IMP)keyboard_points_per_millimeter};
    for(unsigned index=0;index<4;index++) {
        if(keyboard_compat_mask&(1u<<index))continue;
        Class cls=objc_lookUpClass(classes[index]);Class target=index==0?object_getClass((id)cls):cls;
        SEL selector=sel_registerName(selectors[index]);Method method=target?class_getInstanceMethod(target,selector):NULL;
        if(!method_abi(method,index==3?"d":index==2?"@":"{",index==1 || index==3?0:1,index==0?"q":index==2?"@":""))continue;
        if(index<2 && !keyboard_return_struct(method,"{CGSize=dd}"))continue;
        *originals[index]=method_getImplementation(method);
        class_replaceMethod(target,selector,replacements[index],method_getTypeEncoding(method));
        keyboard_compat_mask|=1u<<index;
        fprintf(stderr,"Diting keyboard: %s native outer sizing/glyph compatibility installed=%x\n",getprogname(),keyboard_compat_mask);
    }
    if(!keyboard_dictation_installed) {
        Class cls=objc_lookUpClass("UIKeyboardInputModeController");SEL selector=sel_registerName("updateEnabledDictationLanguages:");
        Method method=cls?class_getInstanceMethod(cls,selector):NULL;
        const char *getters[]={"getDictationSLSLanguagesEnabled","activeInputModeIdentifiers","activeDictationLanguages","suggestedDictationLanguagesForDeviceLanguage"};
        bool valid=method_abi(method,"@",1,"B") && method_abi(cls?class_getClassMethod(cls,sel_registerName("disallowedDictationLanguagesForDeviceLanguage")):NULL,"@",0,"");
        for(unsigned i=0;i<4 && valid;i++)valid=method_abi(class_getInstanceMethod(cls,sel_registerName(getters[i])),"@",0,"");
        if(valid) {
            keyboard_dictation_original=method_getImplementation(method);
            class_replaceMethod(cls,selector,(IMP)keyboard_cached_dictation,method_getTypeEncoding(method));
            keyboard_dictation_installed=true;
        }
    }
    if(!keyboard_languages_installed) {
        Class cls=objc_lookUpClass("UIKeyboardInputModeController");SEL selector=sel_registerName("suggestedDictationLanguagesForDeviceLanguage");
        Method method=cls?class_getInstanceMethod(cls,selector):NULL;
        if(method_abi(method,"@",0,"")) {
            keyboard_languages_original=method_getImplementation(method);
            class_replaceMethod(cls,selector,(IMP)keyboard_cached_languages,method_getTypeEncoding(method));
            CFNotificationCenterRef center=CFNotificationCenterGetLocalCenter();
            CFNotificationCenterAddObserver(center,&keyboard_languages_key,keyboard_languages_changed,CFSTR("NSCurrentLocaleDidChangeNotification"),NULL,CFNotificationSuspensionBehaviorDeliverImmediately);
            CFNotificationCenterAddObserver(center,&keyboard_languages_key,keyboard_languages_changed,CFSTR("NSUserDefaultsDidChangeNotification"),NULL,CFNotificationSuspensionBehaviorDeliverImmediately);
            keyboard_languages_installed=true;
        }
    }
    if(!keyboard_espresso_installed && !strcmp(getprogname(),"Spotlight") && header && header->magic==MH_MAGIC_64) {
        const struct mach_header_64 *h=(const void *)header;
        const struct load_command *command=(const void *)(h+1);const char *end=(const char *)command+h->sizeofcmds;
        static const uint8_t uuid[]={0xf6,0x65,0xc5,0x85,0x77,0x07,0x31,0x9e,0xa6,0x73,0x34,0xed,0x06,0x98,0x4b,0x96};
        bool matched=false;
        for(unsigned i=0;i<h->ncmds;i++) {
            if((const char *)command+sizeof(*command)>end || command->cmdsize<sizeof(*command) || command->cmdsize>(size_t)(end-(const char *)command))return;
            if(command->cmd==LC_UUID && command->cmdsize==sizeof(struct uuid_command) && !memcmp(((const struct uuid_command *)command)->uuid,uuid,16))matched=true;
            command=(const void *)((const char *)command+command->cmdsize);
        }
        if(matched) {
            Class cls=objc_lookUpClass("NSUserDefaults");SEL selector=sel_registerName("persistentDomainForName:");
            Method method=cls?class_getInstanceMethod(cls,selector):NULL;
            if(method_abi(method,"@",1,"@")) {
                keyboard_espresso_defaults_original=method_getImplementation(method);keyboard_espresso_image=header;
                class_replaceMethod(cls,selector,(IMP)keyboard_espresso_defaults,method_getTypeEncoding(method));
                keyboard_espresso_installed=true;
            }
        }
    }
}

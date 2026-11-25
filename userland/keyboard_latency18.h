/* SPDX-License-Identifier: MIT
 * Optional timing of real 22H374 keyboard calls. Arguments and results are
 * passed unchanged. Geometry and system image names are bounded diagnostics;
 * input text, key contents and preferences are never logged.
 */
#include <mach/mach_time.h>
#include "DitingKeyboardNative.h"

static IMP keyboard_timing_original[12];
static unsigned keyboard_timing_mask, keyboard_timing_count, keyboard_abi_mask;
static unsigned keyboard_timing_op_count[12];
__attribute__((section("__DATA,__diting_kbd")))
struct DitingKeyboardNativeDiagnostic diting_keyboard_native_diagnostic={
    .magic=DitingKeyboardNativeMagic,.version=1,.bytes=sizeof(struct DitingKeyboardNativeDiagnostic),
    .methods=DitingKeyboardNativeMethods};
static mach_timebase_info_data_t keyboard_timing_scale;
static const char *keyboard_timing_classes[] = {
    "UIKeyboardLayoutStar", "UIKeyboardLayoutStar", "UIKeyboardLayoutStar",
    "UIKeyboardImpl", "UIKeyboardImpl", "UIKeyboardImpl", "UIKeyboardImpl", "UIKBRenderer", "UIKeyboardLayoutStar",
    "UIKBSplitImageView", "UISystemKeyboardDockController", "UIImage"};
static const char *keyboard_timing_selectors[] = {
    "touchDown:executionContext:", "touchUp:executionContext:",
    "showKeyboardWithInputTraits:screenTraits:splitTraits:",
    "handleKeyboardInput:executionContext:", "addInputString:fromVariantKey:",
    "playInputClick", "setKeyboardInputMode:userInitiated:", "renderShadowEffect:withTraits:", "layoutSubviews",
    "setImage:cachedWidth:keyplane:", "configuredGlyphWithName:", "imageNamed:inBundle:withConfiguration:"};
static uint64_t keyboard_timing_now(void)
{
    return (uint64_t)((__uint128_t)mach_absolute_time() * keyboard_timing_scale.numer / keyboard_timing_scale.denom);
}
static void keyboard_timing_end(unsigned index, uint64_t start)
{
    const uint64_t end = keyboard_timing_now();
    struct DitingKeyboardNativeCall *call=&diting_keyboard_native_diagnostic.calls[index];
    const uint64_t duration=end-start;
    __atomic_fetch_add(&diting_keyboard_native_diagnostic.writers,1,__ATOMIC_ACQ_REL);
    __atomic_fetch_add(&diting_keyboard_native_diagnostic.epoch,1,__ATOMIC_RELEASE);
    __atomic_fetch_add(&call->calls,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&call->total_ns,duration,__ATOMIC_RELAXED);
    uint64_t previous=__atomic_load_n(&call->max_ns,__ATOMIC_RELAXED);
    for(unsigned attempt=0;previous<duration && attempt<16;attempt++)
        if(__atomic_compare_exchange_n(&call->max_ns,&previous,duration,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED))break;
    __atomic_store_n(&call->last_start_ns,start,__ATOMIC_RELAXED);
    __atomic_store_n(&call->last_end_ns,end,__ATOMIC_RELAXED);
    __atomic_fetch_add(&diting_keyboard_native_diagnostic.epoch,1,__ATOMIC_RELEASE);
    __atomic_fetch_sub(&diting_keyboard_native_diagnostic.writers,1,__ATOMIC_RELEASE);
    const unsigned count = __atomic_fetch_add(&keyboard_timing_count, 1, __ATOMIC_RELAXED);
    const unsigned per_op=__atomic_fetch_add(&keyboard_timing_op_count[index],1,__ATOMIC_RELAXED);
    if (per_op < 128)
        fprintf(stderr, "DitingKeyboardTiming schema=1 process=%s pid=%d id=%u call=%u class=%s op=%s start_ns=%llu end_ns=%llu duration_us=%llu\n",
            getprogname(), getpid(), count, per_op, keyboard_timing_classes[index], keyboard_timing_selectors[index],
            (unsigned long long)start, (unsigned long long)end, (unsigned long long)((end - start) / 1000));
    else if (per_op == 128)
        fprintf(stderr, "DitingKeyboardTiming schema=1 process=%s pid=%d op=%s limit=128 counters=active\n", getprogname(), getpid(),keyboard_timing_selectors[index]);
}
#define KEYBOARD_TIMING_V2(index, name) \
    static void name(id self, SEL selector, id first, id second) { \
        const uint64_t start = keyboard_timing_now(); \
        ((void (*)(id,SEL,id,id))keyboard_timing_original[index])(self,selector,first,second); \
        keyboard_timing_end(index,start); \
    }
KEYBOARD_TIMING_V2(0, keyboard_timing_down)
KEYBOARD_TIMING_V2(1, keyboard_timing_up)
KEYBOARD_TIMING_V2(3, keyboard_timing_input)
#undef KEYBOARD_TIMING_V2
static void keyboard_log_traits(id traits);
static void keyboard_timing_show(id self, SEL selector, id first, id second, id third)
{
    const uint64_t start = keyboard_timing_now();
    ((void (*)(id,SEL,id,id,id))keyboard_timing_original[2])(self,selector,first,second,third);
    keyboard_timing_end(2,start);
    keyboard_log_traits(second);
}
#define KEYBOARD_TIMING_BOOL(index, name) \
    static void name(id self, SEL selector, id value, BOOL flag) { \
        const uint64_t start = keyboard_timing_now(); \
        ((void (*)(id,SEL,id,BOOL))keyboard_timing_original[index])(self,selector,value,flag); \
        keyboard_timing_end(index,start); \
    }
KEYBOARD_TIMING_BOOL(4, keyboard_timing_insert)
KEYBOARD_TIMING_BOOL(6, keyboard_timing_mode)
#undef KEYBOARD_TIMING_BOOL
static BOOL keyboard_timing_click(id self, SEL selector)
{
    const uint64_t start = keyboard_timing_now();
    const BOOL result = ((BOOL (*)(id,SEL))keyboard_timing_original[5])(self,selector);
    keyboard_timing_end(5,start);
    return result;
}
static id keyboard_timing_shadow(id self, SEL selector, id effect, id traits)
{
    const uint64_t start = keyboard_timing_now();
    const id result = ((id (*)(id,SEL,id,id))keyboard_timing_original[7])(self,selector,effect,traits);
    keyboard_timing_end(7,start);
    return result;
}
static void keyboard_timing_shadow_void(id self, SEL selector, id effect, id traits)
{
    const uint64_t start=keyboard_timing_now();
    ((void (*)(id,SEL,id,id))keyboard_timing_original[7])(self,selector,effect,traits);
    keyboard_timing_end(7,start);
}
static bool keyboard_rect_abi(Method method)
{
    if(!method || method_getNumberOfArguments(method)!=2)return false;
    char *type=method_copyReturnType(method);
    const bool valid=type && !strcmp(type,"{CGRect={CGPoint=dd}{CGSize=dd}}");
    free(type);return valid;
}
static bool keyboard_subclass(id object, const char *name)
{
    Class wanted=objc_lookUpClass(name);
    if(!object || !wanted)return false;
    for(Class cls=object_getClass(object);cls;cls=class_getSuperclass(cls))
        if(cls==wanted)return true;
    return false;
}
static bool keyboard_struct_getter(id object, SEL selector, const char *encoding)
{
    Method method=object?class_getInstanceMethod(object_getClass(object),selector):NULL;
    if(!method || method_getNumberOfArguments(method)!=2)return false;
    char *type=method_copyReturnType(method);
    const bool valid=type && !strcmp(type,encoding);
    free(type);return valid;
}
static CGRect keyboard_view_rect(id view, const char *name)
{
    SEL selector=sel_registerName(name);
    if(!keyboard_subclass(view,"UIView") || !keyboard_rect_abi(class_getInstanceMethod(object_getClass(view),selector)))
        return CGRectZero;
    return ((CGRect(*)(id,SEL))objc_msgSend)(view,selector);
}
static CGSize keyboard_image_size(id image)
{
    SEL selector=sel_registerName("size");
    if(!keyboard_subclass(image,"UIImage") || !keyboard_struct_getter(image,selector,"{CGSize=dd}"))return CGSizeZero;
    return ((CGSize(*)(id,SEL))objc_msgSend)(image,selector);
}
static double keyboard_double(id object, const char *name)
{
    SEL selector=sel_registerName(name);
    if(!method_abi(object?class_getInstanceMethod(object_getClass(object),selector):NULL,"d",0,""))return 0;
    return ((double(*)(id,SEL))objc_msgSend)(object,selector);
}
static void keyboard_log_traits(id traits)
{
    static unsigned count;
    if(!keyboard_subclass(traits,"UIKBScreenTraits") || __atomic_fetch_add(&count,1,__ATOMIC_RELAXED)>=12)return;
    SEL bounds=sel_registerName("bounds"),reference=sel_registerName("keyboardScreenReferenceSize"),stretch=sel_registerName("stretchFactor");
    if(!keyboard_rect_abi(class_getInstanceMethod(object_getClass(traits),bounds)) ||
       !keyboard_struct_getter(traits,reference,"{CGSize=dd}") || !keyboard_struct_getter(traits,stretch,"{CGSize=dd}"))return;
    const CGRect b=((CGRect(*)(id,SEL))objc_msgSend)(traits,bounds);
    const CGSize r=((CGSize(*)(id,SEL))objc_msgSend)(traits,reference),s=((CGSize(*)(id,SEL))objc_msgSend)(traits,stretch);
    fprintf(stderr,"DitingKeyboardTraits schema=1 process=%s pid=%d bounds=%.2f,%.2f,%.2f,%.2f width=%.2f reference=%.2f,%.2f stretch=%.4f,%.4f screen_ratio=%.4f\n",
        getprogname(),getpid(),b.origin.x,b.origin.y,b.size.width,b.size.height,
        keyboard_double(traits,"keyboardWidth"),r.width,r.height,s.width,s.height,keyboard_double(traits,"screenToNativeScaleRatio"));
}
static void keyboard_trace_views(id view, unsigned group, unsigned depth, unsigned *nodes)
{
    if(depth>8 || *nodes>=64 || !keyboard_subclass(view,"UIView"))return;
    const unsigned node=(*nodes)++;
    const CGRect f=keyboard_view_rect(view,"frame"),b=keyboard_view_rect(view,"bounds");
    CGRect converted=CGRectZero;
    SEL convert=sel_registerName("convertRect:toView:");
    Method method=class_getInstanceMethod(object_getClass(view),convert);
    if(method_abi(method,"{",2,"{@")) {
        char *type=method_copyReturnType(method);
        const bool valid=type && !strcmp(type,"{CGRect={CGPoint=dd}{CGSize=dd}}");free(type);
        if(valid)converted=((CGRect(*)(id,SEL,CGRect,id))objc_msgSend)(view,convert,b,nil);
    }
    fprintf(stderr,"DitingKeyboardView schema=1 process=%s pid=%d group=%u node=%u depth=%u class=%s frame=%.2f,%.2f,%.2f,%.2f bounds=%.2f,%.2f,%.2f,%.2f window_rect=%.2f,%.2f,%.2f,%.2f\n",
        getprogname(),getpid(),group,node,depth,object_getClassName(view),
        f.origin.x,f.origin.y,f.size.width,f.size.height,b.origin.x,b.origin.y,b.size.width,b.size.height,
        converted.origin.x,converted.origin.y,converted.size.width,converted.size.height);
    SEL children=sel_registerName("subviews");
    if(depth==8 || !method_abi(class_getInstanceMethod(object_getClass(view),children),"@",0,""))return;
    id list=((id(*)(id,SEL))objc_msgSend)(view,children);
    if(!keyboard_subclass(list,"NSArray"))return;
    for(CFIndex i=0,n=CFArrayGetCount((CFArrayRef)list);i<n && *nodes<64;i++)
        keyboard_trace_views((id)CFArrayGetValueAtIndex((CFArrayRef)list,i),group,depth+1,nodes);
}
static void keyboard_trace_parents(id view, unsigned group)
{
    SEL parent=sel_registerName("superview"),traits=sel_registerName("traitCollection"),idiom=sel_registerName("userInterfaceIdiom"),transform=sel_registerName("transform");
    for(unsigned depth=0;depth<8 && keyboard_subclass(view,"UIView");depth++) {
        const CGRect f=keyboard_view_rect(view,"frame"),b=keyboard_view_rect(view,"bounds");
        CGAffineTransform t=CGAffineTransformIdentity;
        if(keyboard_struct_getter(view,transform,"{CGAffineTransform=dddddd}"))
            t=((CGAffineTransform(*)(id,SEL))objc_msgSend)(view,transform);
        long long value=-1;
        if(method_abi(class_getInstanceMethod(object_getClass(view),traits),"@",0,"")) {
            id collection=((id(*)(id,SEL))objc_msgSend)(view,traits);
            if(method_abi(collection?class_getInstanceMethod(object_getClass(collection),idiom):NULL,"q",0,""))
                value=((long long(*)(id,SEL))objc_msgSend)(collection,idiom);
        }
        fprintf(stderr,"DitingKeyboardParent schema=1 process=%s pid=%d group=%u depth=%u class=%s frame=%.2f,%.2f,%.2f,%.2f bounds=%.2f,%.2f,%.2f,%.2f transform=%.4f,%.4f,%.4f,%.4f,%.2f,%.2f idiom=%lld\n",
            getprogname(),getpid(),group,depth,object_getClassName(view),f.origin.x,f.origin.y,f.size.width,f.size.height,
            b.origin.x,b.origin.y,b.size.width,b.size.height,t.a,t.b,t.c,t.d,t.tx,t.ty,value);
        if(!method_abi(class_getInstanceMethod(object_getClass(view),parent),"@",0,""))break;
        view=((id(*)(id,SEL))objc_msgSend)(view,parent);
    }
}
static void keyboard_timing_split_image(id self, SEL selector, id image, double cached_width, id keyplane)
{
    const uint64_t start=keyboard_timing_now();
    ((void(*)(id,SEL,id,double,id))keyboard_timing_original[9])(self,selector,image,cached_width,keyplane);
    keyboard_timing_end(9,start);
    static unsigned count;
    const unsigned group=__atomic_fetch_add(&count,1,__ATOMIC_RELAXED);
    if(group>=24)return;
    const CGSize size=keyboard_image_size(image);
    fprintf(stderr,"DitingKeyboardSplit schema=1 process=%s pid=%d group=%u cached_width=%.2f image=%.2f,%.2f scale=%.2f\n",
        getprogname(),getpid(),group,cached_width,size.width,size.height,keyboard_double(image,"scale"));
    unsigned nodes=0;keyboard_trace_views(self,100+group,0,&nodes);
}
static void keyboard_asset_name(id value, char *buffer, size_t bytes)
{
    buffer[0]=0;
    if(keyboard_subclass(value,"NSString"))
        (void)CFStringGetCString((CFStringRef)value,buffer,bytes,kCFStringEncodingUTF8);
}
static id keyboard_timing_glyph(id self, SEL selector, id name)
{
    const uint64_t start=keyboard_timing_now();
    id result=((id(*)(id,SEL,id))keyboard_timing_original[10])(self,selector,name);
    keyboard_timing_end(10,start);
    static unsigned count;
    if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)<32) {
        char asset[128];keyboard_asset_name(name,asset,sizeof(asset));
        const CGSize size=keyboard_image_size(result);
        fprintf(stderr,"DitingKeyboardAsset schema=1 process=%s pid=%d source=dock name=%s found=%u image=%.2f,%.2f\n",
            getprogname(),getpid(),asset,result!=nil,size.width,size.height);
    }
    return result;
}
static id keyboard_timing_named_image(id self, SEL selector, id name, id bundle, id configuration)
{
    const uint64_t start=keyboard_timing_now();
    id result=((id(*)(id,SEL,id,id,id))keyboard_timing_original[11])(self,selector,name,bundle,configuration);
    keyboard_timing_end(11,start);
    static unsigned count;
    if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)<48) {
        char asset[128],path[256]={0};keyboard_asset_name(name,asset,sizeof(asset));
        SEL query=sel_registerName("bundlePath");
        if(keyboard_subclass(bundle,"NSBundle") && method_abi(class_getInstanceMethod(object_getClass(bundle),query),"@",0,""))
            keyboard_asset_name(((id(*)(id,SEL))objc_msgSend)(bundle,query),path,sizeof(path));
        fprintf(stderr,"DitingKeyboardAsset schema=1 process=%s pid=%d source=named name=%s bundle=%s found=%u\n",
            getprogname(),getpid(),asset,path,result!=nil);
    }
    return result;
}
static void keyboard_timing_layout(id self, SEL selector)
{
    const uint64_t start=keyboard_timing_now();
    ((void (*)(id,SEL))keyboard_timing_original[8])(self,selector);
    keyboard_timing_end(8,start);
    static unsigned count;
    const unsigned group=__atomic_fetch_add(&count,1,__ATOMIC_RELAXED);
    if(group>=24)return;
    Class screen_class=objc_lookUpClass("UIScreen");
    SEL main=sel_registerName("mainScreen"),frame=sel_registerName("frame"),bounds=sel_registerName("bounds");
    if(!method_abi(class_getClassMethod(screen_class,main),"@",0,"") ||
       !keyboard_rect_abi(class_getInstanceMethod(object_getClass(self),frame)) ||
       !keyboard_rect_abi(class_getInstanceMethod(object_getClass(self),bounds)) ||
       !keyboard_rect_abi(class_getInstanceMethod(screen_class,bounds)))return;
    id screen=((id(*)(id,SEL))objc_msgSend)((id)screen_class,main);
    if(!screen)return;
    const CGRect f=((CGRect(*)(id,SEL))objc_msgSend)(self,frame);
    const CGRect b=((CGRect(*)(id,SEL))objc_msgSend)(self,bounds);
    const CGRect s=((CGRect(*)(id,SEL))objc_msgSend)(screen,bounds);
    SEL window_selector=sel_registerName("window"),keys_selector=sel_registerName("_paddedKeyUnionFrame");
    CGRect w=CGRectZero,k=CGRectZero;
    if(method_abi(class_getInstanceMethod(object_getClass(self),window_selector),"@",0,"")) {
        id window=((id(*)(id,SEL))objc_msgSend)(self,window_selector);
        if(window && keyboard_rect_abi(class_getInstanceMethod(object_getClass(window),frame)))
            w=((CGRect(*)(id,SEL))objc_msgSend)(window,frame);
    }
    if(keyboard_rect_abi(class_getInstanceMethod(object_getClass(self),keys_selector)))
        k=((CGRect(*)(id,SEL))objc_msgSend)(self,keys_selector);
    fprintf(stderr,"DitingKeyboardGeometry schema=1 process=%s pid=%d frame=%.2f,%.2f,%.2f,%.2f bounds=%.2f,%.2f,%.2f,%.2f screen=%.2f,%.2f,%.2f,%.2f\n",
        getprogname(),getpid(),f.origin.x,f.origin.y,f.size.width,f.size.height,
        b.origin.x,b.origin.y,b.size.width,b.size.height,s.origin.x,s.origin.y,s.size.width,s.size.height);
    fprintf(stderr,"DitingKeyboardPlacement schema=1 process=%s pid=%d window=%.2f,%.2f,%.2f,%.2f keys=%.2f,%.2f,%.2f,%.2f\n",
        getprogname(),getpid(),w.origin.x,w.origin.y,w.size.width,w.size.height,k.origin.x,k.origin.y,k.size.width,k.size.height);
    if(group<6){unsigned nodes=0;keyboard_trace_views(self,group,0,&nodes);keyboard_trace_parents(self,group);}
    if(group==0 || group==2) {
        id window=keyboard_object_getter(self,"window");
        unsigned nodes=0;keyboard_trace_views(window,900+group,0,&nodes);
    }
}
static void keyboard_timing_image(const struct mach_header *header, intptr_t slide)
{
    (void)header; (void)slide;
    const IMP replacements[] = {(IMP)keyboard_timing_down, (IMP)keyboard_timing_up,
        (IMP)keyboard_timing_show, (IMP)keyboard_timing_input, (IMP)keyboard_timing_insert,
        (IMP)keyboard_timing_click, (IMP)keyboard_timing_mode, (IMP)keyboard_timing_shadow, (IMP)keyboard_timing_layout,
        (IMP)keyboard_timing_split_image,(IMP)keyboard_timing_glyph,(IMP)keyboard_timing_named_image};
    const char *result_types[] = {"v","v","v","v","v","B","v","v@^","v","v","@","@"};
    const char *argument_types[] = {"@@","@@","@@@","@@","@B","","@B","@@","","@d@","@","@@@"};
    const unsigned arguments[] = {2,2,3,2,2,0,2,2,0,3,1,3};
    const unsigned before = keyboard_timing_mask;
    for (unsigned index = 0; index < 12; ++index) {
        if (keyboard_timing_mask & (1u << index)) continue;
        Class cls = objc_lookUpClass(keyboard_timing_classes[index]);
        SEL selector = sel_registerName(keyboard_timing_selectors[index]);
        Class target=index==11?object_getClass((id)cls):cls;
        Method method = target ? class_getInstanceMethod(target, selector) : NULL;
        if(method && !(keyboard_abi_mask & (1u << index))) {
            keyboard_abi_mask |= 1u << index;
            fprintf(stderr,"DitingKeyboardABI schema=1 process=%s class=%s op=%s types=%s\n",
                getprogname(),keyboard_timing_classes[index],keyboard_timing_selectors[index],method_getTypeEncoding(method));
        }
        if (!method_abi(method, result_types[index], arguments[index], argument_types[index])) continue;
        keyboard_timing_original[index] = method_getImplementation(method);
        const IMP replacement=index==7 && method_abi(method,"v",2,"@@")?(IMP)keyboard_timing_shadow_void:replacements[index];
        class_replaceMethod(target, selector, replacement, method_getTypeEncoding(method));
        keyboard_timing_mask |= 1u << index;
    }
    if (before != keyboard_timing_mask)
        fprintf(stderr, "DitingKeyboardTiming schema=1 process=%s pid=%d installed=%x\n",
            getprogname(), getpid(), keyboard_timing_mask);
}
static void keyboard_abort(int number, siginfo_t *info, void *raw)
{
    (void)info;
    const ucontext_t *context=raw;
    const uint64_t pc=context->uc_mcontext->__ss.__pc,lr=context->uc_mcontext->__ss.__lr;
    uint64_t fp=context->uc_mcontext->__ss.__fp;
    char line[256];
    int bytes=snprintf(line,sizeof(line),"DitingKeyboardAbort process=%s pid=%d signal=%d pc=%llx lr=%llx\n",
        getprogname(),getpid(),number,(unsigned long long)pc,(unsigned long long)lr);
    if(bytes>0 && bytes<(int)sizeof(line))(void)write(STDERR_FILENO,line,(size_t)bytes);
    for(unsigned index=0;index<24 && fp && !(fp&15);index++) {
        uint64_t pair[2];vm_size_t got=0;
        if(vm_read_overwrite(mach_task_self(),(vm_address_t)fp,sizeof(pair),(vm_address_t)pair,&got)!=KERN_SUCCESS || got!=sizeof(pair))break;
        bytes=snprintf(line,sizeof(line),"DitingKeyboardAbort process=%s pid=%d frame=%u address=%llx\n",
            getprogname(),getpid(),index,(unsigned long long)pair[1]);
        if(bytes>0 && bytes<(int)sizeof(line))(void)write(STDERR_FILENO,line,(size_t)bytes);
        if(pair[0]<=fp || pair[0]-fp>8*1024*1024)break;
        fp=pair[0];
    }
    // SA_RESETHAND restores the original default fatal action. Preserve the
    // abort even when it was delivered outside libc's abort() implementation.
    (void)kill(getpid(),number);
}
static void keyboard_timing_start(void)
{
    const char *name = getprogname();
    if (!diting_keyboard_host(name)) return;
    if (!boot_option("diting_kbd=3")) return;
    if (mach_timebase_info(&keyboard_timing_scale) || !keyboard_timing_scale.numer || !keyboard_timing_scale.denom) return;
    // Managed applications can inherit /dev/null rather than launchd's
    // console file actions. Route the requested bounded diagnostic there.
    int console=open("/dev/console",O_WRONLY|O_NOCTTY);
    if(console>=0 && console!=STDERR_FILENO){(void)dup2(console,STDERR_FILENO);close(console);}
    setvbuf(stderr,NULL,_IONBF,0);
    if(!strcmp(name,"Spotlight")) {
        struct sigaction previous,action={0};
        if(!sigaction(SIGABRT,NULL,&previous) && previous.sa_handler==SIG_DFL) {
            action.sa_sigaction=keyboard_abort;action.sa_flags=SA_SIGINFO|SA_RESETHAND;
            sigemptyset(&action.sa_mask);(void)sigaction(SIGABRT,&action,NULL);
        }
    }
    _dyld_register_func_for_add_image(keyboard_timing_image);
}

/* SPDX-License-Identifier: MIT
 * Fixed keyboard-only diagnostic. No application task is suspended or changed.
 * The sampler reads bounded thread stacks in named keyboard hosts and services.
 */
#include "DitingKeyboardNative.h"
static NSString *keyboard_hex(uint64_t address) {
    return [NSString stringWithFormat:@"%llx", (unsigned long long)address];
}
static bool keyboard_read(task_t task, uint64_t address, void *out, size_t bytes) {
    vm_size_t got = 0;
    return address && bytes <= 1024 &&
        vm_read_overwrite(task, (vm_address_t)address, bytes, (vm_address_t)out, &got) == KERN_SUCCESS && got == bytes;
}
static uint64_t keyboard_native_section(task_t task, uint64_t loaded, const mach_header_64 &header, uint64_t preferred) {
    uint64_t at=loaded+sizeof(header);
    for(unsigned command=0;command<header.ncmds;command++) {
        load_command current{};
        if(!keyboard_read(task,at,&current,sizeof(current)) || current.cmdsize<sizeof(current) ||
           at+current.cmdsize>loaded+sizeof(header)+header.sizeofcmds)break;
        if(current.cmd==LC_SEGMENT_64 && current.cmdsize>=sizeof(segment_command_64)) {
            segment_command_64 segment{};
            if(!keyboard_read(task,at,&segment,sizeof(segment)) || segment.nsects>32 ||
               sizeof(segment)+size_t(segment.nsects)*sizeof(section_64)>current.cmdsize)break;
            for(unsigned index=0;index<segment.nsects;index++) {
                section_64 section{};
                if(!keyboard_read(task,at+sizeof(segment)+size_t(index)*sizeof(section),&section,sizeof(section)))break;
                if(!strncmp(section.sectname,"__diting_kbd",16) && !strncmp(section.segname,"__DATA",16) &&
                   section.size==sizeof(DitingKeyboardNativeDiagnostic) && section.addr>=segment.vmaddr &&
                   section.addr-segment.vmaddr<=segment.vmsize && section.size<=segment.vmsize-(section.addr-segment.vmaddr) &&
                   loaded>=preferred && section.addr<=UINT64_MAX-(loaded-preferred))
                    return section.addr+loaded-preferred;
            }
        }
        at+=current.cmdsize;
    }
    return 0;
}
static NSDictionary *keyboard_native_counters(task_t task, uint64_t address) {
    DitingKeyboardNativeDiagnostic snapshot{};
    uint32_t after[2]{};
    for(unsigned attempt=0;attempt<3;attempt++) {
        if(!keyboard_read(task,address,&snapshot,sizeof(snapshot)) ||
           !keyboard_read(task,address+offsetof(DitingKeyboardNativeDiagnostic,writers),after,sizeof(after)))break;
        if(snapshot.magic!=DitingKeyboardNativeMagic || snapshot.version!=1 || snapshot.bytes!=sizeof(snapshot) ||
           snapshot.methods!=DitingKeyboardNativeMethods)break;
        if(snapshot.writers || after[0] || snapshot.epoch!=after[1] || snapshot.epoch&1)continue;
        NSMutableArray *calls=[NSMutableArray new];
        for(unsigned index=0;index<DitingKeyboardNativeMethods;index++) {
            const auto &c=snapshot.calls[index];
            // Schema 1 columns: method, calls, total_ns, max_ns, last_start_ns,
            // last_end_ns. Compact rows leave room for real pixel frames.
            if(c.calls)[calls addObject:@[@(index),@(c.calls),@(c.total_ns),@(c.max_ns),@(c.last_start_ns),@(c.last_end_ns)]];
        }
        return @{@"valid":@YES,@"version":@(snapshot.version),@"bytes":@(snapshot.bytes),@"epoch":@(snapshot.epoch),@"calls":calls};
    }
    return @{@"valid":@NO,@"scope":@"native diagnostic unavailable or updating"};
}
static NSDictionary *keyboard_thread(task_t task, thread_t thread, bool first) {
    thread_basic_info_data_t basic{};
    mach_msg_type_number_t words = THREAD_BASIC_INFO_COUNT;
    const kern_return_t info = thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&basic, &words);
    thread_identifier_info_data_t identifier{};
    words = THREAD_IDENTIFIER_INFO_COUNT;
    const kern_return_t named = thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&identifier, &words);
    arm_thread_state64_t state{};
    words = ARM_THREAD_STATE64_COUNT;
    const kern_return_t result = thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &words);
    NSMutableArray *frames = [NSMutableArray new];
    if (!result) {
        const uint64_t pc = arm_thread_state64_get_pc(state), lr = arm_thread_state64_get_lr(state);
        [frames addObject:keyboard_hex(pc)];
        [frames addObject:keyboard_hex(lr)];
        uint64_t fp = arm_thread_state64_get_fp(state), sp = arm_thread_state64_get_sp(state);
        for (unsigned j = 0; j < (first ? 10u : 8u) && fp >= sp && fp - sp < 8 * 1024 * 1024 && !(fp & 15); ++j) {
            uint64_t pair[2]{};
            if (!keyboard_read(task, fp, pair, sizeof(pair))) break;
            [frames addObject:keyboard_hex(pair[1])];
            if (pair[0] <= fp) break;
            fp = pair[0];
        }
    }
    const uint64_t user = uint64_t(basic.user_time.seconds) * 1000000 + basic.user_time.microseconds;
    const uint64_t system = uint64_t(basic.system_time.seconds) * 1000000 + basic.system_time.microseconds;
    return @{@"thread_id":@(named ? 0 : identifier.thread_id), @"first_thread":@(first),
        @"info_error":@(info), @"state_error":@(result), @"run_state":@(basic.run_state),
        @"cpu_usage":@(basic.cpu_usage), @"user_us":@(user), @"system_us":@(system), @"frames":frames};
}
static NSArray *keyboard_image_metadata(task_t task, int pid, const dyld_all_image_infos &images) {
    static NSMutableDictionary *cached = [NSMutableDictionary new];
    NSDictionary *old = cached[@(pid)];
    if ([old[@"count"] unsignedIntValue] == images.infoArrayCount) return old[@"images"] ?: @[];
    NSMutableArray *rows = [NSMutableArray new];
    if (images.infoArrayCount <= 2048 && images.sharedCacheBaseAddress) {
        for (unsigned first = 0; first < images.infoArrayCount && rows.count < 24; first += 32) {
            dyld_image_info entries[32]{};
            const unsigned count = std::min(32u, images.infoArrayCount - first);
            if (!keyboard_read(task, uint64_t(images.infoArray) + size_t(first) * sizeof(entries[0]), entries, count * sizeof(entries[0]))) break;
            for (unsigned index = 0; index < count && rows.count < 24; ++index) {
                const uint64_t loaded = uint64_t(entries[index].imageLoadAddress);
                const uint64_t cache = uint64_t(images.sharedCacheBaseAddress);
                if (first + index && loaded >= cache && loaded - cache < (8ull << 30)) continue;
                mach_header_64 header{};
                if (!keyboard_read(task, loaded, &header, sizeof(header)) || header.magic != MH_MAGIC_64 ||
                    header.ncmds > 128 || header.sizeofcmds > 65536) continue;
                uint64_t preferred = 0, text_size = 0, at = loaded + sizeof(header);
                for (unsigned command = 0; command < header.ncmds; ++command) {
                    load_command current{};
                    if (!keyboard_read(task, at, &current, sizeof(current)) || current.cmdsize < sizeof(current) ||
                        at + current.cmdsize > loaded + sizeof(header) + header.sizeofcmds) break;
                    if (current.cmd == LC_SEGMENT_64 && current.cmdsize >= sizeof(segment_command_64)) {
                        segment_command_64 segment{};
                        if (keyboard_read(task, at, &segment, sizeof(segment)) && !strncmp(segment.segname, "__TEXT", 16)) {
                            preferred = segment.vmaddr; text_size = segment.vmsize; break;
                        }
                    }
                    at += current.cmdsize;
                }
                char path[256]{};
                if (!keyboard_read(task, uint64_t(entries[index].imageFilePath), path, sizeof(path))) continue;
                path[sizeof(path) - 1] = 0;
                if (text_size) {
                    NSMutableDictionary *row=[@{@"path":@(path),@"load_address":keyboard_hex(loaded),
                        @"text_vmaddr":keyboard_hex(preferred),@"text_bytes":@(text_size)} mutableCopy];
                    if(!strcmp(path,"/diting/libditing-ui18.dylib")) {
                        const uint64_t native=keyboard_native_section(task,loaded,header,preferred);
                        if(native)row[@"keyboard_native_address"]=keyboard_hex(native);
                    }
                    [rows addObject:row];
                }
            }
        }
    }
    cached[@(pid)] = @{@"count":@(images.infoArrayCount), @"images":rows};
    return rows;
}
static NSDictionary *keyboard_process(int pid, const char *name) {
    task_t task = MACH_PORT_NULL;
    // Darwin24 refuses foreign control ports while Developer Mode is off.
    // A read port supplies real thread states and memory without task control.
    using ReadTask = kern_return_t (*)(mach_port_name_t, int, mach_port_name_t *);
    const auto read_task = reinterpret_cast<ReadTask>(dlsym(RTLD_DEFAULT, "task_read_for_pid"));
    const kern_return_t access = read_task ? read_task(mach_task_self(), pid, &task) : KERN_NOT_SUPPORTED;
    if (access || !MACH_PORT_VALID(task)) return @{@"pid":@(pid), @"name":@(name),
        @"task_error":@(access ? access : KERN_INVALID_CAPABILITY), @"task_port_flavor":@"read"};
    task_dyld_info_data_t dyld{};
    mach_msg_type_number_t words = TASK_DYLD_INFO_COUNT;
    const kern_return_t dyld_error = task_info(task, TASK_DYLD_INFO, (task_info_t)&dyld, &words);
    struct dyld_all_image_infos images{};
    bool image_read = !dyld_error && sizeof(images) <= 1024 &&
        keyboard_read(task, dyld.all_image_info_addr, &images, sizeof(images));
    NSArray *image_metadata = image_read ? keyboard_image_metadata(task, pid, images) : @[];
    NSDictionary *native=@{};
    for(NSDictionary *image in image_metadata) {
        NSString *address=image[@"keyboard_native_address"];
        if(address)native=keyboard_native_counters(task,strtoull(address.UTF8String,nullptr,16));
    }
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    const kern_return_t result = task_threads(task, &threads, &count);
    NSMutableArray *rows = [NSMutableArray new];
    if (!result && count) {
        unsigned busy = 0; int busiest = -1;
        for (unsigned i = 1; i < count && i < 64; ++i) {
            thread_basic_info_data_t info{};
            words = THREAD_BASIC_INFO_COUNT;
            if (!thread_info(threads[i], THREAD_BASIC_INFO, (thread_info_t)&info, &words) && info.cpu_usage > busiest) {
                busiest = info.cpu_usage; busy = i;
            }
        }
        [rows addObject:keyboard_thread(task, threads[0], true)];
        if (!strcmp(name, "InputUI") || !strcmp(name, "kbd")) {
            for (unsigned i = 1; i < count && i < 32; ++i) [rows addObject:keyboard_thread(task, threads[i], false)];
        } else if (busy && busiest > 0) [rows addObject:keyboard_thread(task, threads[busy], false)];
    }
    if (!result) {
        for (unsigned i = 0; i < count; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(*threads));
    }
    mach_port_deallocate(mach_task_self(), task);
    return @{@"pid":@(pid), @"name":@(name), @"task_error":@(access), @"task_port_flavor":@"read", @"threads_error":@(result),
        @"thread_count":@(count), @"dyld_info_error":@(dyld_error), @"dyld_image_read":@(image_read),
        @"shared_cache_slide":image_read ? keyboard_hex(images.sharedCacheSlide) : @"unavailable",
        @"image_metadata":image_metadata, @"native_keyboard_counters":native, @"threads":rows};
}
struct KeyboardSampler {
    pthread_t thread{};
    std::atomic<bool> stop{false};
    std::atomic<unsigned> phase{0};
    NSMutableArray *samples = [NSMutableArray new];
    int error = 0; bool started = false;
    static void *run(void *context) {
        auto &self = *static_cast<KeyboardSampler *>(context);
        @autoreleasepool {
            try {
                UITimingPolicy timing; timing.acquire();
                for (unsigned sample = 0; sample < 48 && !self.stop.load(); ++sample) {
                    const uint64_t at = now_ns();
                    std::vector<kinfo_proc> pids(512);
                    const int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
                    size_t bytes = pids.size() * sizeof(kinfo_proc);
                    const int listed = sysctl(const_cast<int *>(mib), 4, pids.data(), &bytes, nullptr, 0);
                    const int list_error = listed ? errno : 0;
                    NSMutableArray *processes = [NSMutableArray new];
                    if (!listed && bytes <= pids.size() * sizeof(kinfo_proc) && !(bytes % sizeof(kinfo_proc))) {
                        for (size_t index = 0; index < bytes / sizeof(kinfo_proc); ++index) {
                            const char *name = pids[index].kp_proc.p_comm;
                            if (!pids[index].kp_proc.p_pid || !memchr(name, 0, sizeof(pids[index].kp_proc.p_comm))) continue;
                            if (!strcmp(name, "InputUI") || !strcmp(name, "kbd") ||
                                !strcmp(name, "SpringBoard") || !strcmp(name, "backboardd") ||
                                !strcmp(name, "Spotlight") || !strcmp(name, "Preferences"))
                                [processes addObject:keyboard_process(pids[index].kp_proc.p_pid, name)];
                        }
                    }
                    [self.samples addObject:@{@"at_ns":@(at), @"end_ns":@(now_ns()),
                        @"phase":@(self.phase.load()), @"pid_bytes":@(bytes), @"pid_error":@(list_error), @"processes":processes}];
                    for (unsigned i = 0; i < 40 && !self.stop.load(); ++i) wait_until_ns(now_ns() + 10000000);
                }
            } catch (...) { self.error = EIO; }
        }
        return nullptr;
    }
    void begin() {
        const int result = pthread_create(&thread, nullptr, &run, this);
        if (result) throw Failure{"keyboard-sampler-create", result};
        started = true;
    }
    void finish() {
        if (!started) return;
        stop.store(true);
        const int result = pthread_join(thread, nullptr);
        started = false;
        if (result) error = result;
    }
    ~KeyboardSampler() { finish(); }
};
static NSDictionary *keyboard_resource_info() {
    NSMutableArray *files = [NSMutableArray new];
    for (const char *path : {
        "/System/Library/PrivateFrameworks/TextInputUI.framework/KBLayouts_iPhone.dat",
        "/System/Library/PrivateFrameworks/TextInputUI.framework/KBLayouts_iPad.dat",
        "/Applications/InputUI.app/InputUI", "/System/Library/TextInput/kbd",
        "/private/var/mobile/Library/Caches/com.apple.keyboards/images"}) {
        struct stat st{};
        const int result = lstat(path, &st);
        const int error = result ? errno : 0;
        [files addObject:@{@"path":@(path), @"error":@(error), @"bytes":@(uint64_t(st.st_size)),
            @"mode":@(st.st_mode), @"uid":@(st.st_uid), @"gid":@(st.st_gid)}];
    }
    return @{@"files":files, @"read_only":@YES};
}
static NSDictionary *keyboard_pixels(UI &ui) {
    const auto *source = reinterpret_cast<const volatile uint32_t *>(ui.framebuffer);
    unsigned width=256,height=576;
    std::vector<uint32_t> pixels;
    std::vector<uint8_t> packed;
    uLongf bytes=0;
    for(;;) {
        pixels.resize(width*height);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
            pixels[y*width+x]=source[size_t(y*(ui.height-1)/(height-1))*(ui.stride/4)+x*(ui.width-1)/(width-1)];
        packed.resize(compressBound(pixels.size()*sizeof(uint32_t)));bytes=packed.size();
        if(compress2(packed.data(),&bytes,reinterpret_cast<const Bytef *>(pixels.data()),pixels.size()*sizeof(uint32_t),Z_BEST_SPEED)!=Z_OK)
            throw Failure{"keyboard-frame-compression",EIO};
        if(bytes<=65536)break;
        if(width<=64)throw Failure{"keyboard-frame-size",EOVERFLOW};
        width/=2;height/=2;
    }
    NSData *data = [NSData dataWithBytes:packed.data() length:bytes];
    return @{@"width":@(width), @"height":@(height), @"format":@"BGRA8-zlib", @"raw_bytes":@(pixels.size()*sizeof(uint32_t)), @"source":@"pre-DPU non-atomic framebuffer",
        @"bytes_base64":[data base64EncodedStringWithOptions:0]};
}
static void keyboard_diagnosis(UI &ui) {
    testcase(64, "keyboard-show-typing-diagnosis", "keyboard", [&] {
        using CsOps = int (*)(pid_t, unsigned, void *, size_t);
        const auto csops = reinterpret_cast<CsOps>(dlsym(RTLD_DEFAULT, "csops"));
        uint32_t signature_flags = 0;
        if (!csops || csops(getpid(), 0, &signature_flags, sizeof(signature_flags)) || !(signature_flags & 0x04000000))
            throw Failure{"keyboard-diagnostic-platform-signature", EPERM};
        const unsigned accepted = ui.accepted, refused = ui.refused;
        KeyboardSampler sampler;
        sampler.begin();
        NSMutableArray *actions = [NSMutableArray new];
        // Compare two Spotlight searches with a native Preferences search.
        // Do not submit a search, select a result or edit any preference.
        for (unsigned cycle = 0; cycle < 3; ++cycle) {
            sampler.phase.store(1 + cycle * 3);
            ui.home(); ui.wait(500000);
            ui.begin_timeline();
            NSMutableArray *hashes = [NSMutableArray new];
            const uint64_t show_start = now_ns();
            if (cycle < 2) ui.swipe(0.5, 0.30, 0.5, 0.65, hashes);
            else {
                if (!ui.launch || !ui.frontmost) throw Failure{"keyboard-preferences-launch-unavailable", ENXIO};
                const int launched = ui.launch(CFSTR("com.apple.Preferences"), false);
                const uint64_t until = now_ns() + 3000000000ull;
                while (![ui.foreground() isEqualToString:@"com.apple.Preferences"] && now_ns() < until) ui.wait(100000);
                if (launched || ![ui.foreground() isEqualToString:@"com.apple.Preferences"])
                    throw Failure{"keyboard-preferences-foreground", launched ? launched : ENXIO};
                ui.wait(3000000);
                ui.swipe(0.5, 0.25, 0.5, 0.80, hashes);
                ui.swipe(0.5, 0.25, 0.5, 0.80, hashes);
                ui.contact(0.35, 0.205, true, false, false); ui.wait(30000);
                ui.contact(0.35, 0.205, false, false, false);
            }
            sampler.phase.store(2 + cycle * 3);
            ui.wait(1500000);
            [actions addObject:@{@"action":cycle < 2 ? @"spotlight-show" : @"preferences-search-show", @"cycle":@(cycle),
                @"start_ns":@(show_start), @"end_ns":@(now_ns()), @"content_hash":keyboard_hex(ui.snapshot()),
                @"frame":keyboard_pixels(ui)}];
            sampler.phase.store(3 + cycle * 3);
            for (unsigned key = 0; key < 8; ++key) {
                const double x = 0.25 + 0.10 * (key % 4);
                const uint64_t at = now_ns();
                ui.contact(x, 0.745, true, false, false); ui.wait(30000);
                ui.contact(x, 0.745, false, false, false); ui.wait(150000);
                [actions addObject:@{@"action":@"letter-touch", @"cycle":@(cycle), @"index":@(key),
                    @"start_ns":@(at), @"end_ns":@(now_ns()), @"content_hash":keyboard_hex(ui.snapshot())}];
            }
            [actions addObject:@{@"action":@"typing-end", @"cycle":@(cycle), @"at_ns":@(now_ns()), @"frame":keyboard_pixels(ui)}];
        }
        sampler.phase.store(10);
        ui.home(); ui.wait(500000);
        sampler.finish();
        bool input_ui_seen = false, input_ui_read = false;
        for (NSDictionary *sample in sampler.samples) for (NSDictionary *process in sample[@"processes"]) {
            if ([process[@"name"] isEqualToString:@"InputUI"]) {
                input_ui_seen = true;
                for (NSDictionary *thread in process[@"threads"])
                    if (![process[@"task_error"] unsignedIntValue] && ![thread[@"state_error"] unsignedIntValue] &&
                        [thread[@"frames"] count] >= 2) input_ui_read = true;
            }
        }
        return @{@"pass":@(ui.refused == refused && ui.accepted > accepted && input_ui_seen && input_ui_read && !sampler.error),
            @"accepted_events":@(ui.accepted - accepted), @"refused_events":@(ui.refused - refused),
            @"input_ui_seen":@(input_ui_seen), @"input_ui_stacks_read":@(input_ui_read), @"signature_flags":@(signature_flags),
            @"sampler_error":@(sampler.error), @"resources":keyboard_resource_info(),
            @"actions":actions, @"samples":sampler.samples, @"physical_present":@"UNQUALIFIED",
            @"scope":@"acknowledged HID and bounded process stacks; content hashes do not prove text delivery or keyboard latency"};
    });
}

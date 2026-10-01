// EXTREME-tier signature bypass: redirect a raw, hand-written `svc #0` syscall instead of
// requiring the app to go through a libc symbol every other sigbypass layer can hook.
//
// Every layer below this one -- hookJavaIO/hookJavaFilePathAccessors (Java), the inline libc
// hooks (openat/open/stat/...), and the xhook GOT/PLT rewrite (Stealth) -- only sees a call to
// *openat* if the app actually calls the libc function named `openat`. A packer or anti-tamper
// routine that instead emits `svc #0` directly (syscall number in x8, args in x0-x5, AArch64
// Linux ABI) with its own inline assembly bypasses all of them in one step: there is no libc
// call, no GOT entry, nothing to intercept before the instruction traps into the kernel.
//
// This mirrors what upstream JingMatrix/LSPatch calls its signature-bypass Level 3: scan the
// executable segments of the images the *app itself* loaded for the `svc #0` encoding, and use
// Dobby's instruction instrumentation (already a transitive dependency of this target via
// core/native -> dobby_static, so this adds no new third-party library) to run a callback
// immediately before each one, with full access to the pending syscall's registers. If it's
// openat/openat2 reading the APK path we care about, the callback rewrites the pathname register
// before the instruction executes.
//
// Two scope rules keep this from touching code it must not. (1) Only images under /data (the
// app's own .so, a packer's decrypted secondary .so) are scanned -- never libc/libart/the
// linker/app_process, which hold only the normal syscall thunks (futex, read, poll, nanosleep).
// (2) Within those, a site is instrumented only when the nearest preceding `MOVZ x8,#nr` proves
// it issues openat/openat2. Both exist because the danger is not handle_svc (it already ignores
// non-openat at runtime) but Dobby's trampoline wrapping a hot, blocking, signal-restartable
// syscall: a thread parked in futex that traps through it returns with a corrupted register
// context and SIGSEGVs. JM's Level 3 scopes to "the app's own code" for the same reason.
//
// This does NOT replace the openat/GOT hooks: those remain the broad-coverage path for the
// overwhelming majority of real-world code, which does call into libc. This is narrowly scoped to
// the specific code a packer deliberately hand-wrote to dodge hooking -- and by the same token,
// since it is new code appended at a handful of specific addresses, a self-checksumming packer
// can in principle notice the patched bytes. Opt-in, arm64-only, matching JM's own tradeoff.
#include "svc_bypass.h"

#include "common/logging.h"
#include "native_util.h"
#include "utils/jni_helper.hpp"

#include "dobby.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <limits.h>
#include <link.h>
#include <mutex>
#include <sys/syscall.h>
#include <vector>

namespace lspd {

#if defined(__aarch64__)

    namespace {

        // AArch64 encoding of "svc #0" -- the only immediate Linux's syscall ABI ever uses; iOS'
        // Dobby sample plugin (core/external/dobby/builtin-plugin/SupervisorCallMonitor) looks for
        // "svc #0x80" instead (0xd4001001), which is the Darwin convention and never appears in
        // code generated for Android, so that plugin's constant would never match anything here.
        constexpr uint32_t kSvcInstruction = 0xd4000001u;

        // Matches `MOVZ (w8|x8), #imm16` with no shift: fixed opcode bits [30:23]=0xA5 and hw=0
        // and Rd=8, leaving sf([31]) and imm16([20:5]) free. Both a hand-written anti-tamper stub
        // and every libc syscall thunk load the syscall number into x8 with exactly this
        // instruction right before `svc #0`, so the imm16 it carries is the syscall number.
        constexpr uint32_t kMovzX8Mask = 0x7FE0001Fu;
        constexpr uint32_t kMovzX8Value = 0x52800008u;

        std::mutex g_svc_mutex;
        char g_target_path[PATH_MAX] = {0};
        char g_redirect_path[PATH_MAX] = {0};
        std::atomic<bool> g_svc_armed{false};

        // Addresses DobbyInstrument has already been called on, so a rescan triggered by a later
        // dlopen never re-instruments a site a previous scan already covered.
        std::vector<uintptr_t> g_patched_addrs;

        bool is_already_patched_locked(uintptr_t addr) {
            for (uintptr_t patched : g_patched_addrs) {
                if (patched == addr) return true;
            }
            return false;
        }

        // Same "exact path, or the same path plus a ' (deleted)' suffix" comparison bypass_sig.cpp
        // and seccompv.cpp each already carry their own copy of, for a once-renamed/unlinked APK.
        bool path_matches_target(const char* pathname) {
            if (pathname == nullptr || g_target_path[0] == '\0') {
                return false;
            }
            if (strcmp(pathname, g_target_path) == 0) {
                return true;
            }
            size_t target_len = strlen(g_target_path);
            return strncmp(pathname, g_target_path, target_len) == 0
                   && strcmp(pathname + target_len, " (deleted)") == 0;
        }

        // Runs on whatever thread executes a patched svc instruction. Only openat/openat2 sites
        // are ever instrumented (see svc_site_targets_openat), so in practice nr is already one
        // of those; the runtime check is kept as cheap defense in case a gated site ever issues
        // something else. Must stay allocation-free and lock-free: g_target_path/g_redirect_path
        // are fixed-size buffers
        // written once (under g_svc_mutex) before any instrumentation is installed and never
        // mutated again for the lifetime of the process, so reading them here without the lock is
        // the same accepted pattern seccompv.cpp's sigsys_handler already relies on.
        void handle_svc(void* /*address*/, DobbyRegisterContext* ctx) {
            long nr = static_cast<long>(ctx->general.regs.x8);
            if (nr != __NR_openat
#if defined(__NR_openat2)
                && nr != __NR_openat2
#endif
            ) {
                return;
            }
            auto* pathname = reinterpret_cast<const char*>(ctx->general.regs.x1);
            if (!path_matches_target(pathname)) {
                return;
            }
            ctx->general.regs.x1 = reinterpret_cast<uint64_t>(g_redirect_path);
        }

        void copy_path(char* dest, const char* src) {
            if (src == nullptr) {
                dest[0] = '\0';
                return;
            }
            strncpy(dest, src, PATH_MAX - 1);
            dest[PATH_MAX - 1] = '\0';
        }

        // True only when the nearest `MOVZ x8,#nr` preceding this svc loads openat/openat2. The
        // crash this guards is not in handle_svc -- it is Dobby's trampoline wrapping a hot,
        // blocking, signal-restartable syscall (futex), which returns with a corrupted register
        // context and SIGSEGVs. So a futex/read/mmap site must never be instrumented in the first
        // place. Scanning back to the nearest x8 load distinguishes an openat stub from a futex
        // stub even when they sit four instructions apart, because each has its own MOVZ x8.
        bool svc_site_targets_openat(uintptr_t svc_addr, uintptr_t seg_start) {
            constexpr int kMaxLookback = 16; // instructions
            for (int k = 1; k <= kMaxLookback; ++k) {
                uintptr_t probe = svc_addr - static_cast<uintptr_t>(k) * sizeof(uint32_t);
                if (probe < seg_start) break;
                uint32_t insn = *reinterpret_cast<const uint32_t*>(probe);
                if ((insn & kMovzX8Mask) != kMovzX8Value) continue; // not a MOVZ into x8
                uint32_t imm = (insn >> 5) & 0xFFFFu;               // the syscall number it loads
                return imm == static_cast<uint32_t>(__NR_openat)
#if defined(__NR_openat2)
                       || imm == static_cast<uint32_t>(__NR_openat2)
#endif
                        ;
            }
            return false;
        }

        // Scans one loaded image's readable+executable PT_LOAD segments for "svc #0" and arms
        // Dobby on every occurrence not already instrumented.
        //
        // Deliberately bounded by p_filesz, not p_memsz, and gated on PF_R as well as PF_X: this
        // project already hit a SIGSEGV once (fixed in bc8afc8a2) from a similar scan that walked
        // /proc/self/maps by p_memsz and read into a PROT_NONE guard gap between segments. p_memsz
        // can run past the file-backed part of a segment into zero-filled pages this segment does
        // not actually own, and some hardened libraries on newer Android ship execute-only (PF_X
        // without PF_R) text pages -- reading either crashes the host app, not just this scan.
        void scan_image_for_svc(const dl_phdr_info* info) {
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
                if (phdr.p_type != PT_LOAD) continue;
                if ((phdr.p_flags & PF_X) == 0 || (phdr.p_flags & PF_R) == 0) continue;

                auto seg_start = static_cast<uintptr_t>(info->dlpi_addr + phdr.p_vaddr);
                uintptr_t seg_end = seg_start + phdr.p_filesz;
                // AArch64 instructions are always 4-byte aligned; p_vaddr for an executable
                // segment already is too, but round up defensively rather than assume it.
                seg_start = (seg_start + 3u) & ~static_cast<uintptr_t>(3u);

                for (uintptr_t addr = seg_start; addr + sizeof(uint32_t) <= seg_end; addr += sizeof(uint32_t)) {
                    if (*reinterpret_cast<const uint32_t*>(addr) != kSvcInstruction) continue;
                    // Scope gate: instrument only sites we can prove issue openat/openat2, never
                    // futex/read/mmap/nanosleep, whose trampoline wrapping is what crashed.
                    if (!svc_site_targets_openat(addr, seg_start)) continue;

                    std::scoped_lock lock(g_svc_mutex);
                    if (is_already_patched_locked(addr)) continue;
                    if (DobbyInstrument(reinterpret_cast<void*>(addr), handle_svc) == 0) {
                        g_patched_addrs.push_back(addr);
                    } else {
                        LOGW("SvcBypass: DobbyInstrument failed at {:p}", reinterpret_cast<void*>(addr));
                    }
                }
            }
        }

        // Only images the app itself loaded (its own .so, a packer's decrypted secondary .so, all
        // under /data or adopted storage) can hold the hand-written `svc #0` this layer exists to
        // catch. libc/libart/the linker/app_process -- every system or runtime image, and the
        // nameless main executable mapping -- carry only the normal syscall thunks (futex, read,
        // poll, nanosleep); instrumenting those is exactly what crashed every thread parked in
        // futex. Gate on path so they are never scanned.
        bool is_app_owned_image(const char* name) {
            if (name == nullptr || name[0] == '\0') return false; // app_process / anonymous mapping
            if (name[0] == '[') return false;                     // [vdso], [anon:...]
            return strncmp(name, "/data/", 6) == 0
                   || strncmp(name, "/mnt/expand/", 12) == 0;      // app installed to adopted storage
        }

        int dl_iterate_callback(dl_phdr_info* info, size_t /*size*/, void* /*data*/) {
            const char* name = info->dlpi_name;
            if (!is_app_owned_image(name)) {
                return 0; // never instrument system/runtime images -- see is_app_owned_image
            }
            if (strstr(name, "libnpatch") != nullptr) {
                return 0; // our own injected library lives under /data too; never instrument it
            }
            scan_image_for_svc(info);
            return 0;
        }

        void scan_all_loaded_images() {
            dl_iterate_phdr(dl_iterate_callback, nullptr);
        }

        using DlopenFn = void* (*)(const char*, int);
        using DlopenExtFn = void* (*)(const char*, int, const void*);

        bool g_dlopen_hooks_installed = false;
        DlopenFn g_dlopen_backup = nullptr;
        DlopenExtFn g_android_dlopen_ext_backup = nullptr;

        // A packer that decrypts and dlopens a fresh SO after startup would otherwise never get
        // scanned at all: the one-time scan_all_loaded_images() call in enable_svc_redirect_impl
        // only sees what's already loaded at that point. Re-scanning everything on every dlopen
        // is the same "accept the cost, it's opt-in and infrequent compared to the syscalls it
        // guards" tradeoff the Stealth GOT hook's own xhook_refresh already makes.
        void* hooked_dlopen(const char* filename, int flags) {
            void* handle = g_dlopen_backup != nullptr ? g_dlopen_backup(filename, flags) : nullptr;
            if (handle != nullptr && g_svc_armed.load(std::memory_order_acquire)) {
                scan_all_loaded_images();
            }
            return handle;
        }

        void* hooked_android_dlopen_ext(const char* filename, int flags, const void* extinfo) {
            void* handle = g_android_dlopen_ext_backup != nullptr
                    ? g_android_dlopen_ext_backup(filename, flags, extinfo)
                    : nullptr;
            if (handle != nullptr && g_svc_armed.load(std::memory_order_acquire)) {
                scan_all_loaded_images();
            }
            return handle;
        }

        void install_dlopen_hooks_once() {
            if (g_dlopen_hooks_installed) return;
            g_dlopen_hooks_installed = true;

            void* dlopen_symbol = dlsym(RTLD_DEFAULT, "dlopen");
            if (dlopen_symbol == nullptr) {
                LOGW("SvcBypass: dlopen symbol not found, incremental rescan disabled");
            } else if (HookInline(dlopen_symbol, reinterpret_cast<void*>(hooked_dlopen),
                                  reinterpret_cast<void**>(&g_dlopen_backup)) != 0) {
                LOGW("SvcBypass: failed to hook dlopen");
            }

            void* dlopen_ext_symbol = dlsym(RTLD_DEFAULT, "android_dlopen_ext");
            if (dlopen_ext_symbol == nullptr) {
                LOGW("SvcBypass: android_dlopen_ext symbol not found");
            } else if (HookInline(dlopen_ext_symbol, reinterpret_cast<void*>(hooked_android_dlopen_ext),
                                  reinterpret_cast<void**>(&g_android_dlopen_ext_backup)) != 0) {
                LOGW("SvcBypass: failed to hook android_dlopen_ext");
            }
        }

        bool enable_svc_redirect_impl(JNIEnv* env, jstring jTargetPath, jstring jRedirectPath,
                                      jstring jPkgName) {
            if (jTargetPath == nullptr || jRedirectPath == nullptr) {
                LOGW("SvcBypass: redirect paths cannot be null");
                return false;
            }

            lsplant::JUTFString target(env, jTargetPath);
            lsplant::JUTFString redirect(env, jRedirectPath);
            {
                std::scoped_lock lock(g_svc_mutex);
                copy_path(g_target_path, target.get());
                copy_path(g_redirect_path, redirect.get());
            }
            (void) jPkgName;

            install_dlopen_hooks_once();
            scan_all_loaded_images();
            g_svc_armed.store(true, std::memory_order_release);
            LOGI("SvcBypass: armed, {} -> {}", g_target_path, g_redirect_path);
            return true;
        }

    } // namespace

#endif // __aarch64__

    LSP_DEF_NATIVE_METHOD(jboolean, SigBypass, enableSvcRedirect,
                          jstring jTargetPath, jstring jRedirectPath, jstring jPkgName) {
#if defined(__aarch64__)
        return enable_svc_redirect_impl(env, jTargetPath, jRedirectPath, jPkgName)
               ? JNI_TRUE : JNI_FALSE;
#else
        (void) jTargetPath;
        (void) jRedirectPath;
        (void) jPkgName;
        LOGI("SvcBypass: skipped on non-arm64 architecture");
        return JNI_FALSE;
#endif
    }

    static JNINativeMethod gMethods[] = {
            LSP_NATIVE_METHOD(SigBypass, enableSvcRedirect,
                              "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z"),
    };

    void RegisterSvcBypass(JNIEnv* env) {
        REGISTER_LSP_NATIVE_METHODS(SigBypass);
    }

} // namespace lspd

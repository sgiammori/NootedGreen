/*
 * Compiler plugin shim for the 2020 Tiger Lake Metal compiler plugin (libMTLIGCCompilerPlugin.dylib).
 *
 * Installed as AppleIntelGraphicsShared.bundle/Contents/MacOS/libMTLIGCCompilerPlugin.dylib in
 * /Library/GPUBundles, with the 2020 plugin beside it as libMTLIGCCompilerPlugin.dylib.real.
 *
 * macOS 15's MTLCompiler links LLVM 32023; the 2020 plugin and its libLLVM are LLVM 3902. The plugin
 * only exports MTLCompilerBuildRequestWithOptions, which receives the host's llvm::Module. This shim adds
 * MTLCompilerBuildRequestWithSerializedBitcode, so the host serializes the module (NootedGreen makes it
 * write bitcode version 4, LLVM 3902's) and the plugin's own libLLVM parses it. The shim also replaces the
 * host's compiler helper, so the builtin modules the plugin links come from its own 2020 runtime .split
 * files, parsed by the same libLLVM, instead of from the host's LLVM.
 *
 * .split file: u32 count, count x { char name[]; '\0'; u32 offset (from file + 4) }, then at
 * file + 4 + offset a bitcode wrapper { u32 0x0B17C0DE, u32 version, u32 offset, u32 size, u32 cputype }.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define DONOR_PATH \
    "/Library/GPUBundles/AppleIntelGraphicsShared.bundle/Contents/MacOS/libMTLIGCCompilerPlugin.dylib.real"
#define LLVM_PATH "/Library/GPUBundles/libLLVM.dylib"

#define NGLOG(fmt, ...) syslog(LOG_ERR, "NGreen IGC shim: " fmt, ##__VA_ARGS__)

typedef void *LLVMContextRef, *LLVMModuleRef, *LLVMMemoryBufferRef, *LLVMValueRef;
typedef int LLVMBool;

static LLVMContextRef (*L_ContextCreate)(void);
static LLVMContextRef (*L_GetModuleContext)(LLVMModuleRef);
static LLVMModuleRef (*L_ModuleCreateInCtx)(const char *, LLVMContextRef);
static LLVMMemoryBufferRef (*L_MemBufRange)(const char *, size_t, const char *, LLVMBool);
static void (*L_DisposeMemBuf)(LLVMMemoryBufferRef);
static LLVMBool (*L_ParseBitcodeInCtx2)(LLVMContextRef, LLVMMemoryBufferRef, LLVMModuleRef *);
static LLVMBool (*L_LinkModules2)(LLVMModuleRef, LLVMModuleRef);
static LLVMValueRef (*L_FirstFunction)(LLVMModuleRef);
static LLVMValueRef (*L_NextFunction)(LLVMValueRef);
static LLVMValueRef (*L_FirstGlobal)(LLVMModuleRef);
static LLVMValueRef (*L_NextGlobal)(LLVMValueRef);
static LLVMBool (*L_IsDeclaration)(LLVMValueRef);
static const char *(*L_GetValueName)(LLVMValueRef);

static void *(*d_Create)(void *);
static void (*d_Delete)(void *);
static void (*d_SetHelper)(void *, void *);
static void (*d_ReleaseReply)(void *);
/* rdi, rsi, rdx, ecx, r8=llvm::Module*, r9=out, then 6 stack slots */
static int (*d_WithOptions)(void *, const void *, unsigned long, unsigned, void *, void *, void *,
                            void *, void *, void *, void *, void *);

static int g_ok = 0;

typedef struct {
    const char *name;
    uint32_t offset; /* relative to base+4 */
} NGSplitEntry;

typedef struct {
    const uint8_t *base;
    size_t size;
    NGSplitEntry *entries;
    uint32_t count;
} NGSplit;

static uint32_t ng_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static NGSplit *ng_split_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        NGLOG("split open FAILED (errno %d): %s", errno, path);
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 8) {
        close(fd);
        return NULL;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        NGLOG("split mmap FAILED: %s", path);
        return NULL;
    }

    NGSplit *sp = calloc(1, sizeof(*sp));
    if (!sp) {
        munmap(m, (size_t)st.st_size);
        return NULL;
    }
    sp->base = (const uint8_t *)m;
    sp->size = (size_t)st.st_size;
    sp->count = ng_rd32(sp->base);
    if (sp->count == 0 || sp->count > 1000000u) {
        NGLOG("split count implausible (%u): %s", sp->count, path);
        munmap(m, sp->size);
        free(sp);
        return NULL;
    }
    sp->entries = calloc(sp->count, sizeof(NGSplitEntry));
    if (!sp->entries) {
        munmap(m, sp->size);
        free(sp);
        return NULL;
    }

    size_t off = 4;
    for (uint32_t i = 0; i < sp->count; i++) {
        size_t start = off;
        while (off < sp->size && sp->base[off] != 0) off++;
        if (off + 5 > sp->size) { /* need NUL + u32 */
            NGLOG("split directory truncated at entry %u: %s", i, path);
            munmap(m, sp->size);
            free(sp->entries);
            free(sp);
            return NULL;
        }
        sp->entries[i].name = (const char *)(sp->base + start);
        off++; /* NUL */
        sp->entries[i].offset = ng_rd32(sp->base + off);
        off += 4;
    }
    return sp;
}

static int ng_split_find(const NGSplit *sp, const char *name, const uint8_t **out, size_t *outLen) {
    if (!sp || !name) return 0;
    for (uint32_t i = 0; i < sp->count; i++) {
        if (strcmp(sp->entries[i].name, name) != 0) continue;
        size_t a = (size_t)sp->entries[i].offset + 4;
        if (a + 20 > sp->size) return 0;
        const uint8_t *w = sp->base + a;
        if (ng_rd32(w) != 0x0B17C0DEu) return 0; /* expect a bitcode wrapper */
        uint32_t inner = ng_rd32(w + 8);
        uint32_t size = ng_rd32(w + 12);
        if (a + inner + size > sp->size) return 0;
        *out = w + inner;
        *outLen = size;
        return 1;
    }
    return 0;
}

static LLVMModuleRef ng_build_reduced(const NGSplit *sp, LLVMModuleRef shader) {
    LLVMContextRef ctx = L_GetModuleContext(shader);
    LLVMModuleRef out = L_ModuleCreateInCtx("ngreen.builtins", ctx);
    if (!out) return NULL;

    for (int pass = 0; pass < 2; pass++) {
        LLVMValueRef v = pass == 0 ? L_FirstFunction(shader) : L_FirstGlobal(shader);
        while (v) {
            if (L_IsDeclaration(v)) {
                const char *nm = L_GetValueName(v);
                const uint8_t *blob = NULL;
                size_t blobLen = 0;
                if (nm && *nm && ng_split_find(sp, nm, &blob, &blobLen)) {
                    LLVMMemoryBufferRef mb =
                        L_MemBufRange((const char *)blob, blobLen, "ngsplit", 0);
                    LLVMModuleRef tmp = NULL;
                    if (mb && L_ParseBitcodeInCtx2(ctx, mb, &tmp) == 0 && tmp) {
                        if (L_LinkModules2(out, tmp) != 0)
                            NGLOG("cannot link builtin '%s'", nm);
                    } else {
                        NGLOG("cannot parse builtin '%s' (%zu bytes)", nm, blobLen);
                    }
                    if (mb) L_DisposeMemBuf(mb);
                }
            }
            v = pass == 0 ? L_NextFunction(v) : L_NextGlobal(v);
        }
    }
    return out;
}

/* The plugin calls helper vtbl[0](helper, path) -> reader and reader vtbl[3](reader, Module *) -> Module *. */

typedef struct {
    void **vptr;
    NGSplit *split;
} NGReader;

typedef struct {
    void **vptr;
} NGHelper;

static void *ng_vslot_noop(void *self) {
    (void)self;
    return NULL;
}

static LLVMModuleRef ng_reader_getModule(NGReader *self, LLVMModuleRef shader) {
    if (!self || !self->split || !shader) return NULL;
    return ng_build_reduced(self->split, shader);
}

static void *g_readerVtbl[8];
static void *g_helperVtbl[8];

static NGReader *ng_helper_createReader(NGHelper *self, const char *path) {
    (void)self;
    NGSplit *sp = ng_split_open(path);
    if (!sp) return NULL;
    NGReader *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->vptr = g_readerVtbl;
    r->split = sp;
    return r;
}

static NGHelper g_helper;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void ng_init(void) {
    for (int i = 0; i < 8; i++) {
        g_readerVtbl[i] = (void *)ng_vslot_noop;
        g_helperVtbl[i] = (void *)ng_vslot_noop;
    }
    g_helperVtbl[0] = (void *)ng_helper_createReader;
    g_readerVtbl[3] = (void *)ng_reader_getModule;
    g_helper.vptr = g_helperVtbl;

    void *d = dlopen(DONOR_PATH, RTLD_LAZY | RTLD_LOCAL);
    if (!d) {
        NGLOG("cannot load the 2020 plugin: %s", dlerror());
        return;
    }
    d_Create = dlsym(d, "MTLCompilerCreate");
    d_Delete = dlsym(d, "MTLCompilerDelete");
    d_SetHelper = dlsym(d, "MTLCompilerSetHelper");
    d_ReleaseReply = dlsym(d, "MTLCompilerReleaseReply");
    d_WithOptions = dlsym(d, "MTLCompilerBuildRequestWithOptions");

    void *l = dlopen(LLVM_PATH, RTLD_LAZY | RTLD_LOCAL);
    if (!l) {
        NGLOG("cannot load libLLVM: %s", dlerror());
        return;
    }
    L_ContextCreate = dlsym(l, "LLVMContextCreate");
    L_GetModuleContext = dlsym(l, "LLVMGetModuleContext");
    L_ModuleCreateInCtx = dlsym(l, "LLVMModuleCreateWithNameInContext");
    L_MemBufRange = dlsym(l, "LLVMCreateMemoryBufferWithMemoryRange");
    L_DisposeMemBuf = dlsym(l, "LLVMDisposeMemoryBuffer");
    L_ParseBitcodeInCtx2 = dlsym(l, "LLVMParseBitcodeInContext2");
    L_LinkModules2 = dlsym(l, "LLVMLinkModules2");
    L_FirstFunction = dlsym(l, "LLVMGetFirstFunction");
    L_NextFunction = dlsym(l, "LLVMGetNextFunction");
    L_FirstGlobal = dlsym(l, "LLVMGetFirstGlobal");
    L_NextGlobal = dlsym(l, "LLVMGetNextGlobal");
    L_IsDeclaration = dlsym(l, "LLVMIsDeclaration");
    L_GetValueName = dlsym(l, "LLVMGetValueName");

    g_ok = d_Create && d_Delete && d_SetHelper && d_ReleaseReply && d_WithOptions &&
           L_GetModuleContext && L_ModuleCreateInCtx && L_MemBufRange && L_ParseBitcodeInCtx2 &&
           L_LinkModules2 && L_FirstFunction && L_NextFunction && L_FirstGlobal && L_NextGlobal &&
           L_IsDeclaration && L_GetValueName;

    if (!g_ok) NGLOG("missing symbols in the 2020 plugin or libLLVM");
}

void *MTLCompilerCreate(void *hwInfo) {
    pthread_once(&g_once, ng_init);
    if (!g_ok) return NULL;
    return d_Create(hwInfo);
}

void MTLCompilerDelete(void *c) {
    pthread_once(&g_once, ng_init);
    if (g_ok) d_Delete(c);
}

void MTLCompilerReleaseReply(void *c) {
    pthread_once(&g_once, ng_init);
    if (g_ok) d_ReleaseReply(c);
}

void MTLCompilerSetHelper(void *compiler, void *hostHelper) {
    pthread_once(&g_once, ng_init);
    if (!g_ok) return;
    d_SetHelper(compiler, &g_helper);
}

int MTLCompilerBuildRequestWithOptions(void *handle, const void *a3, unsigned long a4, unsigned a5,
                                       void *module, void *out, void *s1, void *s2, void *s3,
                                       void *s4, void *s5, void *s6) {
    pthread_once(&g_once, ng_init);
    if (!g_ok) return 1;
    return d_WithOptions(handle, a3, a4, a5, module, out, s1, s2, s3, s4, s5, s6);
}

/* Same arguments as MTLCompilerBuildRequestWithOptions with the module replaced by (bitcode, length);
 * on failure the error string goes to out + 0x10 and the result is 1. */
int MTLCompilerBuildRequestWithSerializedBitcode(void *handle, const void *a3, unsigned long a4,
                                                 unsigned a5, const void *bitcode,
                                                 unsigned long length, void *out, void *s1,
                                                 void *s2, void *s3, void *s4, void *s5,
                                                 void *s6) {
    pthread_once(&g_once, ng_init);
    if (!g_ok) {
        if (out) *(const char **)((char *)out + 0x10) = "NGreen IGC shim: not initialised";
        return 1;
    }
    if (!bitcode || length == 0) {
        if (out) *(const char **)((char *)out + 0x10) = "NGreen IGC shim: empty serialized bitcode";
        return 1;
    }

    LLVMContextRef ctx = L_ContextCreate();
    LLVMMemoryBufferRef mb = L_MemBufRange((const char *)bitcode, (size_t)length, "air", 0);
    LLVMModuleRef mod = NULL;
    LLVMBool rc = (ctx && mb) ? L_ParseBitcodeInCtx2(ctx, mb, &mod) : 1;
    if (mb) L_DisposeMemBuf(mb);

    if (rc != 0 || !mod) {
        NGLOG("LLVM 3902 could not parse %lu bytes of serialized bitcode", length);
        if (out)
            *(const char **)((char *)out + 0x10) =
                "NGreen IGC shim: LLVM 3902 could not parse the serialized bitcode";
        return 1;
    }

    return d_WithOptions(handle, a3, a4, a5, mod, out, s1, s2, s3, s4, s5, s6);
}

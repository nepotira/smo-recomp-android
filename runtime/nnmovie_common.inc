/* nnmovie_common.inc - the part of the movie:: library that needs no decoder: argument macros, status codes,
 * calls into the game (its allocator), movie::MediaData and movie::Buffer. Included by nnmovie_mf.c (Windows,
 * Media Foundation) and android/android_movie.c. Moved out of nnmovie_mf.c for the Android port (2026-10-28):
 * without MediaData the game crashed after a video failed to open (Cap Kingdom's camera tutorial). */
#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))

void os_signal_event(uint64_t key);                   /* os_threads.c */

#define ST_OK 0
#define ST_FAILED 0x1000
#define ST_INVALID 0x100b
#define ST_UNSUPPORTED 0x1006
#define ST_END 0x2000
#define ST_NO_INPUT 0x1011                             /* AcquireInputBufferIndex: nothing offered [subsdk 0x1fac8] */
#define ST_NO_OUTPUT 0x100e                            /* AcquireOutputBufferIndex: nothing decoded [subsdk 0x1e8d0] */
#define FLAG_EOS 4
#define MAX_TRACKS 8
#define SLOT_BYTES 64
#define GAME_DELETE GAME_SYM__ZdlPv              /* the game's operator delete(void *) */
#define TOKEN_MAGIC 0x4d4f564945544f4bULL             /* "MOVIETOK" */

/* ---------------------------------------------------------------- small helpers */
static int g_log;                                     /* SMO_MOVIE_LOG */
static int g_test_resync_flag, g_test_resync_at;      /* SMO_MOVIE_TEST_RESYNC */
static uint64_t g_alloc_fn, g_free_fn, g_alloc_user;  /* movie::SetAllocator */


/* Call a guest function with up to five arguments; the caller's registers are preserved. */
static uint64_t gcall(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t save[9], lr = c->x[30], f = c->fn;
    memcpy(save, c->x, sizeof save);
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4;
    call_guest(c, fn);
    uint64_t r = c->x[0];
    memcpy(c->x, save, sizeof save);
    c->x[30] = lr; c->fn = f;
    return r;
}

static void *galloc(Ctx *c, size_t size, size_t align, int *owns) {
    if (g_alloc_fn) {
        void *p = (void *)(uintptr_t)gcall(c, g_alloc_fn, size, align, g_alloc_user, 0, 0);
        if (p) { *owns = 1; return p; }
    }
    *owns = 2;
    return plat_aligned_malloc(size ? size : 16, align < 16 ? 16 : align);
}
static void gfree(Ctx *c, void *p, int owns) {
    if (!p) return;
    if (owns == 1 && g_free_fn) gcall(c, g_free_fn, (uint64_t)(uintptr_t)p, g_alloc_user, 0, 0, 0);
    else if (owns == 2) plat_aligned_free(p);
}

/* ---------------------------------------------------------------- movie::MediaData */
enum { MD_NONE, MD_I32, MD_I64, MD_F32, MD_F64, MD_STR, MD_PTR, MD_SIZE, MD_RECT, MD_BUF };
typedef struct { char key[40]; int type; int32_t i32; int64_t i64; float f32; double f64; char *str; uint64_t ptr; int rect[4]; } MDEntry;
typedef struct { int n; MDEntry e[40]; } MD;

static MD *md_of(uint64_t guest) { return guest ? (MD *)(uintptr_t)rd64(guest) : NULL; }
static MDEntry *md_find(MD *m, const char *key) {
    if (!m) return NULL;
    for (int i = 0; i < m->n; i++) if (strcmp(m->e[i].key, key) == 0) return &m->e[i];
    return NULL;
}
static MDEntry *md_put(MD *m, const char *key, int type) {
    MDEntry *e = md_find(m, key);
    if (!e) {
        if (m->n >= (int)(sizeof m->e / sizeof m->e[0])) return NULL;
        e = &m->e[m->n++];
        memset(e, 0, sizeof *e);
        strncpy(e->key, key, sizeof e->key - 1);
    } else if (e->str) { free(e->str); e->str = NULL; }
    e->type = type;
    return e;
}
static void md_set_i32(MD *m, const char *k, int32_t v) { MDEntry *e = md_put(m, k, MD_I32); if (e) e->i32 = v; }
static void md_set_i64(MD *m, const char *k, int64_t v) { MDEntry *e = md_put(m, k, MD_I64); if (e) e->i64 = v; }
static void md_set_f32(MD *m, const char *k, float v) { MDEntry *e = md_put(m, k, MD_F32); if (e) e->f32 = v; }
static void md_set_str(MD *m, const char *k, const char *v) { MDEntry *e = md_put(m, k, MD_STR); if (e) e->str = strdup(v ? v : ""); }
static void md_set_ptr(MD *m, const char *k, uint64_t v) { MDEntry *e = md_put(m, k, MD_PTR); if (e) e->ptr = v; }
static void md_clear(MD *m) { if (!m) return; for (int i = 0; i < m->n; i++) free(m->e[i].str); m->n = 0; }
static int md_get_i32(MD *m, const char *k, int32_t *out) {
    MDEntry *e = md_find(m, k);
    if (!e) return 0;
    if (e->type == MD_I32) *out = e->i32; else if (e->type == MD_I64) *out = (int32_t)e->i64; else if (e->type == MD_SIZE) *out = (int32_t)e->i64; else return 0;
    return 1;
}
static int md_get_i64(MD *m, const char *k, int64_t *out) {
    MDEntry *e = md_find(m, k);
    if (!e) return 0;
    if (e->type == MD_I64 || e->type == MD_SIZE) *out = e->i64; else if (e->type == MD_I32) *out = e->i32; else return 0;
    return 1;
}
static int md_get_str(MD *m, const char *k, const char **out) { MDEntry *e = md_find(m, k); if (!e || e->type != MD_STR) return 0; *out = e->str; return 1; }
static int md_get_ptr(MD *m, const char *k, uint64_t *out) { MDEntry *e = md_find(m, k); if (!e || e->type != MD_PTR) return 0; *out = e->ptr; return 1; }

static void mdata_ctor(Ctx *c) { MD *m = calloc(1, sizeof *m); wr64(A0, (uint64_t)(uintptr_t)m); }
static void mdata_copy_ctor(Ctx *c) {                 /* MediaData(const MediaData &) */
    MD *m = calloc(1, sizeof *m), *src = md_of(A1);
    if (src) { *m = *src; for (int i = 0; i < m->n; i++) if (m->e[i].str) m->e[i].str = strdup(m->e[i].str); }
    wr64(A0, (uint64_t)(uintptr_t)m);
}
static void mdata_dtor(Ctx *c) { MD *m = md_of(A0); if (m) { md_clear(m); free(m); wr64(A0, 0); } }
static void mdata_Clear(Ctx *c) { md_clear(md_of(A0)); }
static void mdata_SetInt32(Ctx *c) { MD *m = md_of(A0); if (m) md_set_i32(m, (const char *)(uintptr_t)A1, (int32_t)A2); }
static void mdata_SetInt64(Ctx *c) { MD *m = md_of(A0); if (m) md_set_i64(m, (const char *)(uintptr_t)A1, (int64_t)A2); }
static void mdata_SetString(Ctx *c) { MD *m = md_of(A0); if (m) md_set_str(m, (const char *)(uintptr_t)A1, (const char *)(uintptr_t)A2); }
static void mdata_SetFloat(Ctx *c) { MD *m = md_of(A0); if (m) md_set_f32(m, (const char *)(uintptr_t)A1, c->v[0].f[0]); }
static void mdata_SetPointer(Ctx *c) { MD *m = md_of(A0); if (m) md_set_ptr(m, (const char *)(uintptr_t)A1, A2); }
static void mdata_FindInt32(Ctx *c) { int32_t v = 0; int ok = md_get_i32(md_of(A0), (const char *)(uintptr_t)A1, &v); if (ok && A2) wr32(A2, (uint32_t)v); RET(ok); }
static void mdata_FindInt64(Ctx *c) { int64_t v = 0; int ok = md_get_i64(md_of(A0), (const char *)(uintptr_t)A1, &v); if (ok && A2) wr64(A2, (uint64_t)v); RET(ok); }
static void mdata_FindString(Ctx *c) { const char *s = NULL; int ok = md_get_str(md_of(A0), (const char *)(uintptr_t)A1, &s); if (ok && A2) wr64(A2, (uint64_t)(uintptr_t)s); RET(ok); }
static void mdata_FindPointer(Ctx *c) { uint64_t v = 0; int ok = md_get_ptr(md_of(A0), (const char *)(uintptr_t)A1, &v); if (ok && A2) wr64(A2, v); RET(ok); }
static void mdata_Contains(Ctx *c) { RET(md_find(md_of(A0), (const char *)(uintptr_t)A1) != NULL); }
static void mdata_CountEntries(Ctx *c) { MD *m = md_of(A0); RET(m ? m->n : 0); }
static void mdata_assign(Ctx *c) {                    /* operator= */
    MD *m = md_of(A0), *src = md_of(A1);
    if (m && src && m != src) { md_clear(m); *m = *src; for (int i = 0; i < m->n; i++) if (m->e[i].str) m->e[i].str = strdup(m->e[i].str); }
    RET(A0);
}

/* ---------------------------------------------------------------- movie::Buffer */
#define B_OFFSET 0x00
#define B_SIZE 0x04
#define B_I32 0x08
#define B_U32 0x0c
#define B_OWNS 0x10
#define B_CAP 0x18
#define B_I64 0x20
#define B_BASE 0x28
#define B_DEV 0x30
#define B_SYNC 0x38

static void buf_zero(uint64_t b) { memset((void *)(uintptr_t)b, 0, 0x40); }
static void buf_ctor(Ctx *c) { buf_zero(A0); }
static void buf_ctor_size(Ctx *c) {                   /* Buffer(size_t capacity): owns its memory */
    uint64_t b = A0, cap = A1;
    int owns = 0;
    void *p = galloc(c, (size_t)cap, 16, &owns);
    buf_zero(b);
    wr64(b + B_CAP, cap);
    wr64(b + B_BASE, (uint64_t)(uintptr_t)p);
    wr8(b + B_OWNS, (uint8_t)(p ? owns : 0));
}
static void buf_ctor_data(Ctx *c) {                   /* Buffer(void *data, size_t capacity): borrowed */
    uint64_t b = A0;
    buf_zero(b);
    wr64(b + B_BASE, A1);
    wr64(b + B_CAP, A2);
}
static void buf_copy_from(Ctx *c, uint64_t b, uint64_t src) {
    uint8_t owns = rd8(src + B_OWNS);
    uint64_t cap = rd64(src + B_CAP), base = rd64(src + B_BASE);
    wr32(b + B_OFFSET, rd32(src + B_OFFSET)); wr32(b + B_SIZE, rd32(src + B_SIZE));
    wr32(b + B_I32, rd32(src + B_I32)); wr32(b + B_U32, rd32(src + B_U32));
    wr64(b + B_I64, rd64(src + B_I64)); wr64(b + B_DEV, rd64(src + B_DEV)); wr64(b + B_SYNC, rd64(src + B_SYNC));
    wr64(b + B_CAP, cap);
    if (owns && base) {                               /* an owning buffer is deep-copied */
        int o = 0;
        void *p = galloc(c, (size_t)cap, 16, &o);
        if (p) memcpy(p, (void *)(uintptr_t)base, (size_t)cap);
        wr64(b + B_BASE, (uint64_t)(uintptr_t)p);
        wr8(b + B_OWNS, (uint8_t)(p ? o : 0));
    } else {
        wr64(b + B_BASE, base);
        wr8(b + B_OWNS, 0);
    }
}
static void buf_release(Ctx *c, uint64_t b) {
    uint8_t owns = rd8(b + B_OWNS);
    if (owns) gfree(c, (void *)(uintptr_t)rd64(b + B_BASE), owns);
    wr64(b + B_BASE, 0);
    wr8(b + B_OWNS, 0);
}
static void buf_copy_ctor(Ctx *c) { buf_zero(A0); buf_copy_from(c, A0, A1); }
static void buf_assign(Ctx *c) { if (A0 != A1) { buf_release(c, A0); buf_copy_from(c, A0, A1); } RET(A0); }
static void buf_dtor(Ctx *c) { buf_release(c, A0); }
static void buf_SetDataAndCapacity(Ctx *c) { buf_release(c, A0); wr64(A0 + B_BASE, A1); wr64(A0 + B_CAP, A2); }
static void buf_SetCapacity(Ctx *c) { wr64(A0 + B_CAP, A1); }
static void buf_SetRange(Ctx *c) { wr32(A0 + B_OFFSET, (uint32_t)A1); wr32(A0 + B_SIZE, (uint32_t)A2); }
static void buf_Base(Ctx *c) { RET(rd64(A0 + B_BASE)); }
static void buf_Data(Ctx *c) { RET(rd64(A0 + B_BASE) + (int32_t)rd32(A0 + B_OFFSET)); }
static void buf_Size(Ctx *c) { RET(rd32(A0 + B_SIZE)); }
static void buf_Offset(Ctx *c) { RET(rd32(A0 + B_OFFSET)); }
static void buf_Capacity(Ctx *c) { RET(rd64(A0 + B_CAP)); }
static void buf_SetInt32Data(Ctx *c) { wr32(A0 + B_I32, (uint32_t)A1); }
static void buf_SetInt64Data(Ctx *c) { wr64(A0 + B_I64, A1); }
static void buf_SetUint32Data(Ctx *c) { wr32(A0 + B_U32, (uint32_t)A1); }
static void buf_GetInt32Data(Ctx *c) { RET(rd32(A0 + B_I32)); }
static void buf_GetInt64Data(Ctx *c) { RET(rd64(A0 + B_I64)); }
static void buf_GetUint32Data(Ctx *c) { RET(rd32(A0 + B_U32)); }
static void buf_SetNvnDev(Ctx *c) { wr64(A0 + B_DEV, A1); }
static void buf_GetNvnDev(Ctx *c) { RET(rd64(A0 + B_DEV)); }
static void buf_SetNvnSync(Ctx *c) { wr64(A0 + B_SYNC, A1); }
static void buf_GetNvnSync(Ctx *c) { RET(rd64(A0 + B_SYNC)); }
static void buf_Swap(Ctx *c) { unsigned char t[0x40]; memcpy(t, (void *)(uintptr_t)A0, 0x40); memcpy((void *)(uintptr_t)A0, (void *)(uintptr_t)A1, 0x40); memcpy((void *)(uintptr_t)A1, t, 0x40); }


static void movie_SetAllocator(Ctx *c) {              /* (alloc, free, realloc, user) */
    g_alloc_fn = A0; g_free_fn = A1; g_alloc_user = A3;
    printf("movie: SetAllocator(alloc %llx, free %llx, realloc %llx, user %llx)\n", (unsigned long long)A0, (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3);
}

/* the import rows of what is above (the Windows file lists them in its own table too) */
static const ImportDef movie_data_imports[] = {
    {"_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", movie_SetAllocator},
    /* movie::Buffer */
    {"_ZN5movie6BufferC1Ev", buf_ctor}, {"_ZN5movie6BufferC2Ev", buf_ctor},
    {"_ZN5movie6BufferC1Em", buf_ctor_size}, {"_ZN5movie6BufferC2Em", buf_ctor_size},
    {"_ZN5movie6BufferC1EPvm", buf_ctor_data}, {"_ZN5movie6BufferC2EPvm", buf_ctor_data},
    {"_ZN5movie6BufferC1ERKS0_", buf_copy_ctor}, {"_ZN5movie6BufferC2ERKS0_", buf_copy_ctor},
    {"_ZN5movie6BufferD1Ev", buf_dtor}, {"_ZN5movie6BufferD2Ev", buf_dtor},
    {"_ZN5movie6BufferaSERKS0_", buf_assign},
    {"_ZN5movie6Buffer18SetDataAndCapacityEPvm", buf_SetDataAndCapacity}, {"_ZN5movie6Buffer11SetCapacityEm", buf_SetCapacity},
    {"_ZN5movie6Buffer8SetRangeEii", buf_SetRange}, {"_ZN5movie6Buffer4SwapERS0_", buf_Swap},
    {"_ZNK5movie6Buffer4BaseEv", buf_Base}, {"_ZNK5movie6Buffer4DataEv", buf_Data}, {"_ZNK5movie6Buffer4SizeEv", buf_Size},
    {"_ZNK5movie6Buffer6OffsetEv", buf_Offset}, {"_ZNK5movie6Buffer8CapacityEv", buf_Capacity},
    {"_ZN5movie6Buffer12SetInt32DataEi", buf_SetInt32Data}, {"_ZN5movie6Buffer12SetInt64DataEl", buf_SetInt64Data},
    {"_ZN5movie6Buffer13SetUint32DataEj", buf_SetUint32Data}, {"_ZNK5movie6Buffer12GetInt32DataEv", buf_GetInt32Data},
    {"_ZNK5movie6Buffer12GetInt64DataEv", buf_GetInt64Data}, {"_ZNK5movie6Buffer13GetUint32DataEv", buf_GetUint32Data},
    {"_ZN5movie6Buffer9SetNvnDevEPv", buf_SetNvnDev}, {"_ZNK5movie6Buffer9GetNvnDevEv", buf_GetNvnDev},
    {"_ZN5movie6Buffer10SetNvnSyncEPv", buf_SetNvnSync}, {"_ZNK5movie6Buffer10GetNvnSyncEv", buf_GetNvnSync},
    /* movie::MediaData */
    {"_ZN5movie9MediaDataC1Ev", mdata_ctor}, {"_ZN5movie9MediaDataC2Ev", mdata_ctor},
    {"_ZN5movie9MediaDataC1ERKS0_", mdata_copy_ctor}, {"_ZN5movie9MediaDataC2ERKS0_", mdata_copy_ctor},
    {"_ZN5movie9MediaDataD1Ev", mdata_dtor}, {"_ZN5movie9MediaDataD2Ev", mdata_dtor},
    {"_ZN5movie9MediaDataaSERKS0_", mdata_assign},
    {"_ZN5movie9MediaData5ClearEv", mdata_Clear},
    {"_ZN5movie9MediaData8SetInt32EPKci", mdata_SetInt32}, {"_ZN5movie9MediaData8SetInt64EPKcl", mdata_SetInt64},
    {"_ZN5movie9MediaData9SetStringEPKcS2_", mdata_SetString}, {"_ZN5movie9MediaData8SetFloatEPKcf", mdata_SetFloat},
    {"_ZN5movie9MediaData10SetPointerEPKcPv", mdata_SetPointer},
    {"_ZNK5movie9MediaData9FindInt32EPKcPi", mdata_FindInt32}, {"_ZNK5movie9MediaData9FindInt64EPKcPl", mdata_FindInt64},
    {"_ZNK5movie9MediaData10FindStringEPKcPS2_", mdata_FindString}, {"_ZNK5movie9MediaData11FindPointerEPKcPPv", mdata_FindPointer},
    {"_ZNK5movie9MediaData8ContainsEPKc", mdata_Contains}, {"_ZNK5movie9MediaData12CountEntriesEv", mdata_CountEntries},
};

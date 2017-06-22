#include "deps/bloom.h"
#include "redismodule.h"
#define BLOOM_CALLOC RedisModule_Calloc
#define BLOOM_FREE RedisModule_Free
#include "deps/bloom.c"
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// Core                                                                     ///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

typedef struct linkedBloom {
    struct bloom inner;       //< Inner structure
    size_t fillbits;          //< Number of bits currently filled
    struct linkedBloom *next; //< Prior filter
} linkedBloom;

typedef struct scalableBloom {
    linkedBloom *cur;
    size_t total_entries;
    double error;
    int is_fixed;
} scalableBloom;

static linkedBloom *lb_create(size_t size, double error_rate) {
    linkedBloom *lb = RedisModule_Calloc(1, sizeof(*lb));
    if (size < 1000) {
        size = 1000;
    }
    bloom_init(&lb->inner, size, error_rate);
    return lb;
}

static void sb_free(scalableBloom *sb) {
    linkedBloom *lb = sb->cur;
    while (lb) {
        linkedBloom *lb_next = lb->next;
        bloom_free(&lb->inner);
        RedisModule_Free(lb);
        lb = lb_next;
    }
    RedisModule_Free(sb);
}

static int lb_add(linkedBloom *lb, const void *data, size_t len) {
    int newbits = bloom_add_retbits(&lb->inner, data, len);
    lb->fillbits += newbits;
    return newbits;
}

static int sb_check(const scalableBloom *sb, const void *data, size_t len);

static int sb_add(scalableBloom *sb, const void *data, size_t len) {
    // Does it already exist?

    if (sb_check(sb, data, len)) {
        return 1;
    }

    // Determine if we need to add more items?
    if (sb->cur->fillbits * 2 > sb->cur->inner.bits) {
        linkedBloom *new_lb = lb_create(sb->cur->inner.entries * 2, sb->error);
        new_lb->next = sb->cur;
        sb->cur = new_lb;
    }
    int rv = lb_add(sb->cur, data, len);
    if (rv) {
        sb->total_entries++;
    }
    return rv;
}

static int sb_check(const scalableBloom *sb, const void *data, size_t len) {
    for (const linkedBloom *lb = sb->cur; lb; lb = lb->next) {
        if (bloom_check(&lb->inner, data, len)) {
            return 1;
        }
    }
    return 0;
}

static scalableBloom *sb_create(size_t initsize, double error_rate) {
    scalableBloom *sb = RedisModule_Calloc(1, sizeof(*sb));
    sb->error = error_rate;
    sb->cur = lb_create(initsize, error_rate);
    return sb;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// Redis Commands                                                           ///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
static RedisModuleType *BFType;
static double BFDefaultErrorRate = 0.01;

typedef enum { SB_OK = 0, SB_MISSING, SB_EMPTY, SB_MISMATCH } lookupStatus;

static int bf_get_sb(RedisModuleKey *key, scalableBloom **sbout) {
    *sbout = NULL;
    if (key == NULL) {
        return SB_MISSING;
    }
    int type = RedisModule_KeyType(key);
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        return SB_EMPTY;
    } else if (type == REDISMODULE_KEYTYPE_MODULE && RedisModule_ModuleTypeGetType(key) == BFType) {
        *sbout = RedisModule_ModuleTypeGetValue(key);
        return SB_OK;
    } else {
        return SB_MISMATCH;
    }
}

static const char *status_strerror(int status) {
    switch (status) {
    case SB_MISSING:
        return "ERR not found";
    case SB_MISMATCH:
        return "ERR mismatched type";
    case SB_OK:
        return "ERR item exists";
    default:
        return "Unknown error";
    }
}

static int return_with_error(RedisModuleCtx *ctx, const char *errmsg) {
    RedisModule_ReplyWithError(ctx, errmsg);
    return REDISMODULE_ERR;
}

/**
 * Common function for adding one or more items to a bloom filter.
 * @param key the key key associated with the filter
 * @param sb the actual bloom filter
 * @param is_fixed - for creating only, whether this filter is expected to
 *        be fixed
 * @param error_rate error rate for new filter
 * @param elems list of elements to add
 * @param nelems number of elements to add
 */
static void bf_add_common(RedisModuleKey *key, scalableBloom *sb, int is_fixed, double error_rate,
                          RedisModuleString **elems, int nelems) {
    if (sb == NULL) {
        if (!error_rate) {
            error_rate = BFDefaultErrorRate;
        }
        sb = sb_create(nelems, error_rate);
        RedisModule_ModuleTypeSetValue(key, BFType, sb);
        sb->is_fixed = is_fixed;
    }
    // Now, just add the items
    for (size_t ii = 0; ii < nelems; ++ii) {
        size_t n;
        const char *s = RedisModule_StringPtrLen(elems[ii], &n);
        sb_add(sb, s, n);
    }
}

static int BFCreate_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 3) {
        // CMD, ERR, K1
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    double error_rate;
    if (RedisModule_StringToDouble(argv[2], &error_rate) != REDISMODULE_OK) {
        return return_with_error(ctx, "ERR error rate required");
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    scalableBloom *sb;
    int status = bf_get_sb(key, &sb);
    if (status != SB_EMPTY) {
        return return_with_error(ctx, status_strerror(status));
    }

    bf_add_common(key, NULL, 1, error_rate, argv + 3, argc - 3);
    RedisModule_ReplyWithNull(ctx);
    return REDISMODULE_OK;
}

static int BFCheck_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    scalableBloom *sb;
    int status = bf_get_sb(key, &sb);
    if (status != SB_OK) {
        return return_with_error(ctx, status_strerror(status));
    }

    // Check if it exists?
    size_t n;
    const char *s = RedisModule_StringPtrLen(argv[2], &n);
    int exists = sb_check(sb, s, n);
    RedisModule_ReplyWithLongLong(ctx, exists);
    return REDISMODULE_OK;
}

static int BFAdd_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc < 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    scalableBloom *sb;
    int status = bf_get_sb(key, &sb);
    if (status == SB_OK) {
        if (sb->is_fixed) {
            return return_with_error(ctx, "ERR cannot add: filter is fixed");
        }
        size_t namelen;
        const char *cmdname = RedisModule_StringPtrLen(argv[0], &namelen);
        static const char setnxcmd[] = "BF.SETNX";
        if (namelen == sizeof(setnxcmd) - 1 && !strncasecmp(cmdname, "BF.SETNX", namelen)) {
            return return_with_error(ctx, "ERR filter already exists");
        }
    } else if (status != SB_EMPTY) {
        return_with_error(ctx, status_strerror(status));
    }

    bf_add_common(key, sb, 0, 0, argv + 2, argc - 2);
    RedisModule_ReplyWithNull(ctx);
    return REDISMODULE_OK;
}

static int BFInfo_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    const scalableBloom *sb = NULL;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int status = bf_get_sb(key, (scalableBloom **)&sb);
    if (status != SB_OK) {
        return return_with_error(ctx, status_strerror(status));
    }

    // Start writing info
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    // 2
    RedisModule_ReplyWithSimpleString(ctx, "size");
    RedisModule_ReplyWithLongLong(ctx, sb->total_entries);

    // 4
    RedisModule_ReplyWithSimpleString(ctx, "fixed");
    RedisModule_ReplyWithLongLong(ctx, sb->is_fixed);

    // 6
    RedisModule_ReplyWithSimpleString(ctx, "ratio");
    RedisModule_ReplyWithDouble(ctx, sb->error);

    // 7
    RedisModule_ReplyWithSimpleString(ctx, "filters");

    size_t num_elems = 0;

    for (const linkedBloom *lb = sb->cur; lb; lb = lb->next, num_elems++) {
        RedisModule_ReplyWithArray(ctx, 10);

        // 2
        RedisModule_ReplyWithSimpleString(ctx, "bytes");
        RedisModule_ReplyWithLongLong(ctx, lb->inner.bytes);

        // 4
        RedisModule_ReplyWithSimpleString(ctx, "bits");
        RedisModule_ReplyWithLongLong(ctx, lb->inner.bits);

        // 6
        RedisModule_ReplyWithSimpleString(ctx, "num_filled");
        RedisModule_ReplyWithLongLong(ctx, lb->fillbits);

        // 8
        RedisModule_ReplyWithSimpleString(ctx, "hashes");
        RedisModule_ReplyWithLongLong(ctx, lb->inner.hashes);

        // 10
        RedisModule_ReplyWithSimpleString(ctx, "capacity");
        RedisModule_ReplyWithLongLong(ctx, lb->inner.entries);
    }

    RedisModule_ReplySetArrayLength(ctx, 7 + num_elems);
    return REDISMODULE_OK;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// Datatype Functions                                                       ///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
static void BFRdbSave(RedisModuleIO *io, void *obj) {
    // Save the setting!
    scalableBloom *sb = obj;
    // We don't know how many links are here thus far, so
    RedisModule_SaveUnsigned(io, sb->total_entries);
    RedisModule_SaveDouble(io, sb->error);
    RedisModule_SaveUnsigned(io, sb->is_fixed);
    for (const linkedBloom *lb = sb->cur; lb; lb = lb->next) {
        const struct bloom *bm = &lb->inner;
        RedisModule_SaveUnsigned(io, bm->entries);
        // - SKIP: error ratio is fixed, and stored as part of the header
        // - SKIP: bits is (double)entries * bpe
        RedisModule_SaveUnsigned(io, bm->hashes);
        RedisModule_SaveDouble(io, bm->bpe);
        RedisModule_SaveStringBuffer(io, (const char *)bm->bf, bm->bytes);

        // Save the number of actual entries stored thus far.
        RedisModule_SaveUnsigned(io, lb->fillbits);
    }

    // Finally, save the last 0 indicating that nothing more follows:
    RedisModule_SaveUnsigned(io, 0);
}

static void *BFRdbLoad(RedisModuleIO *io, int encver) {
    if (encver != 0) {
        return NULL;
    }

    // Load our modules
    scalableBloom *sb = RedisModule_Calloc(1, sizeof(*sb));
    sb->total_entries = RedisModule_LoadUnsigned(io);
    sb->error = RedisModule_LoadDouble(io);
    sb->is_fixed = RedisModule_LoadUnsigned(io);

    // Now load the individual nodes
    while (1) {
        unsigned entries = RedisModule_LoadUnsigned(io);
        if (!entries) {
            break;
        }
        linkedBloom *lb = RedisModule_Calloc(1, sizeof(*lb));
        struct bloom *bm = &lb->inner;

        bm->entries = entries;
        bm->error = sb->error;
        bm->hashes = RedisModule_LoadUnsigned(io);
        bm->bpe = RedisModule_LoadDouble(io);
        bm->bits = (double)bm->entries * bm->bpe;
        size_t sztmp;
        bm->bf = (unsigned char *)RedisModule_LoadStringBuffer(io, &sztmp);
        bm->bytes = sztmp;
        bm->ready = 1;
        lb->fillbits = RedisModule_LoadUnsigned(io);
        lb->next = sb->cur;
        sb->cur = lb;
    }

    return sb;
}

static void BFAofRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
    // TODO
    (void)aof;
    (void)key;
    (void)value;
}

static void BFFree(void *value) { sb_free(value); }

static size_t BFMemUsage(const void *value) {
    const scalableBloom *sb = value;
    size_t rv = sizeof(*sb);
    for (const linkedBloom *lb = sb->cur; lb; lb = lb->next) {
        rv += sizeof(*lb);
        rv += lb->inner.bytes;
    }
    return rv;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "bf", 1, REDISMODULE_APIVER_1) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "BF.CREATE", BFCreate_RedisCommand, "write", 1, 1, 1) !=
        REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "BF.SET", BFAdd_RedisCommand, "write", 1, 1, 1) !=
        REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "BF.SETNX", BFAdd_RedisCommand, "write", 1, 1, 1) !=
        REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "BF.TEST", BFCheck_RedisCommand, "readonly", 1, 1, 1) !=
        REDISMODULE_OK)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx, "BF.DEBUG", BFInfo_RedisCommand, "readonly", 1, 1, 1) !=
        REDISMODULE_OK)
        return REDISMODULE_ERR;

    static RedisModuleTypeMethods typeprocs = {.version = REDISMODULE_TYPE_METHOD_VERSION,
                                               .rdb_load = BFRdbLoad,
                                               .rdb_save = BFRdbSave,
                                               .aof_rewrite = BFAofRewrite,
                                               .free = BFFree,
                                               .mem_usage = BFMemUsage};
    BFType = RedisModule_CreateDataType(ctx, "MBbloom--", 0, &typeprocs);
    return BFType == NULL ? REDISMODULE_ERR : REDISMODULE_OK;
}
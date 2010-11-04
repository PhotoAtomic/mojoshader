/**
 * MojoShader; generate shader programs from bytecode of compiled
 *  Direct3D shaders.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#define __MOJOSHADER_INTERNAL__ 1
#include "mojoshader_internal.h"

#if DEBUG_PREPROCESSOR
    #define print_debug_token(token, len, val) \
        MOJOSHADER_print_debug_token("PREPROCESSOR", token, len, val)
#else
    #define print_debug_token(token, len, val)
#endif

#if DEBUG_LEXER
static Token debug_preprocessor_lexer(IncludeState *s)
{
    const Token retval = preprocessor_lexer(s);
    MOJOSHADER_print_debug_token("LEXER", s->token, s->tokenlen, retval);
    return retval;
} // debug_preprocessor_lexer
#define preprocessor_lexer(s) debug_preprocessor_lexer(s)
#endif

#if DEBUG_TOKENIZER
static void print_debug_lexing_position(IncludeState *s)
{
    if (s != NULL)
        printf("NOW LEXING %s:%d ...\n", s->filename, s->line);
} // print_debug_lexing_position
#else
#define print_debug_lexing_position(s)
#endif

typedef struct Context
{
    int isfail;
    int out_of_memory;
    char failstr[256];
    int recursion_count;
    int asm_comments;
    int parsing_pragma;
    Conditional *conditional_pool;
    IncludeState *include_stack;
    IncludeState *include_pool;
    Define *define_hashtable[256];
    Define *define_pool;
    Define *file_macro;
    Define *line_macro;
    StringCache *filename_cache;
    MOJOSHADER_includeOpen open_callback;
    MOJOSHADER_includeClose close_callback;
    MOJOSHADER_malloc malloc;
    MOJOSHADER_free free;
    void *malloc_data;
} Context;


// Convenience functions for allocators...

static inline void out_of_memory(Context *ctx)
{
    ctx->out_of_memory = 1;
} // out_of_memory

static inline void *Malloc(Context *ctx, const size_t len)
{
    void *retval = ctx->malloc((int) len, ctx->malloc_data);
    if (retval == NULL)
        out_of_memory(ctx);
    return retval;
} // Malloc

static inline void Free(Context *ctx, void *ptr)
{
    ctx->free(ptr, ctx->malloc_data);
} // Free

static inline char *StrDup(Context *ctx, const char *str)
{
    char *retval = (char *) Malloc(ctx, strlen(str) + 1);
    if (retval != NULL)
        strcpy(retval, str);
    return retval;
} // StrDup

static void failf(Context *ctx, const char *fmt, ...) ISPRINTF(2,3);
static void failf(Context *ctx, const char *fmt, ...)
{
    ctx->isfail = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->failstr, sizeof (ctx->failstr), fmt, ap);
    va_end(ap);
} // failf

static inline void fail(Context *ctx, const char *reason)
{
    failf(ctx, "%s", reason);
} // fail


#if DEBUG_TOKENIZER
void MOJOSHADER_print_debug_token(const char *subsystem, const char *token,
                                  const unsigned int tokenlen,
                                  const Token tokenval)
{
    printf("%s TOKEN: \"", subsystem);
    unsigned int i;
    for (i = 0; i < tokenlen; i++)
    {
        if (token[i] == '\n')
            printf("\\n");
        else if (token[i] == '\\')
            printf("\\\\");
        else
            printf("%c", token[i]);
    } // for
    printf("\" (");
    switch (tokenval)
    {
        #define TOKENCASE(x) case x: printf("%s", #x); break
        TOKENCASE(TOKEN_UNKNOWN);
        TOKENCASE(TOKEN_IDENTIFIER);
        TOKENCASE(TOKEN_INT_LITERAL);
        TOKENCASE(TOKEN_FLOAT_LITERAL);
        TOKENCASE(TOKEN_STRING_LITERAL);
        TOKENCASE(TOKEN_ADDASSIGN);
        TOKENCASE(TOKEN_SUBASSIGN);
        TOKENCASE(TOKEN_MULTASSIGN);
        TOKENCASE(TOKEN_DIVASSIGN);
        TOKENCASE(TOKEN_MODASSIGN);
        TOKENCASE(TOKEN_XORASSIGN);
        TOKENCASE(TOKEN_ANDASSIGN);
        TOKENCASE(TOKEN_ORASSIGN);
        TOKENCASE(TOKEN_INCREMENT);
        TOKENCASE(TOKEN_DECREMENT);
        TOKENCASE(TOKEN_RSHIFT);
        TOKENCASE(TOKEN_LSHIFT);
        TOKENCASE(TOKEN_ANDAND);
        TOKENCASE(TOKEN_OROR);
        TOKENCASE(TOKEN_LEQ);
        TOKENCASE(TOKEN_GEQ);
        TOKENCASE(TOKEN_EQL);
        TOKENCASE(TOKEN_NEQ);
        TOKENCASE(TOKEN_HASH);
        TOKENCASE(TOKEN_HASHHASH);
        TOKENCASE(TOKEN_PP_INCLUDE);
        TOKENCASE(TOKEN_PP_LINE);
        TOKENCASE(TOKEN_PP_DEFINE);
        TOKENCASE(TOKEN_PP_UNDEF);
        TOKENCASE(TOKEN_PP_IF);
        TOKENCASE(TOKEN_PP_IFDEF);
        TOKENCASE(TOKEN_PP_IFNDEF);
        TOKENCASE(TOKEN_PP_ELSE);
        TOKENCASE(TOKEN_PP_ELIF);
        TOKENCASE(TOKEN_PP_ENDIF);
        TOKENCASE(TOKEN_PP_ERROR);
        TOKENCASE(TOKEN_PP_PRAGMA);
        TOKENCASE(TOKEN_INCOMPLETE_COMMENT);
        TOKENCASE(TOKEN_BAD_CHARS);
        TOKENCASE(TOKEN_EOI);
        TOKENCASE(TOKEN_PREPROCESSING_ERROR);
        #undef TOKENCASE

        case ((Token) '\n'):
            printf("'\\n'");
            break;

        case ((Token) '\\'):
            printf("'\\\\'");
            break;

        default:
            assert(((int)tokenval) < 256);
            printf("'%c'", (char) tokenval);
            break;
    } // switch
    printf(")\n");
} // MOJOSHADER_print_debug_token
#endif



#if !MOJOSHADER_FORCE_INCLUDE_CALLBACKS

// !!! FIXME: most of these _MSC_VER should probably be _WINDOWS?
#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>  // GL headers need this for WINGDIAPI definition.
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

int MOJOSHADER_internal_include_open(MOJOSHADER_includeType inctype,
                                     const char *fname, const char *parent,
                                     const char **outdata,
                                     unsigned int *outbytes,
                                     MOJOSHADER_malloc m, MOJOSHADER_free f,
                                     void *d)
{
#ifdef _MSC_VER
    WCHAR wpath[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, fname, -1, wpath, MAX_PATH))
        return 0;

    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const HANDLE handle = CreateFileW(wpath, FILE_GENERIC_READ, share,
                                      NULL, OPEN_EXISTING, NULL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;

    const DWORD fileSize = GetFileSize(handle, NULL);
    if (fileSize == INVALID_FILE_SIZE)
    {
        CloseHandle(handle);
        return 0;
    } // if

    char *data = (char *) m(fileSize, d);
    if (data == NULL)
    {
        CloseHandle(handle);
        return 0;
    } // if

    DWORD readLength = 0;
    if (!ReadFile(handle, data, fileSize, &readLength, NULL))
    {
        CloseHandle(handle);
        f(data, d);
        return 0;
    } // if

    CloseHandle(handle);

    if (readLength != fileSize)
    {
        f(data, d);
        return 0;
    } // if
    *outdata = data;
    *outbytes = fileSize;
    return 1;
#else
    struct stat statbuf;
    if (stat(fname, &statbuf) == -1)
        return 0;
    char *data = (char *) m(statbuf.st_size, d);
    if (data == NULL)
        return 0;
    const int fd = open(fname, O_RDONLY);
    if (fd == -1)
    {
        f(data, d);
        return 0;
    } // if
    if (read(fd, data, statbuf.st_size) != statbuf.st_size)
    {
        f(data, d);
        close(fd);
        return 0;
    } // if
    close(fd);
    *outdata = data;
    *outbytes = (unsigned int) statbuf.st_size;
    return 1;
#endif
} // MOJOSHADER_internal_include_open


void MOJOSHADER_internal_include_close(const char *data, MOJOSHADER_malloc m,
                                       MOJOSHADER_free f, void *d)
{
    f((void *) data, d);
} // MOJOSHADER_internal_include_close
#endif  // !MOJOSHADER_FORCE_INCLUDE_CALLBACKS


// data buffer stuff...

#define BUFFER_LEN (64 * 1024)
typedef struct BufferList
{
    char buffer[BUFFER_LEN];
    size_t bytes;
    struct BufferList *next;
} BufferList;

typedef struct Buffer
{
    size_t total_bytes;
    BufferList head;
    BufferList *tail;
} Buffer;

static void init_buffer(Buffer *buffer)
{
    buffer->total_bytes = 0;
    buffer->head.bytes = 0;
    buffer->head.next = NULL;
    buffer->tail = &buffer->head;
} // init_buffer


static int add_to_buffer(Buffer *buffer, const char *data,
                         size_t len, MOJOSHADER_malloc m, void *d)
{
    buffer->total_bytes += len;
    while (len > 0)
    {
        const size_t avail = BUFFER_LEN - buffer->tail->bytes;
        const size_t cpy = (avail > len) ? len : avail;
        memcpy(buffer->tail->buffer + buffer->tail->bytes, data, cpy);
        len -= cpy;
        data += cpy;
        buffer->tail->bytes += cpy;
        assert(buffer->tail->bytes <= BUFFER_LEN);
        if (buffer->tail->bytes == BUFFER_LEN)
        {
            BufferList *item = (BufferList *) m(sizeof (BufferList), d);
            if (item == NULL)
                return 0;
            item->bytes = 0;
            item->next = NULL;
            buffer->tail->next = item;
            buffer->tail = item;
        } // if
    } // while

    return 1;
} // add_to_buffer


static char *flatten_buffer(Buffer *buffer, MOJOSHADER_malloc m, void *d)
{
    char *retval = (char *) m(buffer->total_bytes + 1, d);
    if (retval == NULL)
        return NULL;
    BufferList *item = &buffer->head;
    char *ptr = retval;
    while (item != NULL)
    {
        BufferList *next = item->next;
        memcpy(ptr, item->buffer, item->bytes);
        ptr += item->bytes;
        item = next;
    } // while
    *ptr = '\0';

    assert(ptr == (retval + buffer->total_bytes));
    return retval;
} // flatten_buffer


static void free_buffer(Buffer *buffer, MOJOSHADER_free f, void *d)
{
    // head is statically allocated, so start with head.next...
    BufferList *item = buffer->head.next;
    while (item != NULL)
    {
        BufferList *next = item->next;
        f(item, d);
        item = next;
    } // while
    init_buffer(buffer);
} // free_buffer


// !!! FIXME: maybe use these pool magic elsewhere?

// Pool stuff...
// ugh, I hate this macro salsa.
#define FREE_POOL(type, poolname) \
    static void free_##poolname##_pool(Context *ctx) { \
        type *item = ctx->poolname##_pool; \
        while (item != NULL) { \
            type *next = item->next; \
            Free(ctx, item); \
            item = next; \
        } \
    }

#define GET_POOL(type, poolname) \
    static type *get_##poolname(Context *ctx) { \
        type *retval = ctx->poolname##_pool; \
        if (retval != NULL) \
            ctx->poolname##_pool = retval->next; \
        else \
            retval = (type *) Malloc(ctx, sizeof (type)); \
        if (retval != NULL) \
            memset(retval, '\0', sizeof (type)); \
        return retval; \
    }

#define PUT_POOL(type, poolname) \
    static void put_##poolname(Context *ctx, type *item) { \
        item->next = ctx->poolname##_pool; \
        ctx->poolname##_pool = item; \
    }

#define IMPLEMENT_POOL(type, poolname) \
    FREE_POOL(type, poolname) \
    GET_POOL(type, poolname) \
    PUT_POOL(type, poolname)

IMPLEMENT_POOL(Conditional, conditional)
IMPLEMENT_POOL(IncludeState, include)
IMPLEMENT_POOL(Define, define)


// Preprocessor define hashtable stuff...

// !!! FIXME: why isn't this using mojoshader_common.c's code?

// this is djb's xor hashing function.
static inline uint32 hash_string_djbxor(const char *sym)
{
    register uint32 hash = 5381;
    while (*sym)
        hash = ((hash << 5) + hash) ^ *(sym++);
    return hash;
} // hash_string_djbxor

static inline uint8 hash_define(const char *sym)
{
    return (uint8) hash_string_djbxor(sym);
} // hash_define


static int add_define(Context *ctx, const char *sym, const char *val,
                      char **parameters, int paramcount)
{
    const uint8 hash = hash_define(sym);
    Define *bucket = ctx->define_hashtable[hash];
    while (bucket)
    {
        if (strcmp(bucket->identifier, sym) == 0)
        {
            failf(ctx, "'%s' already defined", sym); // !!! FIXME: warning?
            // !!! FIXME: gcc reports the location of previous #define here.
            return 0;
        } // if
        bucket = bucket->next;
    } // while

    bucket = get_define(ctx);
    if (bucket == NULL)
        return 0;

    bucket->definition = val;
    bucket->original = NULL;
    bucket->identifier = sym;
    bucket->parameters = (const char **) parameters;
    bucket->paramcount = paramcount;
    bucket->next = ctx->define_hashtable[hash];
    ctx->define_hashtable[hash] = bucket;
    return 1;
} // add_define


static void free_define(Context *ctx, Define *def)
{
    if (def != NULL)
    {
        int i;
        for (i = 0; i < def->paramcount; i++)
            Free(ctx, (void *) def->parameters[i]);
        Free(ctx, (void *) def->parameters);
        Free(ctx, (void *) def->identifier);
        Free(ctx, (void *) def->definition);
        Free(ctx, (void *) def->original);
        put_define(ctx, def);
    } // if
} // free_define


static int remove_define(Context *ctx, const char *sym)
{
    const uint8 hash = hash_define(sym);
    Define *bucket = ctx->define_hashtable[hash];
    Define *prev = NULL;
    while (bucket)
    {
        if (strcmp(bucket->identifier, sym) == 0)
        {
            if (prev == NULL)
                ctx->define_hashtable[hash] = bucket->next;
            else
                prev->next = bucket->next;
            free_define(ctx, bucket);
            return 1;
        } // if
        prev = bucket;
        bucket = bucket->next;
    } // while

    return 0;
} // remove_define


static const Define *find_define(Context *ctx, const char *sym)
{
    if ( (ctx->file_macro) && (strcmp(sym, "__FILE__") == 0) )
    {
        Free(ctx, (char *) ctx->file_macro->definition);
        const IncludeState *state = ctx->include_stack;
        const char *fname = state ? state->filename : "";
        const size_t len = strlen(fname) + 2;
        char *str = (char *) Malloc(ctx, len);
        if (!str)
            return NULL;
        str[0] = '\"';
        memcpy(str + 1, fname, len - 2);
        str[len - 1] = '\"';
        ctx->file_macro->definition = str;
        return ctx->file_macro;
    } // if

    else if ( (ctx->line_macro) && (strcmp(sym, "__LINE__") == 0) )
    {
        Free(ctx, (char *) ctx->line_macro->definition);
        const IncludeState *state = ctx->include_stack;
        const size_t bufsize = 32;
        char *str = (char *) Malloc(ctx, bufsize);
        if (!str)
            return 0;

        const size_t len = snprintf(str, bufsize, "%u", state->line);
        assert(len < bufsize);
        ctx->line_macro->definition = str;
        return ctx->line_macro;
    } // else

    const uint8 hash = hash_define(sym);
    Define *bucket = ctx->define_hashtable[hash];
    while (bucket)
    {
        if (strcmp(bucket->identifier, sym) == 0)
            return bucket;
        bucket = bucket->next;
    } // while
    return NULL;
} // find_define


static const Define *find_define_by_token(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    assert(state->tokenval == TOKEN_IDENTIFIER);
    char *sym = (char *) alloca(state->tokenlen+1);
    memcpy(sym, state->token, state->tokenlen);
    sym[state->tokenlen] = '\0';
    return find_define(ctx, sym);
} // find_define_by_token


static const Define *find_macro_arg(const IncludeState *state,
                                    const Define *defines)
{
    const Define *def = NULL;
    char *sym = (char *) alloca(state->tokenlen + 1);
    memcpy(sym, state->token, state->tokenlen);
    sym[state->tokenlen] = '\0';

    for (def = defines; def != NULL; def = def->next)
    {
        assert(def->parameters == NULL);  // args can't have args!
        assert(def->paramcount == 0);  // args can't have args!
        if (strcmp(def->identifier, sym) == 0)
            break;
    } // while

    return def;
} // find_macro_arg


static void put_all_defines(Context *ctx)
{
    size_t i;
    for (i = 0; i < STATICARRAYLEN(ctx->define_hashtable); i++)
    {
        Define *bucket = ctx->define_hashtable[i];
        ctx->define_hashtable[i] = NULL;
        while (bucket)
        {
            Define *next = bucket->next;
            free_define(ctx, bucket);
            bucket = next;
        } // while
    } // for
} // put_all_defines


static int push_source(Context *ctx, const char *fname, const char *source,
                       unsigned int srclen, unsigned int linenum,
                       MOJOSHADER_includeClose close_callback)
{
    IncludeState *state = get_include(ctx);
    if (state == NULL)
        return 0;

    if (fname != NULL)
    {
        state->filename = stringcache(ctx->filename_cache, fname);
        if (state->filename == NULL)
        {
            out_of_memory(ctx);
            put_include(ctx, state);
            return 0;
        } // if
    } // if

    state->close_callback = close_callback;
    state->source_base = source;
    state->source = source;
    state->token = source;
    state->tokenval = ((Token) '\n');
    state->orig_length = srclen;
    state->bytes_left = srclen;
    state->line = linenum;
    state->next = ctx->include_stack;
    state->asm_comments = ctx->asm_comments;

    print_debug_lexing_position(state);

    ctx->include_stack = state;

    return 1;
} // push_source


static void pop_source(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    assert(state != NULL);  // more pops than pushes!
    if (state == NULL)
        return;

    if (state->close_callback)
    {
        state->close_callback(state->source_base, ctx->malloc,
                              ctx->free, ctx->malloc_data);
    } // if

    // state->filename is a pointer to the filename cache; don't free it here!

    Conditional *cond = state->conditional_stack;
    while (cond)
    {
        Conditional *next = cond->next;
        put_conditional(ctx, cond);
        cond = next;
    } // while

    ctx->include_stack = state->next;

    print_debug_lexing_position(ctx->include_stack);

    put_include(ctx, state);
} // pop_source


static void close_define_include(const char *data, MOJOSHADER_malloc m,
                                 MOJOSHADER_free f, void *d)
{
    f((void *) data, d);
} // close_define_include


Preprocessor *preprocessor_start(const char *fname, const char *source,
                            unsigned int sourcelen,
                            MOJOSHADER_includeOpen open_callback,
                            MOJOSHADER_includeClose close_callback,
                            const MOJOSHADER_preprocessorDefine *defines,
                            unsigned int define_count, int asm_comments,
                            MOJOSHADER_malloc m, MOJOSHADER_free f, void *d)
{
    int okay = 1;
    unsigned int i = 0;

    // the preprocessor is internal-only, so we verify all these are != NULL.
    assert(m != NULL);
    assert(f != NULL);

    Context *ctx = (Context *) m(sizeof (Context), d);
    if (ctx == NULL)
        return NULL;

    memset(ctx, '\0', sizeof (Context));
    ctx->malloc = m;
    ctx->free = f;
    ctx->malloc_data = d;
    ctx->open_callback = open_callback;
    ctx->close_callback = close_callback;
    ctx->asm_comments = asm_comments;

    ctx->filename_cache = stringcache_create(m, f, d);
    okay = ((okay) && (ctx->filename_cache != NULL));

    ctx->file_macro = get_define(ctx);
    okay = ((okay) && (ctx->file_macro != NULL));
    if ((okay) && (ctx->file_macro))
        okay = ((ctx->file_macro->identifier = StrDup(ctx, "__FILE__")) != 0);

    ctx->line_macro = get_define(ctx);
    okay = ((okay) && (ctx->line_macro != NULL));
    if ((okay) && (ctx->line_macro))
        okay = ((ctx->line_macro->identifier = StrDup(ctx, "__LINE__")) != 0);

    // let the usual preprocessor parser sort these out.
    char *define_include = NULL;
    unsigned int define_include_len = 0;
    if ((okay) && (define_count > 0))
    {
        for (i = 0; i < define_count; i++)
        {
            define_include_len += strlen(defines[i].identifier);
            define_include_len += strlen(defines[i].definition);
            define_include_len += 10;  // "#define<space><space><newline>"
        } // for
        define_include_len++;  // for null terminator.

        define_include = (char *) Malloc(ctx, define_include_len);
        if (define_include == NULL)
            okay = 0;
        else
        {
            char *ptr = define_include;
            for (i = 0; i < define_count; i++)
            {
                ptr += sprintf(ptr, "#define %s %s\n", defines[i].identifier,
                               defines[i].definition);
            } // for
        } // else
    } // if

    if ((okay) && (!push_source(ctx,fname,source,sourcelen,1,NULL)))
        okay = 0;

    if ((okay) && (define_include != NULL))
    {
        okay = push_source(ctx, "<predefined macros>", define_include,
                           define_include_len, 1, close_define_include);
    } // if

    if (!okay)
    {
        preprocessor_end((Preprocessor *) ctx);
        return NULL;
    } // if

    return (Preprocessor *) ctx;
} // preprocessor_start


void preprocessor_end(Preprocessor *_ctx)
{
    Context *ctx = (Context *) _ctx;
    if (ctx == NULL)
        return;

    while (ctx->include_stack != NULL)
        pop_source(ctx);

    put_all_defines(ctx);

    if (ctx->filename_cache != NULL)
        stringcache_destroy(ctx->filename_cache);

    free_define(ctx, ctx->file_macro);
    free_define(ctx, ctx->line_macro);
    free_define_pool(ctx);
    free_conditional_pool(ctx);
    free_include_pool(ctx);

    Free(ctx, ctx);
} // preprocessor_end


int preprocessor_outofmemory(Preprocessor *_ctx)
{
    Context *ctx = (Context *) _ctx;
    return ctx->out_of_memory;
} // preprocessor_outofmemory


static inline void pushback(IncludeState *state)
{
    #if DEBUG_PREPROCESSOR
    printf("PREPROCESSOR PUSHBACK\n");
    #endif
    assert(!state->pushedback);
    state->pushedback = 1;
} // pushback


static Token lexer(IncludeState *state)
{
    if (!state->pushedback)
        return preprocessor_lexer(state);
    state->pushedback = 0;
    return state->tokenval;
} // lexer


// !!! FIXME: parsing fails on preprocessor directives should skip rest of line.
static int require_newline(IncludeState *state)
{
    const Token token = lexer(state);
    pushback(state);  // rewind no matter what.
    return ( (token == TOKEN_INCOMPLETE_COMMENT) || // call it an eol.
             (token == ((Token) '\n')) || (token == TOKEN_EOI) );
} // require_newline


static int token_to_int(IncludeState *state)
{
    assert(state->tokenval == TOKEN_INT_LITERAL);
    char *buf = (char *) alloca(state->tokenlen+1);
    memcpy(buf, state->token, state->tokenlen);
    buf[state->tokenlen] = '\0';
    return atoi(buf);
} // token_to_int


static void handle_pp_include(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    Token token = lexer(state);
    MOJOSHADER_includeType incltype;
    char *filename = NULL;
    int bogus = 0;

    if (token == TOKEN_STRING_LITERAL)
        incltype = MOJOSHADER_INCLUDETYPE_LOCAL;
    else if (token == ((Token) '<'))
    {
        incltype = MOJOSHADER_INCLUDETYPE_SYSTEM;
        // can't use lexer, since every byte between the < > pair is
        //  considered part of the filename.  :/
        while (!bogus)
        {
            if ( !(bogus = (state->bytes_left == 0)) )
            {
                const char ch = *state->source;
                if ( !(bogus = ((ch == '\r') || (ch == '\n'))) )
                {
                    state->source++;
                    state->bytes_left--;

                    if (ch == '>')
                        break;
                } // if
            } // if
        } // while
    } // else if
    else
    {
        bogus = 1;
    } // else

    if (!bogus)
    {
        state->token++;  // skip '<' or '\"'...
        const unsigned int len = ((unsigned int) (state->source-state->token));
        filename = (char *) alloca(len);
        memcpy(filename, state->token, len-1);
        filename[len-1] = '\0';
        bogus = !require_newline(state);
    } // if

    if (bogus)
    {
        fail(ctx, "Invalid #include directive");
        return;
    } // else

    const char *newdata = NULL;
    unsigned int newbytes = 0;
    if ((ctx->open_callback == NULL) || (ctx->close_callback == NULL))
    {
        fail(ctx, "Saw #include, but no include callbacks defined");
        return;
    } // if

    if (!ctx->open_callback(incltype, filename, state->source_base,
                            &newdata, &newbytes, ctx->malloc,
                            ctx->free, ctx->malloc_data))
    {
        fail(ctx, "Include callback failed");  // !!! FIXME: better error
        return;
    } // if

    MOJOSHADER_includeClose callback = ctx->close_callback;
    if (!push_source(ctx, filename, newdata, newbytes, 1, callback))
    {
        assert(ctx->out_of_memory);
        ctx->close_callback(newdata, ctx->malloc, ctx->free, ctx->malloc_data);
    } // if
} // handle_pp_include


static void handle_pp_line(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    char *filename = NULL;
    int linenum = 0;
    int bogus = 0;

    if (lexer(state) != TOKEN_INT_LITERAL)
        bogus = 1;
    else
        linenum = token_to_int(state);

    if (!bogus)
    {
        Token t = lexer(state);
        if (t == ((Token) '\n'))
        {
            state->line = linenum;
            return;
        }
        bogus = (t != TOKEN_STRING_LITERAL);
    }

    if (!bogus)
    {
        state->token++;  // skip '\"'...
        filename = (char *) alloca(state->tokenlen);
        memcpy(filename, state->token, state->tokenlen-1);
        filename[state->tokenlen-1] = '\0';
        bogus = !require_newline(state);
    } // if

    if (bogus)
    {
        fail(ctx, "Invalid #line directive");
        return;
    } // if

    const char *cached = stringcache(ctx->filename_cache, filename);
    if (cached == NULL)
        out_of_memory(ctx);
    state->filename = cached;
    state->line = linenum;
} // handle_pp_line


static void handle_pp_error(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    char *ptr = ctx->failstr;
    int avail = sizeof (ctx->failstr) - 1;
    int cpy = 0;
    int done = 0;

    const char *prefix = "#error";
    const size_t prefixlen = strlen(prefix);
    strcpy(ctx->failstr, prefix);
    avail -= prefixlen;
    ptr += prefixlen;

    state->report_whitespace = 1;
    while (!done)
    {
        const Token token = lexer(state);
        switch (token)
        {
            case ((Token) '\n'):
                state->line--;  // make sure error is on the right line.
                // fall through!
            case TOKEN_INCOMPLETE_COMMENT:
            case TOKEN_EOI:
                pushback(state);  // move back so we catch this later.
                done = 1;
                break;

            case ((Token) ' '):
                if (!avail)
                    break;
                *(ptr++) = ' ';
                avail--;
                break;

            default:
                cpy = Min(avail, (int) state->tokenlen);
                if (cpy)
                    memcpy(ptr, state->token, cpy);
                ptr += cpy;
                avail -= cpy;
                break;
        } // switch
    } // while

    *ptr = '\0';
    state->report_whitespace = 0;
    ctx->isfail = 1;
} // handle_pp_error


static void handle_pp_define(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    int done = 0;

    if (lexer(state) != TOKEN_IDENTIFIER)
    {
        fail(ctx, "Macro names must be identifiers");
        return;
    } // if

    char *definition = NULL;
    char *sym = (char *) Malloc(ctx, state->tokenlen+1);
    if (sym == NULL)
        return;
    memcpy(sym, state->token, state->tokenlen);
    sym[state->tokenlen] = '\0';

    if (strcmp(sym, "defined") == 0)
    {
        Free(ctx, sym);
        fail(ctx, "'defined' cannot be used as a macro name");
        return;
    } // if

    // Don't treat these symbols as special anymore if they get (re)#defined.
    if (strcmp(sym, "__FILE__") == 0)
    {
        if (ctx->file_macro)
        {
            failf(ctx, "'%s' already defined", sym); // !!! FIXME: warning?
            free_define(ctx, ctx->file_macro);
            ctx->file_macro = NULL;
        } // if
    } // if
    else if (strcmp(sym, "__LINE__") == 0)
    {
        if (ctx->line_macro)
        {
            failf(ctx, "'%s' already defined", sym); // !!! FIXME: warning?
            free_define(ctx, ctx->line_macro);
            ctx->line_macro = NULL;
        } // if
    } // else if

    // #define a(b) is different than #define a (b)    :(
    state->report_whitespace = 1;
    lexer(state);
    state->report_whitespace = 0;

    int params = 0;
    char **idents = NULL;
    MOJOSHADER_malloc m = ctx->malloc;
    void *d = ctx->malloc_data;
    static const char space = ' ';

    if (state->tokenval == ((Token) ' '))
        lexer(state);  // skip it.
    else if (state->tokenval == ((Token) '('))
    {
        IncludeState saved;
        memcpy(&saved, state, sizeof (IncludeState));
        while (1)
        {
            if (lexer(state) != TOKEN_IDENTIFIER)
                break;
            params++;
            if (lexer(state) != ((Token) ','))
                break;
        } // while

        if (state->tokenval != ((Token) ')'))
        {
            fail(ctx, "syntax error in macro parameter list");
            goto handle_pp_define_failed;
        } // if

        if (params == 0)  // special case for void args: "#define a() b"
            params = -1;
        else
        {
            idents = (char **) Malloc(ctx, sizeof (char *) * params);
            if (idents == NULL)
                goto handle_pp_define_failed;

            // roll all the way back, do it again.
            memcpy(state, &saved, sizeof (IncludeState));
            memset(idents, '\0', sizeof (char *) * params);

            int i;
            for (i = 0; i < params; i++)
            {
                lexer(state);
                assert(state->tokenval == TOKEN_IDENTIFIER);

                char *dst = (char *) Malloc(ctx, state->tokenlen+1);
                if (dst == NULL)
                    break;

                memcpy(dst, state->token, state->tokenlen);
                dst[state->tokenlen] = '\0';
                idents[i] = dst;

                if (i < (params-1))
                {
                    lexer(state);
                    assert(state->tokenval == ((Token) ','));
                } // if
            } // for

            if (i != params)
            {
                assert(ctx->out_of_memory);
                goto handle_pp_define_failed;
            } // if

            lexer(state);
            assert(state->tokenval == ((Token) ')'));
        } // else

        lexer(state);
    } // else if

    pushback(state);

    Buffer buffer;
    init_buffer(&buffer);

    state->report_whitespace = 1;
    while ((!done) && (!ctx->out_of_memory))
    {
        const Token token = lexer(state);
        switch (token)
        {
            case TOKEN_INCOMPLETE_COMMENT:
            case TOKEN_EOI:
                pushback(state);  // move back so we catch this later.
                done = 1;
                break;

            case ((Token) '\n'):
                done = 1;
                break;

            case ((Token) ' '):  // may not actually point to ' '.
                assert(buffer.total_bytes > 0);
                if (!add_to_buffer(&buffer, &space, 1, m, d))
                    out_of_memory(ctx);
                break;

            default:
                if (!add_to_buffer(&buffer,state->token,state->tokenlen,m,d))
                    out_of_memory(ctx);
                break;
        } // switch
    } // while
    state->report_whitespace = 0;

    if (!ctx->out_of_memory)
    {
        definition = flatten_buffer(&buffer, m, d);
        if (definition == NULL)
            out_of_memory(ctx);
    } // if

    size_t buflen = buffer.total_bytes + 1;
    free_buffer(&buffer, ctx->free, d);

    if (ctx->out_of_memory)
        goto handle_pp_define_failed;

    int hashhash_error = 0;
    if ((buflen > 2) && (definition[0] == '#') && (definition[1] == '#'))
    {
        hashhash_error = 1;
        buflen -= 2;
        memmove(definition, definition + 2, buflen);
    } // if

    if (buflen > 2)
    {
        char *ptr = (definition + buflen) - 2;
        if (*ptr == ' ')
        {
            ptr--;
            buflen--;
        } // if
        if ((buflen > 2) && (ptr[0] == '#') && (ptr[-1] == '#'))
        {
            hashhash_error = 1;
            buflen -= 2;
            ptr[-1] = '\0';
        } // if
    } // if

    if (hashhash_error)
        fail(ctx, "'##' cannot appear at either end of a macro expansion");

    assert(done);

    if (!add_define(ctx, sym, definition, idents, params))
        goto handle_pp_define_failed;

    return;

handle_pp_define_failed:
    Free(ctx, sym);
    Free(ctx, definition);
    while (params--)
        Free(ctx, idents[params]);
    Free(ctx, idents);
} // handle_pp_define


static void handle_pp_undef(Context *ctx)
{
    IncludeState *state = ctx->include_stack;

    if (lexer(state) != TOKEN_IDENTIFIER)
    {
        fail(ctx, "Macro names must be indentifiers");
        return;
    } // if

    char *sym = (char *) alloca(state->tokenlen+1);
    memcpy(sym, state->token, state->tokenlen);
    sym[state->tokenlen] = '\0';

    if (!require_newline(state))
    {
        fail(ctx, "Invalid #undef directive");
        return;
    } // if

    if (strcmp(sym, "__FILE__") == 0)
    {
        if (ctx->file_macro)
        {
            failf(ctx, "undefining \"%s\"", sym);  // !!! FIXME: should be warning.
            free_define(ctx, ctx->file_macro);
            ctx->file_macro = NULL;
        } // if
    } // if
    else if (strcmp(sym, "__LINE__") == 0)
    {
        if (ctx->line_macro)
        {
            failf(ctx, "undefining \"%s\"", sym);  // !!! FIXME: should be warning.
            free_define(ctx, ctx->line_macro);
            ctx->line_macro = NULL;
        } // if
    } // if

    remove_define(ctx, sym);
} // handle_pp_undef


static Conditional *_handle_pp_ifdef(Context *ctx, const Token type)
{
    IncludeState *state = ctx->include_stack;

    assert((type == TOKEN_PP_IFDEF) || (type == TOKEN_PP_IFNDEF));

    if (lexer(state) != TOKEN_IDENTIFIER)
    {
        fail(ctx, "Macro names must be indentifiers");
        return NULL;
    } // if

    char *sym = (char *) alloca(state->tokenlen+1);
    memcpy(sym, state->token, state->tokenlen);
    sym[state->tokenlen] = '\0';

    if (!require_newline(state))
    {
        if (type == TOKEN_PP_IFDEF)
            fail(ctx, "Invalid #ifdef directive");
        else
            fail(ctx, "Invalid #ifndef directive");
        return NULL;
    } // if

    Conditional *conditional = get_conditional(ctx);
    assert((conditional != NULL) || (ctx->out_of_memory));
    if (conditional == NULL)
        return NULL;

    Conditional *parent = state->conditional_stack;
    const int found = (find_define(ctx, sym) != NULL);
    const int chosen = (type == TOKEN_PP_IFDEF) ? found : !found;
    const int skipping = ( (((parent) && (parent->skipping))) || (!chosen) );

    conditional->type = type;
    conditional->linenum = state->line - 1;
    conditional->skipping = skipping;
    conditional->chosen = chosen;
    conditional->next = parent;
    state->conditional_stack = conditional;
    return conditional;
} // _handle_pp_ifdef


static inline void handle_pp_ifdef(Context *ctx)
{
    _handle_pp_ifdef(ctx, TOKEN_PP_IFDEF);
} // handle_pp_ifdef


static inline void handle_pp_ifndef(Context *ctx)
{
    _handle_pp_ifdef(ctx, TOKEN_PP_IFNDEF);
} // handle_pp_ifndef


static int replace_and_push_macro(Context *ctx, const Define *def,
                                  const Define *params)
{
    char *final = NULL;
    MOJOSHADER_malloc m = ctx->malloc;
    MOJOSHADER_free f = ctx->free;
    void *d = ctx->malloc_data;

    // We push the #define and lex it, building a buffer with argument
    //  replacement, stringification, and concatenation.
    IncludeState *state = ctx->include_stack;
    if (!push_source(ctx, state->filename, def->definition,
                     strlen(def->definition), state->line, NULL))
        return 0;

    Buffer buffer;
    init_buffer(&buffer);

    state = ctx->include_stack;
    while (lexer(state) != TOKEN_EOI)
    {
        int wantorig = 0;
        const Define *arg = NULL;

        // put a space between tokens if we're not concatenating.
        if (state->tokenval == TOKEN_HASHHASH)  // concatenate?
        {
            wantorig = 1;
            lexer(state);
            assert(state->tokenval != TOKEN_EOI);
        } // if
        else
        {
            if (buffer.total_bytes > 0)
            {
                if (!add_to_buffer(&buffer, " ", 1, m, d))
                    goto replace_and_push_macro_failed;
            } // if
        } // else

        const char *data = state->token;
        unsigned int len = state->tokenlen;

        if (state->tokenval == TOKEN_HASH)  // stringify?
        {
            lexer(state);
            assert(state->tokenval != TOKEN_EOI);  // we checked for this.

            if (!add_to_buffer(&buffer, "\"", 1, m, d))
                goto replace_and_push_macro_failed;

            if (state->tokenval == TOKEN_IDENTIFIER)
            {
                arg = find_macro_arg(state, params);
                if (arg != NULL)
                {
                    data = arg->original;
                    len = strlen(data);
                } // if
            } // if

            if (!add_to_buffer(&buffer, data, len, m, d))
                goto replace_and_push_macro_failed;

            if (!add_to_buffer(&buffer, "\"", 1, m, d))
                goto replace_and_push_macro_failed;

            continue;
        } // if

        if (state->tokenval == TOKEN_IDENTIFIER)
        {
            arg = find_macro_arg(state, params);
            if (arg != NULL)
            {
                if (!wantorig)
                {
                    wantorig = (lexer(state) == TOKEN_HASHHASH);
                    pushback(state);
                } // if
                data = wantorig ? arg->original : arg->definition;
                len = strlen(data);
            } // if
        } // if

        if (!add_to_buffer(&buffer, data, len, m, d))
            goto replace_and_push_macro_failed;
    } // while

    final = flatten_buffer(&buffer, m, d);
    if (!final)
    {
        out_of_memory(ctx);
        goto replace_and_push_macro_failed;
    } // if

    free_buffer(&buffer, f, d);
    pop_source(ctx);  // ditch the macro.
    state = ctx->include_stack;
    if (!push_source(ctx, state->filename, final, strlen(final), state->line,
                     close_define_include))
    {
        Free(ctx, final);
        return 0;
    } // if

    return 1;

replace_and_push_macro_failed:
    pop_source(ctx);
    free_buffer(&buffer, f, d);
    return 0;
} // replace_and_push_macro


static int handle_macro_args(Context *ctx, const char *sym, const Define *def)
{
    int retval = 0;
    MOJOSHADER_malloc m = ctx->malloc;
    MOJOSHADER_free f = ctx->free;
    void *d = ctx->malloc_data;
    IncludeState *state = ctx->include_stack;
    Define *params = NULL;
    const int expected = (def->paramcount < 0) ? 0 : def->paramcount;
    int saw_params = 0;
    IncludeState saved;  // can't pushback, we need the original token.
    memcpy(&saved, state, sizeof (IncludeState));
    if (lexer(state) != ((Token) '('))
    {
        memcpy(state, &saved, sizeof (IncludeState));
        goto handle_macro_args_failed;  // gcc abandons replacement, too.
    } // if

    state->report_whitespace = 1;

    int void_call = 0;
    int paren = 1;
    while (paren > 0)
    {
        Buffer buffer;
        Buffer origbuffer;
        init_buffer(&buffer);
        init_buffer(&origbuffer);

        Token t = lexer(state);

        assert(!void_call);

        while (1)
        {
            const char *origexpr = state->token;
            unsigned int origexprlen = state->tokenlen;
            const char *expr = state->token;
            unsigned int exprlen = state->tokenlen;

            if (t == ((Token) '('))
                paren++;

            else if (t == ((Token) ')'))
            {
                paren--;
                if (paren < 1)  // end of macro?
                    break;
            } // else if

            else if (t == ((Token) ','))
            {
                if (paren == 1)  // new macro arg?
                    break;
            } // else if

            else if (t == ((Token) ' '))
            {
                // don't add whitespace to the start, so we recognize
                //  void calls correctly.
                origexpr = expr = " ";
                origexprlen = (buffer.total_bytes == 0) ? 0 : 1;
            } // else if

            else if (t == TOKEN_IDENTIFIER)
            {
                const Define *def = find_define_by_token(ctx);
                // don't replace macros with arguments so they replace correctly, later.
                if ((def) && (def->paramcount == 0))
                {
                    expr = def->definition;
                    exprlen = strlen(def->definition);
                } // if
            } // else if

            else if ((t == TOKEN_INCOMPLETE_COMMENT) || (t == TOKEN_EOI))
            {
                pushback(state);
                fail(ctx, "Unterminated macro list");
                goto handle_macro_args_failed;
            } // else if

            assert(expr != NULL);

            if (!add_to_buffer(&buffer, expr, exprlen, m, d))
            {
                out_of_memory(ctx);
                goto handle_macro_args_failed;
            } // if

            if (!add_to_buffer(&origbuffer, origexpr, origexprlen, m, d))
            {
                out_of_memory(ctx);
                goto handle_macro_args_failed;
            } // if

            t = lexer(state);
        } // while

        if (buffer.total_bytes == 0)
            void_call = ((saw_params == 0) && (paren == 0));

        if (saw_params < expected)
        {
            char *origdefinition = flatten_buffer(&origbuffer, m, d);
            char *definition = flatten_buffer(&buffer, m, d);
            Define *p = get_define(ctx);
            if ((!origdefinition) || (!definition) || (!p))
            {
                Free(ctx, origdefinition);
                Free(ctx, definition);
                free_buffer(&origbuffer, f, d);
                free_buffer(&buffer, f, d);
                free_define(ctx, p);
                goto handle_macro_args_failed;
            } // if

            // trim any whitespace from the end of the string...
            int i;
            for (i = (int) buffer.total_bytes - 1; i >= 0; i--)
            {
                if (definition[i] == ' ')
                    definition[i] = '\0';
                else
                    break;
            } // for

            for (i = (int) origbuffer.total_bytes - 1; i >= 0; i--)
            {
                if (origdefinition[i] == ' ')
                    origdefinition[i] = '\0';
                else
                    break;
            } // for

            p->identifier = def->parameters[saw_params];
            p->definition = definition;
            p->original = origdefinition;
            p->next = params;
            params = p;
        } // if

        free_buffer(&buffer, f, d);
        free_buffer(&origbuffer, f, d);
        saw_params++;
    } // while

    assert(paren == 0);

    // "a()" should match "#define a()" ...
    if ((expected == 0) && (saw_params == 1) && (void_call))
    {
        assert(params == NULL);
        saw_params = 0;
    } // if

    if (saw_params != expected)
    {
        failf(ctx, "macro '%s' passed %d arguments, but requires %d",
              sym, saw_params, expected);
        goto handle_macro_args_failed;
    } // if

    // this handles arg replacement and the '##' and '#' operators.
    retval = replace_and_push_macro(ctx, def, params);

handle_macro_args_failed:
    while (params)
    {
        Define *next = params->next;
        params->identifier = NULL;
        free_define(ctx, params);
        params = next;
    } // while

    state->report_whitespace = 0;
    return retval;
} // handle_macro_args


static int handle_pp_identifier(Context *ctx)
{
    if (ctx->recursion_count++ >= 256)  // !!! FIXME: gcc can figure this out.
    {
        fail(ctx, "Recursing macros");
        return 0;
    } // if

    IncludeState *state = ctx->include_stack;
    const char *fname = state->filename;
    const unsigned int line = state->line;
    char *sym = (char *) alloca(state->tokenlen+1);
    memcpy(sym, state->token, state->tokenlen);
    sym[state->tokenlen] = '\0';

    // Is this identifier #defined?
    const Define *def = find_define(ctx, sym);
    if (def == NULL)
        return 0;   // just send the token through unchanged.
    else if (def->paramcount != 0)
        return handle_macro_args(ctx, sym, def);

    const size_t deflen = strlen(def->definition);
    return push_source(ctx, fname, def->definition, deflen, line, NULL);
} // handle_pp_identifier


static int find_precedence(const Token token)
{
    // operator precedence, left and right associative...
    typedef struct { int precedence; Token token; } Precedence;
    static const Precedence ops[] = {
        { 0, TOKEN_OROR }, { 1, TOKEN_ANDAND }, { 2, ((Token) '|') },
        { 3, ((Token) '^') }, { 4, ((Token) '&') }, { 5, TOKEN_NEQ },
        { 6, TOKEN_EQL }, { 7, ((Token) '<') }, { 7, ((Token) '>') },
        { 7, TOKEN_LEQ }, { 7, TOKEN_GEQ }, { 8, TOKEN_LSHIFT },
        { 8, TOKEN_RSHIFT }, { 9, ((Token) '-') }, { 9, ((Token) '+') },
        { 10, ((Token) '%') }, { 10, ((Token) '/') }, { 10, ((Token) '*') },
        { 11, TOKEN_PP_UNARY_PLUS }, { 11, TOKEN_PP_UNARY_MINUS },
        { 11, ((Token) '!') }, { 11, ((Token) '~') },
    };

    size_t i;
    for (i = 0; i < STATICARRAYLEN(ops); i++)
    {
        if (ops[i].token == token)
            return ops[i].precedence;
    } // for

    return -1;
} // find_precedence

// !!! FIXME: we're using way too much stack space here...
typedef struct RpnTokens
{
    int isoperator;
    int value;
} RpnTokens;

static long interpret_rpn(const RpnTokens *tokens, int tokencount, int *error)
{
    long stack[128];
    size_t stacksize = 0;

    *error = 1;

    #define NEED_X_TOKENS(x) do { if (stacksize < x) return 0; } while (0)

    #define BINARY_OPERATION(op) do { \
        NEED_X_TOKENS(2); \
        stack[stacksize-2] = stack[stacksize-2] op stack[stacksize-1]; \
        stacksize--; \
    } while (0)

    #define UNARY_OPERATION(op) do { \
        NEED_X_TOKENS(1); \
        stack[stacksize-1] = op stack[stacksize-1]; \
    } while (0)

    while (tokencount-- > 0)
    {
        if (!tokens->isoperator)
        {
            assert(stacksize < STATICARRAYLEN(stack));
            stack[stacksize++] = (long) tokens->value;
            tokens++;
            continue;
        } // if

        // operators.
        switch (tokens->value)
        {
            case '!': UNARY_OPERATION(!); break;
            case '~': UNARY_OPERATION(~); break;
            case TOKEN_PP_UNARY_MINUS: UNARY_OPERATION(-); break;
            case TOKEN_PP_UNARY_PLUS: UNARY_OPERATION(+); break;
            case TOKEN_OROR: BINARY_OPERATION(||); break;
            case TOKEN_ANDAND: BINARY_OPERATION(&&); break;
            case '|': BINARY_OPERATION(|); break;
            case '^': BINARY_OPERATION(^); break;
            case '&': BINARY_OPERATION(&); break;
            case TOKEN_NEQ: BINARY_OPERATION(!=); break;
            case TOKEN_EQL: BINARY_OPERATION(==); break;
            case '<': BINARY_OPERATION(<); break;
            case '>': BINARY_OPERATION(>); break;
            case TOKEN_LEQ: BINARY_OPERATION(<=); break;
            case TOKEN_GEQ: BINARY_OPERATION(>=); break;
            case TOKEN_LSHIFT: BINARY_OPERATION(<<); break;
            case TOKEN_RSHIFT: BINARY_OPERATION(>>); break;
            case '-': BINARY_OPERATION(-); break;
            case '+': BINARY_OPERATION(+); break;
            case '%': BINARY_OPERATION(%); break;
            case '/': BINARY_OPERATION(/); break;
            case '*': BINARY_OPERATION(*); break;
            default: return 0;
        } // switch

        tokens++;
    } // while

    #undef NEED_X_TOKENS
    #undef BINARY_OPERATION
    #undef UNARY_OPERATION

    if (stacksize != 1)
        return 0;

    *error = 0;
    return stack[0];
} // interpret_rpn

// http://en.wikipedia.org/wiki/Shunting_yard_algorithm
//  Convert from infix to postfix, then use this for constant folding.
//  Everything that parses should fold down to a constant value: any
//  identifiers that aren't resolved as macros become zero. Anything we
//  don't explicitly expect becomes a parsing error.
// returns 1 (true), 0 (false), or -1 (error)
static int reduce_pp_expression(Context *ctx)
{
    RpnTokens output[128];
    Token stack[64];
    Token previous_token = TOKEN_UNKNOWN;
    size_t outputsize = 0;
    size_t stacksize = 0;
    int matched = 0;
    int done = 0;

    #define ADD_TO_OUTPUT(op, val) \
        assert(outputsize < STATICARRAYLEN(output)); \
        output[outputsize].isoperator = op; \
        output[outputsize].value = val; \
        outputsize++;

    #define PUSH_TO_STACK(t) \
        assert(stacksize < STATICARRAYLEN(stack)); \
        stack[stacksize] = t; \
        stacksize++;

    while (!done)
    {
        IncludeState *state = ctx->include_stack;
        Token token = lexer(state);
        int isleft = 1;
        int precedence = -1;

        if ( (token == ((Token) '!')) || (token == ((Token) '~')) )
            isleft = 0;
        else if (token == ((Token) '-'))
        {
            if ((isleft = (previous_token == TOKEN_INT_LITERAL)) == 0)
                token = TOKEN_PP_UNARY_MINUS;
        } // else if
        else if (token == ((Token) '+'))
        {
            if ((isleft = (previous_token == TOKEN_INT_LITERAL)) == 0)
                token = TOKEN_PP_UNARY_PLUS;
        } // else if

        if (token != TOKEN_IDENTIFIER)
            ctx->recursion_count = 0;

        switch (token)
        {
            case TOKEN_EOI:
            case ((Token) '\n'):
                done = 1;
                break;  // we're done!

            case TOKEN_IDENTIFIER:
                if (handle_pp_identifier(ctx))
                    continue;  // go again with new IncludeState.

                if ( (state->tokenlen == 7) &&
                     (memcmp(state->token, "defined", 7) == 0) )
                {
                    token = lexer(state);
                    const int paren = (token == ((Token) '('));
                    if (paren)  // gcc doesn't let us nest parens here, either.
                        token = lexer(state);
                    if (token != TOKEN_IDENTIFIER)
                    {
                        fail(ctx, "operator 'defined' requires an identifier");
                        return -1;
                    } // if
                    const int found = (find_define_by_token(ctx) != NULL);

                    if (paren)
                    {
                        if (lexer(state) != ((Token) ')'))
                        {
                            fail(ctx, "Unmatched ')'");
                            return -1;
                        } // if
                    } // if

                    ADD_TO_OUTPUT(0, found);
                    continue;
                } // if

                // can't replace identifier with a number? It becomes zero.
                token = TOKEN_INT_LITERAL;
                ADD_TO_OUTPUT(0, 0);
                break;

            case TOKEN_INT_LITERAL:
                ADD_TO_OUTPUT(0, token_to_int(state));
                break;

            case ((Token) '('):
                PUSH_TO_STACK((Token) '(');
                break;

            case ((Token) ')'):
                matched = 0;
                while (stacksize > 0)
                {
                    const Token t = stack[--stacksize];
                    if (t == ((Token) '('))
                    {
                        matched = 1;
                        break;
                    } // if
                    ADD_TO_OUTPUT(1, t);
                } // while

                if (!matched)
                {
                    fail(ctx, "Unmatched ')'");
                    return -1;
                } // if
                break;

            default:
                precedence = find_precedence(token);
                // bogus token, or two operators together.
                if (precedence < 0)
                {
                    pushback(state);
                    fail(ctx, "Invalid expression");
                    return -1;
                } // if

                else  // it's an operator.
                {
                    while (stacksize > 0)
                    {
                        const Token t = stack[stacksize-1];
                        const int p = find_precedence(t);
                        if ( (p >= 0) &&
                             ( ((isleft) && (precedence <= p)) ||
                               ((!isleft) && (precedence < p)) ) )
                        {
                            stacksize--;
                            ADD_TO_OUTPUT(1, t);
                        } // if
                        else
                        {
                            break;
                        } // else
                    } // while
                    PUSH_TO_STACK(token);
                } // else
                break;
        } // switch
        previous_token = token;
    } // while

    while (stacksize > 0)
    {
        const Token t = stack[--stacksize];
        if (t == ((Token) '('))
        {
            fail(ctx, "Unmatched ')'");
            return -1;
        } // if
        ADD_TO_OUTPUT(1, t);
    } // while

    #undef ADD_TO_OUTPUT
    #undef PUSH_TO_STACK

    // okay, you now have some validated data in reverse polish notation.
    #if DEBUG_PREPROCESSOR
    printf("PREPROCESSOR EXPRESSION RPN:");
    int i = 0;
    for (i = 0; i < outputsize; i++)
    {
        if (!output[i].isoperator)
            printf(" %d", output[i].value);
        else
        {
            switch (output[i].value)
            {
                case TOKEN_OROR: printf(" ||"); break;
                case TOKEN_ANDAND: printf(" &&"); break;
                case TOKEN_NEQ: printf(" !="); break;
                case TOKEN_EQL: printf(" =="); break;
                case TOKEN_LEQ: printf(" <="); break;
                case TOKEN_GEQ: printf(" >="); break;
                case TOKEN_LSHIFT: printf(" <<"); break;
                case TOKEN_RSHIFT: printf(" >>"); break;
                case TOKEN_PP_UNARY_PLUS: printf(" +"); break;
                case TOKEN_PP_UNARY_MINUS: printf(" -"); break;
                default: printf(" %c", output[i].value); break;
            } // switch
        } // else
    } // for
    printf("\n");
    #endif

    int error = 0;
    const long val = interpret_rpn(output, outputsize, &error);

    #if DEBUG_PREPROCESSOR
    printf("PREPROCESSOR RPN RESULT: %ld%s\n", val, error ? " (ERROR)" : "");
    #endif

    if (error)
    {
        fail(ctx, "Invalid expression");
        return -1;
    } // if

    return ((val) ? 1 : 0);
} // reduce_pp_expression


static Conditional *handle_pp_if(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    const int result = reduce_pp_expression(ctx);
    if (result == -1)
        return NULL;

    Conditional *conditional = get_conditional(ctx);
    assert((conditional != NULL) || (ctx->out_of_memory));
    if (conditional == NULL)
        return NULL;

    Conditional *parent = state->conditional_stack;
    const int chosen = result;
    const int skipping = ( (((parent) && (parent->skipping))) || (!chosen) );

    conditional->type = TOKEN_PP_IF;
    conditional->linenum = state->line - 1;
    conditional->skipping = skipping;
    conditional->chosen = chosen;
    conditional->next = parent;
    state->conditional_stack = conditional;
    return conditional;
} // handle_pp_if


static void handle_pp_elif(Context *ctx)
{
    const int rc = reduce_pp_expression(ctx);
    if (rc == -1)
        return;

    IncludeState *state = ctx->include_stack;
    Conditional *cond = state->conditional_stack;
    if (cond == NULL)
        fail(ctx, "#elif without #if");
    else if (cond->type == TOKEN_PP_ELSE)
        fail(ctx, "#elif after #else");
    else
    {
        const Conditional *parent = cond->next;
        cond->type = TOKEN_PP_ELIF;
        cond->skipping = (parent && parent->skipping) || cond->chosen || !rc;
        if (!cond->chosen)
            cond->chosen = rc;
    } // else
} // handle_pp_elif


static void handle_pp_else(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    Conditional *cond = state->conditional_stack;

    if (!require_newline(state))
        fail(ctx, "Invalid #else directive");
    else if (cond == NULL)
        fail(ctx, "#else without #if");
    else if (cond->type == TOKEN_PP_ELSE)
        fail(ctx, "#else after #else");
    else
    {
        const Conditional *parent = cond->next;
        cond->type = TOKEN_PP_ELSE;
        cond->skipping = (parent && parent->skipping) || cond->chosen;
        if (!cond->chosen)
            cond->chosen = 1;
    } // else
} // handle_pp_else


static void handle_pp_endif(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    Conditional *cond = state->conditional_stack;

    if (!require_newline(state))
        fail(ctx, "Invalid #endif directive");
    else if (cond == NULL)
        fail(ctx, "Unmatched #endif");
    else
    {
        state->conditional_stack = cond->next;  // pop it.
        put_conditional(ctx, cond);
    } // else
} // handle_pp_endif


static void unterminated_pp_condition(Context *ctx)
{
    IncludeState *state = ctx->include_stack;
    Conditional *cond = state->conditional_stack;

    // !!! FIXME: report the line number where the #if is, not the EOI.
    switch (cond->type)
    {
        case TOKEN_PP_IF: fail(ctx, "Unterminated #if"); break;
        case TOKEN_PP_IFDEF: fail(ctx, "Unterminated #ifdef"); break;
        case TOKEN_PP_IFNDEF: fail(ctx, "Unterminated #ifndef"); break;
        case TOKEN_PP_ELSE: fail(ctx, "Unterminated #else"); break;
        case TOKEN_PP_ELIF: fail(ctx, "Unterminated #elif"); break;
        default: assert(0 && "Shouldn't hit this case"); break;
    } // switch

    // pop this conditional, we'll report the next error next time...

    state->conditional_stack = cond->next;  // pop it.
    put_conditional(ctx, cond);
} // unterminated_pp_condition


static inline const char *_preprocessor_nexttoken(Preprocessor *_ctx,
                                             unsigned int *_len, Token *_token)
{
    Context *ctx = (Context *) _ctx;

    while (1)
    {
        if (ctx->isfail)
        {
            ctx->isfail = 0;
            *_token = TOKEN_PREPROCESSING_ERROR;
            *_len = strlen(ctx->failstr);
            return ctx->failstr;
        } // if

        IncludeState *state = ctx->include_stack;
        if (state == NULL)
        {
            *_token = TOKEN_EOI;
            *_len = 0;
            return NULL;  // we're done!
        } // if

        const Conditional *cond = state->conditional_stack;
        const int skipping = ((cond != NULL) && (cond->skipping));

        const Token token = lexer(state);

        if (token != TOKEN_IDENTIFIER)
            ctx->recursion_count = 0;

        if (token == TOKEN_EOI)
        {
            assert(state->bytes_left == 0);
            if (state->conditional_stack != NULL)
            {
                unterminated_pp_condition(ctx);
                continue;  // returns an error.
            } // if

            pop_source(ctx);
            continue;  // pick up again after parent's #include line.
        } // if

        else if (token == TOKEN_INCOMPLETE_COMMENT)
        {
            fail(ctx, "Incomplete multiline comment");
            continue;  // will return at top of loop.
        } // else if

        else if (token == TOKEN_PP_IFDEF)
        {
            handle_pp_ifdef(ctx);
            continue;  // get the next thing.
        } // else if

        else if (token == TOKEN_PP_IFNDEF)
        {
            handle_pp_ifndef(ctx);
            continue;  // get the next thing.
        } // else if

        else if (token == TOKEN_PP_IF)
        {
            handle_pp_if(ctx);
            continue;  // get the next thing.
        } // else if

        else if (token == TOKEN_PP_ELIF)
        {
            handle_pp_elif(ctx);
            continue;  // get the next thing.
        } // else if

        else if (token == TOKEN_PP_ENDIF)
        {
            handle_pp_endif(ctx);
            continue;  // get the next thing.
        } // else if

        else if (token == TOKEN_PP_ELSE)
        {
            handle_pp_else(ctx);
            continue;  // get the next thing.
        } // else if

        // NOTE: Conditionals must be above (skipping) test.
        else if (skipping)
            continue;  // just keep dumping tokens until we get end of block.

        else if (token == TOKEN_PP_INCLUDE)
        {
            handle_pp_include(ctx);
            continue;  // will return error or use new top of include_stack.
        } // else if

        else if (token == TOKEN_PP_LINE)
        {
            handle_pp_line(ctx);
            continue;  // get the next thing.
        } // else if

        else if (token == TOKEN_PP_ERROR)
        {
            handle_pp_error(ctx);
            continue;  // will return at top of loop.
        } // else if

        else if (token == TOKEN_PP_DEFINE)
        {
            handle_pp_define(ctx);
            continue;  // will return at top of loop.
        } // else if

        else if (token == TOKEN_PP_UNDEF)
        {
            handle_pp_undef(ctx);
            continue;  // will return at top of loop.
        } // else if

        else if (token == TOKEN_PP_PRAGMA)
        {
            ctx->parsing_pragma = 1;
        } // else if

        if (token == TOKEN_IDENTIFIER)
        {
            if (handle_pp_identifier(ctx))
                continue;  // pushed the include_stack.
        } // else if

        else if (token == ((Token) '\n'))
        {
            print_debug_lexing_position(state);
            if (ctx->parsing_pragma)  // let this one through.
                ctx->parsing_pragma = 0;
            else
            {
                // preprocessor is line-oriented, nothing else gets newlines.
                continue;  // get the next thing.
            } // else
        } // else if

        assert(!skipping);
        *_token = token;
        *_len = state->tokenlen;
        return state->token;
    } // while

    assert(0 && "shouldn't hit this code");
    *_token = TOKEN_UNKNOWN;
    *_len = 0;
    return NULL;
} // _preprocessor_nexttoken


const char *preprocessor_nexttoken(Preprocessor *ctx, unsigned int *len,
                                   Token *token)
{
    const char *retval = _preprocessor_nexttoken(ctx, len, token);
    print_debug_token(retval, *len, *token);
    return retval;
} // preprocessor_nexttoken


const char *preprocessor_sourcepos(Preprocessor *_ctx, unsigned int *pos)
{
    Context *ctx = (Context *) _ctx;
    if (ctx->include_stack == NULL)
    {
        *pos = 0;
        return NULL;
    } // if

    *pos = ctx->include_stack->line;
    return ctx->include_stack->filename;
} // preprocessor_sourcepos


static int indent_buffer(Buffer *buffer, int n, int newline,
                         MOJOSHADER_malloc m, void *d)
{
    static char spaces[4] = { ' ', ' ', ' ', ' ' };
    if (newline)
    {
        while (n--)
        {
            if (!add_to_buffer(buffer, spaces, sizeof (spaces), m, d))
                return 0;
        } // while
    } // if
    else
    {
        if (!add_to_buffer(buffer, spaces, 1, m, d))
            return 0;
    } // else
    return 1;
} // indent_buffer


static const MOJOSHADER_preprocessData out_of_mem_data_preprocessor = {
    1, &MOJOSHADER_out_of_mem_error, 0, 0, 0, 0, 0
};


// public API...

const MOJOSHADER_preprocessData *MOJOSHADER_preprocess(const char *filename,
                             const char *source, unsigned int sourcelen,
                             const MOJOSHADER_preprocessorDefine *defines,
                             unsigned int define_count,
                             MOJOSHADER_includeOpen include_open,
                             MOJOSHADER_includeClose include_close,
                             MOJOSHADER_malloc m, MOJOSHADER_free f, void *d)
{
    #ifdef _WINDOWS
    static const char endline[] = { '\r', '\n' };
    #else
    static const char endline[] = { '\n' };
    #endif

    if (!m) m = MOJOSHADER_internal_malloc;
    if (!f) f = MOJOSHADER_internal_free;
    if (!include_open) include_open = MOJOSHADER_internal_include_open;
    if (!include_close) include_close = MOJOSHADER_internal_include_close;

    ErrorList *errors = errorlist_create(m, f, d);
    if (errors == NULL)
        return &out_of_mem_data_preprocessor;

    Preprocessor *pp = preprocessor_start(filename, source, sourcelen,
                                          include_open, include_close,
                                          defines, define_count, 0, m, f, d);

    if (pp == NULL)
    {
        errorlist_destroy(errors);
        return &out_of_mem_data_preprocessor;
    } // if

    Token token = TOKEN_UNKNOWN;
    const char *tokstr = NULL;

    Buffer buffer;
    init_buffer(&buffer);

    int nl = 1;
    int indent = 0;
    unsigned int len = 0;
    int out_of_memory = 0;
    while ((tokstr = preprocessor_nexttoken(pp, &len, &token)) != NULL)
    {
        int isnewline = 0;

        assert(token != TOKEN_EOI);

        if (!out_of_memory)
            out_of_memory = preprocessor_outofmemory(pp);

        // Microsoft's preprocessor is weird.
        // It ignores newlines, and then inserts its own around certain
        //  tokens. For example, after a semicolon. This allows HLSL code to
        //  be mostly readable, instead of a stream of tokens.
        if ( (token == ((Token) '}')) || (token == ((Token) ';')) )
        {
            if (!out_of_memory)
            {
                if ( (token == ((Token) '}')) && (indent > 0) )
                    indent--;

                out_of_memory =
                    (!indent_buffer(&buffer, indent, nl, m, d)) ||
                    (!add_to_buffer(&buffer, tokstr, len, m, d)) ||
                    (!add_to_buffer(&buffer, endline, sizeof (endline), m, d));

                isnewline = 1;
            } // if
        } // if

        else if (token == ((Token) '\n'))
        {
            if (!out_of_memory)
            {
                out_of_memory =
                    (!add_to_buffer(&buffer, endline, sizeof (endline), m, d));
            } // if
            isnewline = 1;
        } // else if

        else if (token == ((Token) '{'))
        {
            if (!out_of_memory)
            {
                out_of_memory =
                    (!add_to_buffer(&buffer,endline,sizeof (endline),m,d)) ||
                    (!indent_buffer(&buffer, indent, 1, m, d)) ||
                    (!add_to_buffer(&buffer, "{", 1, m, d)) ||
                    (!add_to_buffer(&buffer,endline,sizeof (endline),m,d));
                indent++;
                isnewline = 1;
            } // if
        } // else if

        else if (token == TOKEN_PREPROCESSING_ERROR)
        {
            if (!out_of_memory)
            {
                unsigned int pos = 0;
                const char *fname = preprocessor_sourcepos(pp, &pos);
                if (!errorlist_add(errors, fname, (int) pos, tokstr))
                    out_of_memory = 1;
            } // if
        } // else if

        else
        {
            if (!out_of_memory)
            {
                out_of_memory = (!indent_buffer(&buffer, indent, nl, m, d)) ||
                                (!add_to_buffer(&buffer, tokstr, len, m, d));

            } // if
        } // else

        nl = isnewline;

        if (out_of_memory)
        {
            preprocessor_end(pp);
            free_buffer(&buffer, f, d);
            errorlist_destroy(errors);
            return &out_of_mem_data_preprocessor;
        } // if
    } // while
    
    assert((token == TOKEN_EOI) || (out_of_memory));

    preprocessor_end(pp);

    const size_t total_bytes = buffer.total_bytes;
    char *output = flatten_buffer(&buffer, m, d);
    free_buffer(&buffer, f, d);
    if (output == NULL)
    {
        errorlist_destroy(errors);
        return &out_of_mem_data_preprocessor;
    } // if

    MOJOSHADER_preprocessData *retval = (MOJOSHADER_preprocessData *)
                                    m(sizeof (MOJOSHADER_preprocessData), d);
    if (retval == NULL)
    {
        errorlist_destroy(errors);
        f(output, d);
        return &out_of_mem_data_preprocessor;
    } // if

    memset(retval, '\0', sizeof (*retval));
    if (errors->count > 0)
    {
        retval->error_count = errors->count;
        retval->errors = errorlist_flatten(errors);
        if (retval->errors == NULL)
        {
            errorlist_destroy(errors);
            f(retval, d);
            f(output, d);
            return &out_of_mem_data_preprocessor;
        } // if
    } // if

    errorlist_destroy(errors);

    retval->output = output;
    retval->output_len = total_bytes;
    retval->malloc = m;
    retval->free = f;
    retval->malloc_data = d;
    return retval;
} // MOJOSHADER_preprocess


void MOJOSHADER_freePreprocessData(const MOJOSHADER_preprocessData *_data)
{
    MOJOSHADER_preprocessData *data = (MOJOSHADER_preprocessData *) _data;
    if ((data == NULL) || (data == &out_of_mem_data_preprocessor))
        return;

    MOJOSHADER_free f = (data->free == NULL) ? MOJOSHADER_internal_free : data->free;
    void *d = data->malloc_data;
    int i;

    f((void *) data->output, d);

    for (i = 0; i < data->error_count; i++)
    {
        f((void *) data->errors[i].error, d);
        f((void *) data->errors[i].filename, d);
    } // for
    f(data->errors, d);

    f(data, d);
} // MOJOSHADER_freePreprocessData


// end of mojoshader_preprocessor.c ...


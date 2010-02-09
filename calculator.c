#define __MOJOSHADER_INTERNAL__ 1
#include "mojoshader_internal.h"

#if DEBUG_COMPILER_PARSER
#define LEMON_SUPPORT_TRACING 1
#endif

typedef union TokenData
{
    int64 i64;
    double dbl;
    const char *string;
} TokenData;

typedef struct StringBucket
{
    char *string;
    struct StringBucket *next;
} StringBucket;

typedef struct Context
{
    int isfail;
    int out_of_memory;
    MOJOSHADER_malloc malloc;
    MOJOSHADER_free free;
    void *malloc_data;
    int error_count;
    ErrorList *errors;
    Preprocessor *preprocessor;
    StringBucket *string_hashtable[256];
} Context;


// Convenience functions for allocators...

static inline void out_of_memory(Context *ctx)
{
    ctx->isfail = ctx->out_of_memory = 1;
} // out_of_memory

static inline void *Malloc(Context *ctx, const size_t len)
{
    void *retval = ctx->malloc((int) len, ctx->malloc_data);
    if (retval == NULL)
        out_of_memory(ctx);
    return retval;
} // Malloc

static inline char *StrDup(Context *ctx, const char *str)
{
    char *retval = (char *) Malloc(ctx, strlen(str) + 1);
    if (retval != NULL)
        strcpy(retval, str);
    return retval;
} // StrDup

static inline void Free(Context *ctx, void *ptr)
{
    if (ptr != NULL)  // check for NULL in case of dumb free() impl.
        ctx->free(ptr, ctx->malloc_data);
} // Free

typedef enum Operator
{
    OP_START_RANGE_UNARY,
    OP_POSTINCREMENT,
    OP_POSTDECREMENT,
    OP_PREINCREMENT,
    OP_PREDECREMENT,
    OP_NEGATE,
    OP_COMPLEMENT,
    OP_NOT,
    OP_END_RANGE_UNARY,

    OP_START_RANGE_BINARY,
    OP_DEREF_ARRAY,
    OP_CALLFUNC,
    OP_DEREF_STRUCT,
    OP_COMMA,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_ADD,
    OP_SUBTRACT,
    OP_LSHIFT,
    OP_RSHIFT,
    OP_LESSTHAN,
    OP_GREATERTHAN,
    OP_LESSTHANOREQUAL,
    OP_GREATERTHANOREQUAL,
    OP_EQUAL,
    OP_NOTEQUAL,
    OP_BINARYAND,
    OP_BINARYXOR,
    OP_BINARYOR,
    OP_LOGICALAND,
    OP_LOGICALOR,
    OP_ASSIGN,
    OP_MULASSIGN,
    OP_DIVASSIGN,
    OP_MODASSIGN,
    OP_ADDASSIGN,
    OP_SUBASSIGN,
    OP_LSHIFTASSIGN,
    OP_RSHIFTASSIGN,
    OP_ANDASSIGN,
    OP_XORASSIGN,
    OP_ORASSIGN,
    OP_END_RANGE_BINARY,

    OP_START_RANGE_TERNARY,
    OP_CONDITIONAL,
    OP_END_RANGE_TERNARY,

    OP_START_RANGE_DATA,
    OP_IDENTIFIER,
    OP_INT_LITERAL,
    OP_FLOAT_LITERAL,
    OP_STRING_LITERAL,
    OP_END_RANGE_DATA,
} Operator;

static inline int operator_is_unary(const Operator op)
{
    return ((op > OP_START_RANGE_UNARY) && (op < OP_END_RANGE_UNARY));
} // operator_is_unary

static inline int operator_is_binary(const Operator op)
{
    return ((op > OP_START_RANGE_BINARY) && (op < OP_END_RANGE_BINARY));
} // operator_is_binary

static inline int operator_is_ternary(const Operator op)
{
    return ((op > OP_START_RANGE_TERNARY) && (op < OP_END_RANGE_TERNARY));
} // operator_is_ternary


typedef struct Expression
{
    Operator op;  // operator
} Expression;

#define NEW_EXPR(cls) \
    cls *retval = Malloc(ctx, sizeof (cls)); \
    if (retval == NULL) { return NULL; }

typedef struct ExpressionUnary
{
    Operator op;  // operator
    Expression *operand;
} ExpressionUnary;

typedef struct ExpressionBinary
{
    Operator op;  // operator
    Expression *left;
    Expression *right;
} ExpressionBinary;

typedef struct ExpressionTernary
{
    Operator op;  // operator
    Expression *left;
    Expression *center;
    Expression *right;
} ExpressionTernary;

typedef struct ExpressionIdentifier
{
    Operator op;  // Always OP_IDENTIFIER
    const char *identifier;
} ExpressionIdentifier;

typedef struct ExpressionIntLiteral
{
    Operator op;  // Always OP_INT_LITERAL
    int64 value;
} ExpressionIntLiteral;

typedef struct ExpressionFloatLiteral
{
    Operator op;  // Always OP_FLOAT_LITERAL
    double value;
} ExpressionFloatLiteral;

typedef struct ExpressionStringLiteral
{
    Operator op;  // Always OP_STRING_LITERAL
    const char *string;
} ExpressionStringLiteral;

static Expression *new_unary_expr(Context *ctx, const Operator op,
                                  Expression *operand)
{
    NEW_EXPR(ExpressionUnary);
    assert(operator_is_unary(op));
    retval->op = op;
    retval->operand = operand;
    return (Expression *) retval;
} // new_unary_expr

static Expression *new_binary_expr(Context *ctx, const Operator op,
                                   Expression *left, Expression *right)
{
    NEW_EXPR(ExpressionBinary);
    assert(operator_is_binary(op));
    retval->op = op;
    retval->left = left;
    retval->right = right;
    return (Expression *) retval;
} // new_binary_expr

static Expression *new_ternary_expr(Context *ctx, const Operator op,
                                    Expression *left, Expression *center,
                                    Expression *right)
{
    NEW_EXPR(ExpressionTernary);
    assert(operator_is_ternary(op));
    retval->op = op;
    retval->left = left;
    retval->center = center;
    retval->right = right;
    return (Expression *) retval;
} // new_ternary_expr

static Expression *new_identifier_expr(Context *ctx, const TokenData *data)
{
    NEW_EXPR(ExpressionIdentifier);
    retval->op = OP_IDENTIFIER;
    retval->identifier = data->string;  // cached; don't copy string.
    return (Expression *) retval;
} // new_identifier_expr

static inline int64 strtoi64(const char *str, unsigned int len)
{
    int64 retval = 0;
    int64 mult = 1;
    int i = 0;

    while ((len) && (*str == ' '))
    {
        str++;
        len--;
    } // while

    if ((len) && (*str == '-'))
    {
        mult = -1;
        str++;
        len--;
    } // if

    while (i < len)
    {
        const char ch = str[i];
        if ((ch < '0') || (ch > '9'))
            break;
        i++;
    } // while

    while (--i >= 0)
    {
        const char ch = str[i];
        retval += ((int64) (ch - '0')) * mult;
        mult *= 10;
    } // while

    return retval;
} // strtoi64

static Expression *new_literal_int_expr(Context *ctx, const TokenData *data)
{
    NEW_EXPR(ExpressionIntLiteral);
    retval->op = OP_INT_LITERAL;
    retval->value = data->i64;
    return (Expression *) retval;
} // new_literal_int_expr

static inline double strtodouble(const char *_str, unsigned int len)
{
    // !!! FIXME: laziness prevails.
    char *str = (char *) alloca(len+1);
    memcpy(str, _str, len);
    str[len] = '\0';
    return strtod(str, NULL);
} // strtodouble

static Expression *new_literal_float_expr(Context *ctx, const TokenData *data)
{
    NEW_EXPR(ExpressionFloatLiteral);
    retval->op = OP_FLOAT_LITERAL;
    retval->value = data->dbl;
    return (Expression *) retval;
} // new_literal_float_expr

static Expression *new_literal_string_expr(Context *ctx, const TokenData *data)
{
    NEW_EXPR(ExpressionStringLiteral);
    retval->op = OP_STRING_LITERAL;
    retval->string = data->string;  // cached; don't copy string.
    return (Expression *) retval;
} // new_string_literal_expr

static void print_expr(const Expression *expr, const int depth)
{
    int i;
    for (i = 0; i < depth; i++)
        printf("    ");

    printf("Expression ");
    switch (expr->op)
    {
        #define PRINT_OP(op) case op: printf("%s\n", #op); break;
        PRINT_OP(OP_DEREF_ARRAY);
        PRINT_OP(OP_CALLFUNC);
        PRINT_OP(OP_DEREF_STRUCT);
        PRINT_OP(OP_POSTINCREMENT);
        PRINT_OP(OP_POSTDECREMENT);
        PRINT_OP(OP_COMMA);
        PRINT_OP(OP_PREINCREMENT);
        PRINT_OP(OP_PREDECREMENT);
        PRINT_OP(OP_NEGATE);
        PRINT_OP(OP_COMPLEMENT);
        PRINT_OP(OP_NOT);
        PRINT_OP(OP_MULTIPLY);
        PRINT_OP(OP_DIVIDE);
        PRINT_OP(OP_MODULO);
        PRINT_OP(OP_ADD);
        PRINT_OP(OP_SUBTRACT);
        PRINT_OP(OP_LSHIFT);
        PRINT_OP(OP_RSHIFT);
        PRINT_OP(OP_LESSTHAN);
        PRINT_OP(OP_GREATERTHAN);
        PRINT_OP(OP_LESSTHANOREQUAL);
        PRINT_OP(OP_GREATERTHANOREQUAL);
        PRINT_OP(OP_EQUAL);
        PRINT_OP(OP_NOTEQUAL);
        PRINT_OP(OP_BINARYAND);
        PRINT_OP(OP_BINARYXOR);
        PRINT_OP(OP_BINARYOR);
        PRINT_OP(OP_LOGICALAND);
        PRINT_OP(OP_LOGICALOR);
        PRINT_OP(OP_CONDITIONAL);
        PRINT_OP(OP_ASSIGN);
        PRINT_OP(OP_MULASSIGN);
        PRINT_OP(OP_DIVASSIGN);
        PRINT_OP(OP_MODASSIGN);
        PRINT_OP(OP_ADDASSIGN);
        PRINT_OP(OP_SUBASSIGN);
        PRINT_OP(OP_LSHIFTASSIGN);
        PRINT_OP(OP_RSHIFTASSIGN);
        PRINT_OP(OP_ANDASSIGN);
        PRINT_OP(OP_XORASSIGN);
        PRINT_OP(OP_ORASSIGN);
        PRINT_OP(OP_INT_LITERAL);
        PRINT_OP(OP_FLOAT_LITERAL);
        PRINT_OP(OP_STRING_LITERAL);
        PRINT_OP(OP_IDENTIFIER);
        default: printf("---UNKNOWN!---\n"); return;
    } // switch

    if (operator_is_unary(expr->op))
    {
        const ExpressionUnary *unary = (const ExpressionUnary *) expr;
        print_expr(unary->operand, depth + 1);
    } // if
    else if (operator_is_binary(expr->op))
    {
        const ExpressionBinary *binary = (const ExpressionBinary *) expr;
        print_expr(binary->left, depth + 1);
        print_expr(binary->right, depth + 1);
    } // else if
    else if (operator_is_ternary(expr->op))
    {
        const ExpressionTernary *ternary = (const ExpressionTernary *) expr;
        print_expr(ternary->left, depth + 1);
        print_expr(ternary->center, depth + 1);
        print_expr(ternary->right, depth + 1);
    } // else if

    else
    {
        for (i = 0; i < (depth + 1); i++)
            printf("    ");

        if (expr->op == OP_IDENTIFIER)
        {
            const ExpressionIdentifier *ident = (const ExpressionIdentifier *) expr;
            printf("(%s)\n", ident->identifier);
        } // if
        else if (expr->op == OP_INT_LITERAL)
        {
            const ExpressionIntLiteral *lit = (const ExpressionIntLiteral *) expr;
            printf("(%lld)\n", (long long) lit->value);
        } // if
        else if (expr->op == OP_FLOAT_LITERAL)
        {
            const ExpressionFloatLiteral *lit = (const ExpressionFloatLiteral *) expr;
            printf("(%lf)\n", lit->value);
        } // if
        else if (expr->op == OP_STRING_LITERAL)
        {
            const ExpressionStringLiteral *lit = (const ExpressionStringLiteral *) expr;
            printf("(\"%s\")\n", lit->string);
        } // if
        else
        {
            assert(0 && "Shouldn't hit this.");
        } // else
    } // else
} // print_expr

static double run_expr(const Expression *expr)
{
    if (operator_is_unary(expr->op))
    {
        const ExpressionUnary *unary = (const ExpressionUnary *) expr;
        if (expr->op == OP_NEGATE)
            return -run_expr(unary->operand);
        else if (expr->op == OP_COMPLEMENT)
            return (double) (~((int64)run_expr(unary->operand)));
        else if (expr->op == OP_NOT)
            return (run_expr(unary->operand) == 0.0) ? 1.0 : 0.0;
    } // if
    else if (operator_is_binary(expr->op))
    {
        const ExpressionBinary *binary = (const ExpressionBinary *) expr;
        if (expr->op == OP_MULTIPLY)
            return run_expr(binary->left) * run_expr(binary->right);
        else if (expr->op == OP_DIVIDE)
            return run_expr(binary->left) / run_expr(binary->right);
        else if (expr->op == OP_ADD)
            return run_expr(binary->left) + run_expr(binary->right);
        else if (expr->op == OP_SUBTRACT)
            return run_expr(binary->left) - run_expr(binary->right);
        else if (expr->op == OP_LESSTHAN)
            return (run_expr(binary->left) < run_expr(binary->right)) ? 1.0 : 0.0;
        else if (expr->op == OP_GREATERTHAN)
            return (run_expr(binary->left) > run_expr(binary->right)) ? 1.0 : 0.0;
        else if (expr->op == OP_LESSTHANOREQUAL)
            return (run_expr(binary->left) <= run_expr(binary->right)) ? 1.0 : 0.0;
        else if (expr->op == OP_GREATERTHANOREQUAL)
            return (run_expr(binary->left) >= run_expr(binary->right)) ? 1.0 : 0.0;
        else if (expr->op == OP_EQUAL)
            return (run_expr(binary->left) == run_expr(binary->right)) ? 1.0 : 0.0;
        else if (expr->op == OP_NOTEQUAL)
            return (run_expr(binary->left) == run_expr(binary->right)) ? 1.0 : 0.0;
        else if (expr->op == OP_LOGICALAND)
            return (((int64)run_expr(binary->left)) && ((int64)run_expr(binary->right))) ? 1.0 : 0.0;
        else if (expr->op == OP_LOGICALOR)
            return (((int64)run_expr(binary->left)) || ((int64)run_expr(binary->right))) ? 1.0 : 0.0;
        else if (expr->op == OP_BINARYAND)
            return (double)(((int64)run_expr(binary->left)) & ((int64)run_expr(binary->right)));
        else if (expr->op == OP_BINARYOR)
            return (double)(((int64)run_expr(binary->left)) | ((int64)run_expr(binary->right)));
        else if (expr->op == OP_BINARYXOR)
            return (double)(((int64)run_expr(binary->left)) ^ ((int64)run_expr(binary->right)));
        else if (expr->op == OP_LSHIFT)
            return (double)(((int64)run_expr(binary->left)) << ((int64)run_expr(binary->right)));
        else if (expr->op == OP_RSHIFT)
            return (double)(((int64)run_expr(binary->left)) >> ((int64)run_expr(binary->right)));
        else if (expr->op == OP_MODULO)
            return (double)(((int64)run_expr(binary->left)) % ((int64)run_expr(binary->right)));
    } // else if

    else if (operator_is_ternary(expr->op))
    {
        const ExpressionTernary *ternary = (const ExpressionTernary *) expr;
        if (expr->op == OP_CONDITIONAL)
            return (run_expr(ternary->left) != 0.0) ? run_expr(ternary->center) : run_expr(ternary->right);
    } // else if

    else
    {
        if (expr->op == OP_INT_LITERAL)
        {
            const ExpressionIntLiteral *lit = (const ExpressionIntLiteral *) expr;
            return ((double) lit->value);
        } // if
        else if (expr->op == OP_FLOAT_LITERAL)
        {
            const ExpressionFloatLiteral *lit = (const ExpressionFloatLiteral *) expr;
            return lit->value;
        } // if
    } // else

    return 0.0;  // oh well.
} // run_expr

static void free_expr(Context *ctx, Expression *expr)
{
    if (operator_is_unary(expr->op))
    {
        const ExpressionUnary *unary = (const ExpressionUnary *) expr;
        free_expr(ctx, unary->operand);
    } // if
    else if (operator_is_binary(expr->op))
    {
        const ExpressionBinary *binary = (const ExpressionBinary *) expr;
        free_expr(ctx, binary->left);
        free_expr(ctx, binary->right);
    } // else if
    else if (operator_is_ternary(expr->op))
    {
        const ExpressionTernary *ternary = (const ExpressionTernary *) expr;
        free_expr(ctx, ternary->left);
        free_expr(ctx, ternary->center);
        free_expr(ctx, ternary->right);
    } // else if

    // don't need to free extra fields in other types at the moment.

    Free(ctx, expr);
} // free_expr

static void parse_complete(Context *ctx, Expression *expr)
{
    print_expr(expr, 0);
    printf("Result: %lf\n\n", run_expr(expr));
    free_expr(ctx, expr);
} // parse_complete


// !!! FIXME: sort of cut-and-paste from the preprocessor...

// this is djb's xor hashing function.
static inline uint32 hash_string_djbxor(const char *str, unsigned int len)
{
    register uint32 hash = 5381;
    while (len--)
        hash = ((hash << 5) + hash) ^ *(str++);
    return hash;
} // hash_string_djbxor

static inline uint8 hash_string(const char *str, const unsigned int len)
{
    return (uint8) hash_string_djbxor(str, len);
} // hash_string

static const char *cache_string(Context *ctx, const char *str,
                                const unsigned int len)
{
    const uint8 hash = hash_string(str, len);
    StringBucket *bucket = ctx->string_hashtable[hash];
    StringBucket *prev = NULL;
    while (bucket)
    {
        const char *bstr = bucket->string;
        if ((strncmp(bstr, str, len) == 0) && (bstr[len] == 0))
        {
            // Matched! Move this to the front of the list.
            if (prev != NULL)
            {
                assert(prev->next == bucket);
                prev->next = bucket->next;
                bucket->next = ctx->string_hashtable[hash];
                ctx->string_hashtable[hash] = bucket;
            } // if
            return bstr; // already cached
        } // if
        prev = bucket;
        bucket = bucket->next;
    } // while

    // no match, add to the table.
    bucket = (StringBucket *) Malloc(ctx, sizeof (StringBucket));
    if (bucket == NULL)
        return NULL;
    bucket->string = (char *) Malloc(ctx, len + 1);
    if (bucket->string == NULL)
    {
        Free(ctx, bucket);
        return NULL;
    } // if
    memcpy(bucket->string, str, len);
    bucket->string[len] = '\0';
    bucket->next = ctx->string_hashtable[hash];
    ctx->string_hashtable[hash] = bucket;
    return bucket->string;
} // cache_string

static void free_string_cache(Context *ctx)
{
    size_t i;
    for (i = 0; i < STATICARRAYLEN(ctx->string_hashtable); i++)
    {
        StringBucket *bucket = ctx->string_hashtable[i];
        ctx->string_hashtable[i] = NULL;
        while (bucket)
        {
            StringBucket *next = bucket->next;
            Free(ctx, bucket->string);
            Free(ctx, bucket);
            bucket = next;
        } // while
    } // for
} // free_string_cache


// This is where the actual parsing happens. It's Lemon-generated!
#define __MOJOSHADER_CALC_COMPILER__ 1
#include "calculator.h"

static int convert_to_lemon_token(const Context *ctx, const Token tokenval)
{
    switch (tokenval)
    {
        case ((Token) ','): return TOKEN_CALC_COMMA;
        case ((Token) '='): return TOKEN_CALC_ASSIGN;
        case ((Token) TOKEN_ADDASSIGN): return TOKEN_CALC_ADDASSIGN;
        case ((Token) TOKEN_SUBASSIGN): return TOKEN_CALC_SUBASSIGN;
        case ((Token) TOKEN_MULTASSIGN): return TOKEN_CALC_MULASSIGN;
        case ((Token) TOKEN_DIVASSIGN): return TOKEN_CALC_DIVASSIGN;
        case ((Token) TOKEN_MODASSIGN): return TOKEN_CALC_MODASSIGN;
        case ((Token) TOKEN_LSHIFTASSIGN): return TOKEN_CALC_LSHIFTASSIGN;
        case ((Token) TOKEN_RSHIFTASSIGN): return TOKEN_CALC_RSHIFTASSIGN;
        case ((Token) TOKEN_ANDASSIGN): return TOKEN_CALC_ANDASSIGN;
        case ((Token) TOKEN_ORASSIGN): return TOKEN_CALC_ORASSIGN;
        case ((Token) TOKEN_XORASSIGN): return TOKEN_CALC_XORASSIGN;
        case ((Token) '?'): return TOKEN_CALC_QUESTION;
        case ((Token) TOKEN_OROR): return TOKEN_CALC_OROR;
        case ((Token) TOKEN_ANDAND): return TOKEN_CALC_ANDAND;
        case ((Token) '|'): return TOKEN_CALC_OR;
        case ((Token) '^'): return TOKEN_CALC_XOR;
        case ((Token) '&'): return TOKEN_CALC_AND;
        case ((Token) TOKEN_EQL): return TOKEN_CALC_EQL;
        case ((Token) TOKEN_NEQ): return TOKEN_CALC_NEQ;
        case ((Token) '<'): return TOKEN_CALC_LT;
        case ((Token) TOKEN_LEQ): return TOKEN_CALC_LEQ;
        case ((Token) '>'): return TOKEN_CALC_GT;
        case ((Token) TOKEN_GEQ): return TOKEN_CALC_GEQ;
        case ((Token) TOKEN_LSHIFT): return TOKEN_CALC_LSHIFT;
        case ((Token) TOKEN_RSHIFT): return TOKEN_CALC_RSHIFT;
        case ((Token) '+'): return TOKEN_CALC_PLUS;
        case ((Token) '-'): return TOKEN_CALC_MINUS;
        case ((Token) '*'): return TOKEN_CALC_STAR;
        case ((Token) '/'): return TOKEN_CALC_SLASH;
        case ((Token) '%'): return TOKEN_CALC_PERCENT;
        case ((Token) '!'): return TOKEN_CALC_EXCLAMATION;
        case ((Token) '~'): return TOKEN_CALC_COMPLEMENT;
        case ((Token) TOKEN_DECREMENT): return TOKEN_CALC_MINUSMINUS;
        case ((Token) TOKEN_INCREMENT): return TOKEN_CALC_PLUSPLUS;
        case ((Token) '.'): return TOKEN_CALC_DOT;
        case ((Token) '['): return TOKEN_CALC_LBRACKET;
        case ((Token) ']'): return TOKEN_CALC_RBRACKET;
        case ((Token) '('): return TOKEN_CALC_LPAREN;
        case ((Token) ')'): return TOKEN_CALC_RPAREN;
        case ((Token) TOKEN_INT_LITERAL): return TOKEN_CALC_INT_CONSTANT;
        case ((Token) TOKEN_FLOAT_LITERAL): return TOKEN_CALC_FLOAT_CONSTANT;
        case ((Token) TOKEN_STRING_LITERAL): return TOKEN_CALC_STRING_LITERAL;
        case ((Token) ':'): return TOKEN_CALC_COLON;
        //case ((Token) ';'): return TOKEN_CALC_SEMICOLON;
        //case ((Token) '{'): return TOKEN_CALC_LBRACE;
        //case ((Token) '}'): return TOKEN_CALC_RBRACE;
        case ((Token) TOKEN_IDENTIFIER): return TOKEN_CALC_IDENTIFIER;
        case TOKEN_EOI: return 0;
        case TOKEN_BAD_CHARS: printf("bad chars from lexer\n"); return 0;
        case TOKEN_PREPROCESSING_ERROR: printf("error from lexer\n"); return 0;
        default: assert(0 && "unexpected token from lexer\n"); return 0;
    } // switch

    return 0;
} // convert_to_lemon_token

static void MOJOSHADER_compile(const char *filename,
                             const char *source, unsigned int sourcelen,
                             const MOJOSHADER_preprocessorDefine *defines,
                             unsigned int define_count,
                             MOJOSHADER_includeOpen include_open,
                             MOJOSHADER_includeClose include_close,
                             MOJOSHADER_malloc m, MOJOSHADER_free f, void *d)
{
    Context ctx;
    TokenData data;
    unsigned int tokenlen;
    Token tokenval;
    const char *token;
    int lemon_token;

    if (m == NULL) m = MOJOSHADER_internal_malloc;
    if (f == NULL) f = MOJOSHADER_internal_free;

    memset(&ctx, '\0', sizeof (Context));
    ctx.malloc = m;
    ctx.free = f;
    ctx.malloc_data = d;
    ctx.preprocessor = preprocessor_start(filename, source, sourcelen,
                                           include_open, include_close,
                                           defines, define_count, 0, m, f, d);

    void *pParser = ParseCalculatorAlloc(m, d);

    #if DEBUG_COMPILER_PARSER
    ParseCalculatorTrace(stdout, "COMPILER: ");
    #endif

    do {
        token = preprocessor_nexttoken(ctx.preprocessor, &tokenlen, &tokenval);
        lemon_token = convert_to_lemon_token(&ctx, tokenval);
        switch (lemon_token)
        {
            case TOKEN_CALC_INT_CONSTANT:
                data.i64 = strtoi64(token, tokenlen);
                break;

            case TOKEN_CALC_FLOAT_CONSTANT:
                data.dbl = strtodouble(token, tokenlen);
                break;

            case TOKEN_CALC_STRING_LITERAL:
            case TOKEN_CALC_IDENTIFIER:
                data.string = cache_string(&ctx, token, tokenlen);
                break;

            default:
                data.i64 = 0;
                break;
        } // switch

        ParseCalculator(pParser, lemon_token, data, &ctx);
    } while ((!ctx.isfail) && (tokenval != TOKEN_EOI));

    ParseCalculatorFree(pParser, f, d);
    // !!! FIXME: destruct (ctx) here.
    free_string_cache(&ctx);
} // MOJOSHADER_compile

int main(int argc, char **argv)
{
    const char *ln;
    size_t len = 0;
    FILE *io = stdin;
    const char *filename = "<stdin>";

    while ((ln = fgetln(io, &len)) != NULL)
    {
        if (len == 1)
            continue;
        else if ((len == 5) && (memcmp(ln, "quit\n", 5) == 0))
            break;
        else if ((len == 2) && (memcmp(ln, "q\n", 2) == 0))
            break;

        MOJOSHADER_compile(filename, ln, (unsigned int) len,
                           NULL, 0, NULL, NULL, NULL, NULL, NULL);
    } // while

    fclose(io);
    return 0;
} // main

// end of calculator.c ...

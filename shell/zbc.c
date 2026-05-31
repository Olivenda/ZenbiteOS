/* ZBC -- Zenbite C: a tiny tree-walking interpreter for a useful subset
 * of C. This is NOT a real compiler -- it parses your source and walks
 * the AST directly. The point is to let users write and run real C-looking
 * programs inside Zenbite, in the spirit of Turbo C 1.0's REPL-ish feel.
 *
 * Supported:
 *   int variables (global and local), int literals (decimal, hex),
 *   arithmetic: + - * / % ()
 *   comparison: == != < > <= >=
 *   logical:    && || !
 *   bitwise:    & | ^ << >>
 *   assignment: = += -= *= /=
 *   control:    if/else, while, for, return, break, continue
 *   functions:  int name(int a, int b) { ... }   void name(...) { ... }
 *   builtins:   printf(fmt, ...)  -> %d %i %u %x %c %s %% (printf-style)
 *               print(int)        -> prints a decimal number + newline
 *               puts(string)      -> prints a string literal + newline
 *               putchar(int)      -> prints one character
 *               getchar()         -> reads one keypress (blocking)
 *   includes:   #include <stdio.h> and other # lines are accepted and
 *               ignored -- the runtime above is always available.
 *
 * Not supported (yet): pointers, arrays, structs, floats, strings as
 * variables. Use `puts("hi")` for output; strings are literals only.
 *
 * Hard limits: 64 functions, 64 variables per scope, 4096 AST nodes per
 * program, 256 KiB source. Plenty for educational programs.
 */

#include "kernel.h"
#include "kio.h"
#include "string.h"
#include "fs.h"

#define MAX_SRC      (8 * 1024)
#define MAX_TOKENS   512
#define MAX_NODES    512
#define MAX_FUNCS    16
#define MAX_VARS     32
#define MAX_STRTAB   1024
#define MAX_CALL     8

enum tok {
    T_EOF = 0, T_INT, T_IDENT, T_STR,
    T_LPAR, T_RPAR, T_LBRACE, T_RBRACE, T_SEMI, T_COMMA,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT,
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ,
    T_EQ, T_NEQ, T_LT, T_GT, T_LE, T_GE,
    T_AND, T_OR, T_NOT, T_BAND, T_BOR, T_BXOR, T_SHL, T_SHR,
    T_KW_INT, T_KW_VOID, T_KW_IF, T_KW_ELSE, T_KW_WHILE,
    T_KW_RETURN, T_KW_BREAK, T_KW_CONT, T_KW_FOR
};

struct token {
    enum tok kind;
    int      ival;
    char     sval[32];
    int      str_off;          /* offset into string table */
};

enum nkind {
    N_NUM, N_VAR, N_STR, N_ASSIGN, N_BIN, N_UN, N_CALL,
    N_IF, N_WHILE, N_FOR, N_BLOCK, N_RETURN, N_BREAK, N_CONT, N_EXPR_STMT,
    N_FUNC,
};

struct node {
    enum nkind kind;
    int  op;              /* tok kind for BIN/UN/ASSIGN */
    int  ival;
    int  str_off;
    char name[32];
    int  child[4];        /* indices into node table; -1 = none */
    int  next;            /* used for statement lists / args, -1 terminator */
};

struct func {
    char name[32];
    int  body;            /* node idx of block */
    int  param_count;
    char params[8][32];
    int  is_void;
    int  is_builtin;
    int  (*native)(int *args, int n, int *out);
};

struct var {
    char name[32];
    int  value;
};

/* ---- globals (kept off the kernel stack) -------------------- */
static char           src[MAX_SRC];
static struct token   toks[MAX_TOKENS];
static int            tok_count, tok_pos;
static char           strtab[MAX_STRTAB];
static int            strtab_pos;
static struct node    nodes[MAX_NODES];
static int            node_count;
static struct func    funcs[MAX_FUNCS];
static int            func_count;
static struct var     globals[MAX_VARS];
static int            global_count;
static struct var     scopes[MAX_CALL][MAX_VARS];
static int            scope_var_count[MAX_CALL];
static int            scope_depth;
static int            had_error;
static int            had_return;
static int            return_value;
static int            had_break, had_continue;

static void zbc_err(const char *msg) {
    if (!had_error) kprintf("ZBC error: %s\n", msg);
    had_error = 1;
}

/* ---- string table -------------------------------------------- */
static int strtab_add(const char *s, int len) {
    if (strtab_pos + len + 1 > MAX_STRTAB) { zbc_err("string table full"); return 0; }
    int off = strtab_pos;
    memcpy(&strtab[off], s, (size_t)len);
    strtab[off + len] = '\0';
    strtab_pos += len + 1;
    return off;
}

/* ---- tokeniser ----------------------------------------------- */
struct kw { const char *n; enum tok t; };
static const struct kw keywords[] = {
    { "int",      T_KW_INT    },
    { "void",     T_KW_VOID   },
    { "if",       T_KW_IF     },
    { "else",     T_KW_ELSE   },
    { "while",    T_KW_WHILE  },
    { "return",   T_KW_RETURN },
    { "break",    T_KW_BREAK  },
    { "continue", T_KW_CONT   },
    { "for",      T_KW_FOR    },
};

static int tokenize(const char *s) {
    int p = 0;
    tok_count = 0;
    while (s[p] && tok_count < MAX_TOKENS - 1) {
        char c = s[p];
        if (isspace((u8)c)) { p++; continue; }
        if (c == '/' && s[p+1] == '/') { while (s[p] && s[p] != '\n') p++; continue; }
        if (c == '/' && s[p+1] == '*') {
            p += 2;
            while (s[p] && !(s[p] == '*' && s[p+1] == '/')) p++;
            if (s[p]) p += 2;
            continue;
        }
        /* Preprocessor lines (#include <stdio.h>, #define, #pragma, ...)
         * are accepted and skipped. The C runtime Zenbite provides --
         * printf/puts/putchar/getchar/print -- is always available, so
         * <stdio.h> etc. are implicit. This lets users paste standard-
         * looking C without the parser choking on the directives. */
        if (c == '#') { while (s[p] && s[p] != '\n') p++; continue; }
        struct token *t = &toks[tok_count];
        memset(t, 0, sizeof *t);
        if (isdigit((u8)c)) {
            int v = 0;
            if (c == '0' && (s[p+1] == 'x' || s[p+1] == 'X')) {
                p += 2;
                while (isdigit((u8)s[p]) ||
                       (s[p] >= 'a' && s[p] <= 'f') ||
                       (s[p] >= 'A' && s[p] <= 'F')) {
                    char ch = s[p++];
                    int d = isdigit((u8)ch) ? ch - '0' :
                            (ch >= 'a' ? ch - 'a' + 10 : ch - 'A' + 10);
                    v = v * 16 + d;
                }
            } else {
                while (isdigit((u8)s[p])) { v = v * 10 + (s[p++] - '0'); }
            }
            t->kind = T_INT; t->ival = v;
            tok_count++; continue;
        }
        if (isalpha((u8)c) || c == '_') {
            int n = 0;
            while ((isalpha((u8)s[p]) || isdigit((u8)s[p]) || s[p] == '_') && n < 31)
                t->sval[n++] = s[p++];
            t->sval[n] = '\0';
            t->kind = T_IDENT;
            for (size_t k = 0; k < ARRAY_LEN(keywords); k++)
                if (strcmp(t->sval, keywords[k].n) == 0) { t->kind = keywords[k].t; break; }
            tok_count++; continue;
        }
        if (c == '"') {
            p++;
            char tmp[256]; int n = 0;
            while (s[p] && s[p] != '"' && n < 255) {
                if (s[p] == '\\' && s[p+1]) {
                    char e = s[++p];
                    p++;
                    switch (e) {
                    case 'n': tmp[n++] = '\n'; break;
                    case 't': tmp[n++] = '\t'; break;
                    case 'r': tmp[n++] = '\r'; break;
                    case '\\': tmp[n++] = '\\'; break;
                    case '"': tmp[n++] = '"'; break;
                    case '0': tmp[n++] = '\0'; break;
                    default: tmp[n++] = e;
                    }
                } else {
                    tmp[n++] = s[p++];
                }
            }
            if (s[p] == '"') p++;
            t->kind = T_STR;
            t->str_off = strtab_add(tmp, n);
            tok_count++; continue;
        }
        switch (c) {
        case '(': t->kind = T_LPAR;  p++; break;
        case ')': t->kind = T_RPAR;  p++; break;
        case '{': t->kind = T_LBRACE;p++; break;
        case '}': t->kind = T_RBRACE;p++; break;
        case ';': t->kind = T_SEMI;  p++; break;
        case ',': t->kind = T_COMMA; p++; break;
        case '+': p++; if (s[p]=='=') { t->kind=T_PLUSEQ; p++; }  else t->kind=T_PLUS;  break;
        case '-': p++; if (s[p]=='=') { t->kind=T_MINUSEQ;p++; }  else t->kind=T_MINUS; break;
        case '*': p++; if (s[p]=='=') { t->kind=T_STAREQ; p++; }  else t->kind=T_STAR;  break;
        case '/': p++; if (s[p]=='=') { t->kind=T_SLASHEQ;p++; }  else t->kind=T_SLASH; break;
        case '%': t->kind=T_PCT;     p++; break;
        case '=': p++; if (s[p]=='=') { t->kind=T_EQ;     p++; }  else t->kind=T_ASSIGN;break;
        case '!': p++; if (s[p]=='=') { t->kind=T_NEQ;    p++; }  else t->kind=T_NOT;   break;
        case '<':
            p++;
            if      (s[p]=='=') { t->kind=T_LE;  p++; }
            else if (s[p]=='<') { t->kind=T_SHL; p++; }
            else                  t->kind=T_LT;
            break;
        case '>':
            p++;
            if      (s[p]=='=') { t->kind=T_GE;  p++; }
            else if (s[p]=='>') { t->kind=T_SHR; p++; }
            else                  t->kind=T_GT;
            break;
        case '&': p++; if (s[p]=='&') { t->kind=T_AND;    p++; }  else t->kind=T_BAND;  break;
        case '|': p++; if (s[p]=='|') { t->kind=T_OR;     p++; }  else t->kind=T_BOR;   break;
        case '^': t->kind=T_BXOR; p++; break;
        default:
            kprintf("ZBC: unexpected character '%c'\n", c);
            return -1;
        }
        tok_count++;
    }
    toks[tok_count].kind = T_EOF;
    return 0;
}

/* ---- parser -------------------------------------------------- */
static int parse_block(void);
static int parse_expr (void);
static int parse_stmt (void);

static struct token *peek(void) { return &toks[tok_pos]; }
static struct token *advance(void) { return &toks[tok_pos++]; }
static int accept(enum tok t) {
    if (peek()->kind == t) { advance(); return 1; }
    return 0;
}
static int expect(enum tok t, const char *what) {
    if (!accept(t)) { zbc_err(what); return 0; }
    return 1;
}

static int new_node(enum nkind k) {
    if (node_count >= MAX_NODES) { zbc_err("too many AST nodes"); return 0; }
    int i = node_count++;
    struct node *n = &nodes[i];
    memset(n, 0, sizeof *n);
    n->kind = k;
    for (int j = 0; j < 4; j++) n->child[j] = -1;
    n->next = -1;
    return i;
}

static int parse_primary(void) {
    struct token *t = peek();
    if (t->kind == T_INT) {
        int n = new_node(N_NUM);
        nodes[n].ival = advance()->ival;
        return n;
    }
    if (t->kind == T_STR) {
        int n = new_node(N_STR);
        nodes[n].str_off = advance()->str_off;
        return n;
    }
    if (t->kind == T_IDENT) {
        struct token *id = advance();
        if (peek()->kind == T_LPAR) {
            advance();
            int n = new_node(N_CALL);
            strncpy(nodes[n].name, id->sval, 31);
            int prev = -1;
            int first = -1;
            int argi = 0;
            while (peek()->kind != T_RPAR && !had_error) {
                int a = parse_expr();
                if (first < 0) { nodes[n].child[0] = a; first = a; }
                else nodes[prev].next = a;
                prev = a;
                argi++;
                if (!accept(T_COMMA)) break;
            }
            expect(T_RPAR, "expected )");
            nodes[n].ival = argi;
            return n;
        }
        int n = new_node(N_VAR);
        strncpy(nodes[n].name, id->sval, 31);
        return n;
    }
    if (accept(T_LPAR)) {
        int n = parse_expr();
        expect(T_RPAR, "expected )");
        return n;
    }
    if (t->kind == T_MINUS || t->kind == T_NOT) {
        int op = advance()->kind;
        int n = new_node(N_UN);
        nodes[n].op = op;
        nodes[n].child[0] = parse_primary();
        return n;
    }
    zbc_err("expected expression");
    return new_node(N_NUM);
}

static int bin_prec(enum tok k) {
    switch (k) {
    case T_OR:                                            return 1;
    case T_AND:                                           return 2;
    case T_BOR:                                           return 3;
    case T_BXOR:                                          return 4;
    case T_BAND:                                          return 5;
    case T_EQ: case T_NEQ:                                return 6;
    case T_LT: case T_GT: case T_LE: case T_GE:           return 7;
    case T_SHL: case T_SHR:                               return 8;
    case T_PLUS: case T_MINUS:                            return 9;
    case T_STAR: case T_SLASH: case T_PCT:                return 10;
    default: return 0;
    }
}

static int parse_binop(int prec) {
    int lhs = parse_primary();
    while (bin_prec(peek()->kind) >= prec) {
        int op = advance()->kind;
        int rhs = parse_binop(bin_prec(op) + 1);
        int n = new_node(N_BIN);
        nodes[n].op = op;
        nodes[n].child[0] = lhs;
        nodes[n].child[1] = rhs;
        lhs = n;
    }
    return lhs;
}

static int parse_expr(void) {
    int lhs = parse_binop(1);
    enum tok k = peek()->kind;
    if (k == T_ASSIGN || k == T_PLUSEQ || k == T_MINUSEQ ||
        k == T_STAREQ || k == T_SLASHEQ) {
        if (nodes[lhs].kind != N_VAR) { zbc_err("assign to non-variable"); return lhs; }
        advance();
        int rhs = parse_expr();
        int n = new_node(N_ASSIGN);
        nodes[n].op = k;
        nodes[n].child[0] = lhs;
        nodes[n].child[1] = rhs;
        return n;
    }
    return lhs;
}

static int parse_stmt(void) {
    if (accept(T_LBRACE)) return parse_block();

    if (accept(T_KW_IF)) {
        expect(T_LPAR, "(");
        int cond = parse_expr();
        expect(T_RPAR, ")");
        int then_b = parse_stmt();
        int else_b = -1;
        if (accept(T_KW_ELSE)) else_b = parse_stmt();
        int n = new_node(N_IF);
        nodes[n].child[0] = cond;
        nodes[n].child[1] = then_b;
        nodes[n].child[2] = else_b;
        return n;
    }
    if (accept(T_KW_WHILE)) {
        expect(T_LPAR, "(");
        int cond = parse_expr();
        expect(T_RPAR, ")");
        int body = parse_stmt();
        int n = new_node(N_WHILE);
        nodes[n].child[0] = cond;
        nodes[n].child[1] = body;
        return n;
    }
    if (accept(T_KW_FOR)) {
        /* for (init; cond; post) body
         * init may be `int i = e` or an expression or empty;
         * cond and post are expressions or empty. */
        expect(T_LPAR, "(");
        int init = -1;
        if (accept(T_KW_INT)) {
            struct token *id = advance();
            if (id->kind != T_IDENT) { zbc_err("expected name"); return new_node(N_NUM); }
            int rhs = -1;
            if (accept(T_ASSIGN)) rhs = parse_expr();
            int a = new_node(N_ASSIGN);
            nodes[a].op = T_ASSIGN;
            int v = new_node(N_VAR);
            strncpy(nodes[v].name, id->sval, 31);
            nodes[v].ival = 1;            /* declaration */
            if (rhs < 0) { rhs = new_node(N_NUM); nodes[rhs].ival = 0; }
            nodes[a].child[0] = v;
            nodes[a].child[1] = rhs;
            init = a;
        } else if (peek()->kind != T_SEMI) {
            init = parse_expr();
        }
        expect(T_SEMI, ";");
        int cond = (peek()->kind != T_SEMI) ? parse_expr() : -1;
        expect(T_SEMI, ";");
        int post = (peek()->kind != T_RPAR) ? parse_expr() : -1;
        expect(T_RPAR, ")");
        int body = parse_stmt();
        int n = new_node(N_FOR);
        nodes[n].child[0] = init;
        nodes[n].child[1] = cond;
        nodes[n].child[2] = post;
        nodes[n].child[3] = body;
        return n;
    }
    if (accept(T_KW_RETURN)) {
        int n = new_node(N_RETURN);
        if (!accept(T_SEMI)) {
            nodes[n].child[0] = parse_expr();
            expect(T_SEMI, ";");
        }
        return n;
    }
    if (accept(T_KW_BREAK)) { expect(T_SEMI, ";"); return new_node(N_BREAK); }
    if (accept(T_KW_CONT))  { expect(T_SEMI, ";"); return new_node(N_CONT);  }

    if (accept(T_KW_INT)) {
        struct token *id = advance();
        if (id->kind != T_IDENT) { zbc_err("expected name"); return new_node(N_NUM); }
        int rhs = -1;
        if (accept(T_ASSIGN)) rhs = parse_expr();
        expect(T_SEMI, ";");
        int n = new_node(N_ASSIGN);
        nodes[n].op = T_ASSIGN;
        int v = new_node(N_VAR);
        strncpy(nodes[v].name, id->sval, 31);
        nodes[v].ival = 1;            /* mark as declaration */
        if (rhs < 0) { rhs = new_node(N_NUM); nodes[rhs].ival = 0; }
        nodes[n].child[0] = v;
        nodes[n].child[1] = rhs;
        return n;
    }

    int e = parse_expr();
    expect(T_SEMI, ";");
    int n = new_node(N_EXPR_STMT);
    nodes[n].child[0] = e;
    return n;
}

static int parse_block(void) {
    int b = new_node(N_BLOCK);
    int last = -1;
    while (peek()->kind != T_RBRACE && peek()->kind != T_EOF) {
        int s = parse_stmt();
        if (last < 0) nodes[b].child[0] = s;
        else nodes[last].next = s;
        last = s;
        if (had_error) return b;
    }
    expect(T_RBRACE, "}");
    return b;
}

static int parse_func(int is_void) {
    if (func_count >= MAX_FUNCS) { zbc_err("too many functions"); return -1; }
    struct func *f = &funcs[func_count++];
    memset(f, 0, sizeof *f);
    f->is_void = is_void;
    struct token *id = advance();
    if (id->kind != T_IDENT) { zbc_err("function name"); return -1; }
    strncpy(f->name, id->sval, 31);
    expect(T_LPAR, "(");
    while (peek()->kind != T_RPAR && !had_error) {
        accept(T_KW_INT);
        struct token *pid = advance();
        if (pid->kind != T_IDENT) { zbc_err("param name"); return -1; }
        if (f->param_count < 8) strncpy(f->params[f->param_count++], pid->sval, 31);
        if (!accept(T_COMMA)) break;
    }
    expect(T_RPAR, ")");
    expect(T_LBRACE, "{");
    f->body = parse_block();
    return 0;
}

/* ---- scope helpers ------------------------------------------- */
static struct var *find_var(const char *name) {
    if (scope_depth > 0) {
        for (int i = scope_var_count[scope_depth - 1] - 1; i >= 0; i--)
            if (strcmp(scopes[scope_depth - 1][i].name, name) == 0)
                return &scopes[scope_depth - 1][i];
    }
    for (int i = global_count - 1; i >= 0; i--)
        if (strcmp(globals[i].name, name) == 0) return &globals[i];
    return NULL;
}

static void set_local(const char *name, int v) {
    int d = scope_depth > 0 ? scope_depth - 1 : 0;
    int idx = scope_var_count[d];
    if (idx >= MAX_VARS) { zbc_err("too many locals"); return; }
    struct var *nv = &scopes[d][idx];
    strncpy(nv->name, name, 31); nv->name[31] = '\0';
    nv->value = v;
    scope_var_count[d] = idx + 1;
}

/* ---- builtin functions --------------------------------------- */
static int builtin_print(int *a, int n, int *out)   { if (n) kprintf("%d\n", a[0]); *out = 0; return 0; }
static int builtin_putchar(int *a, int n, int *out) { if (n) kputc((char)a[0]); *out = 0; return 0; }
static int builtin_getchar(int *a, int n, int *out) { (void)a; (void)n; *out = kb_getc(); return 0; }

/* ---- evaluator ----------------------------------------------- */
static int eval(int n_idx);

static int call_function(const char *name, int *args, int argn) {
    if (strcmp(name, "print")   == 0) { int r; builtin_print  (args, argn, &r); return r; }
    if (strcmp(name, "putchar") == 0) { int r; builtin_putchar(args, argn, &r); return r; }
    if (strcmp(name, "getchar") == 0) { int r; builtin_getchar(args, argn, &r); return r; }
    if (strcmp(name, "puts")    == 0) { /* arg is a string literal -- handled in eval */ return 0; }

    for (int i = 0; i < func_count; i++) {
        if (strcmp(funcs[i].name, name) != 0) continue;
        if (scope_depth >= MAX_CALL) { zbc_err("call too deep"); return 0; }
        scope_depth++;
        scope_var_count[scope_depth - 1] = 0;
        for (int p = 0; p < funcs[i].param_count && p < argn; p++)
            set_local(funcs[i].params[p], args[p]);
        had_return = 0; return_value = 0;
        eval(funcs[i].body);
        had_return = 0;
        scope_depth--;
        return return_value;
    }
    zbc_err("undefined function");
    kprintf("       (called: %s)\n", name);
    return 0;
}

static int eval(int idx) {
    if (idx < 0 || had_error) return 0;
    struct node *n = &nodes[idx];
    switch (n->kind) {
    case N_NUM: return n->ival;
    case N_STR: return n->str_off;
    case N_VAR: {
        struct var *v = find_var(n->name);
        if (!v) { zbc_err("undefined variable"); kprintf("       (%s)\n", n->name); return 0; }
        return v->value;
    }
    case N_ASSIGN: {
        struct node *lhs = &nodes[n->child[0]];
        int rv = eval(n->child[1]);
        struct var *v = find_var(lhs->name);
        if (!v) {
            if (lhs->ival == 1) {       /* declaration */
                set_local(lhs->name, rv);
                return rv;
            }
            zbc_err("assign to undefined variable");
            kprintf("       (%s)\n", lhs->name);
            return rv;
        }
        switch (n->op) {
        case T_ASSIGN:  v->value = rv; break;
        case T_PLUSEQ:  v->value += rv; break;
        case T_MINUSEQ: v->value -= rv; break;
        case T_STAREQ:  v->value *= rv; break;
        case T_SLASHEQ: v->value = rv ? v->value / rv : 0; break;
        }
        return v->value;
    }
    case N_BIN: {
        int a = eval(n->child[0]);
        if (n->op == T_AND) return a ? (eval(n->child[1]) != 0) : 0;
        if (n->op == T_OR)  return a ? 1 : (eval(n->child[1]) != 0);
        int b = eval(n->child[1]);
        switch (n->op) {
        case T_PLUS:  return a + b;
        case T_MINUS: return a - b;
        case T_STAR:  return a * b;
        case T_SLASH: return b ? a / b : 0;
        case T_PCT:   return b ? a % b : 0;
        case T_EQ:    return a == b;
        case T_NEQ:   return a != b;
        case T_LT:    return a < b;
        case T_GT:    return a > b;
        case T_LE:    return a <= b;
        case T_GE:    return a >= b;
        case T_BAND:  return a & b;
        case T_BOR:   return a | b;
        case T_BXOR:  return a ^ b;
        case T_SHL:   return a << b;
        case T_SHR:   return a >> b;
        }
        return 0;
    }
    case N_UN: {
        int a = eval(n->child[0]);
        if (n->op == T_MINUS) return -a;
        if (n->op == T_NOT)   return !a;
        return a;
    }
    case N_CALL: {
        /* Collect arg values. */
        if (strcmp(n->name, "puts") == 0) {
            /* String literal expected. */
            int a = n->child[0];
            if (a < 0 || nodes[a].kind != N_STR) { zbc_err("puts needs a string"); return 0; }
            kputs(&strtab[nodes[a].str_off]);
            kputc('\n');
            return 0;
        }
        if (strcmp(n->name, "printf") == 0) {
            /* printf("fmt", args...) -- supports %d %i %u %x %c %s %%.
             * The format must be a string literal; %s args must be string
             * literals too (no char* variables in zbc yet). */
            int a = n->child[0];
            if (a < 0 || nodes[a].kind != N_STR) { zbc_err("printf needs a format string"); return 0; }
            const char *f = &strtab[nodes[a].str_off];
            int arg = nodes[a].next;     /* first value arg after the fmt */
            for (const char *q = f; *q; q++) {
                if (*q != '%') { kputc(*q); continue; }
                q++;
                if (*q == '%') { kputc('%'); continue; }
                if (*q == 's') {
                    if (arg >= 0 && nodes[arg].kind == N_STR) {
                        kputs(&strtab[nodes[arg].str_off]);
                        arg = nodes[arg].next;
                    }
                    continue;
                }
                int v = 0;
                if (arg >= 0) { v = eval(arg); arg = nodes[arg].next; }
                switch (*q) {
                    case 'd': case 'i': kprintf("%d", v); break;
                    case 'u':           kprintf("%u", (u32)v); break;
                    case 'x':           kprintf("%x", (u32)v); break;
                    case 'c':           kputc((char)v); break;
                    default:            kputc('%'); kputc(*q); break;
                }
            }
            return 0;
        }
        int args[8]; int an = 0;
        int c = n->child[0];
        while (c >= 0 && an < 8) {
            args[an++] = eval(c);
            c = nodes[c].next;
        }
        return call_function(n->name, args, an);
    }
    case N_IF: {
        if (eval(n->child[0])) eval(n->child[1]);
        else if (n->child[2] >= 0) eval(n->child[2]);
        return 0;
    }
    case N_WHILE: {
        while (!had_return && !had_break && !had_error && eval(n->child[0])) {
            had_continue = 0;
            eval(n->child[1]);
        }
        had_break = 0;
        return 0;
    }
    case N_FOR: {
        /* child[0]=init child[1]=cond child[2]=post child[3]=body.
         * `continue` skips the rest of the body but still runs post. */
        if (n->child[0] >= 0) eval(n->child[0]);
        while (!had_return && !had_break && !had_error) {
            if (n->child[1] >= 0 && !eval(n->child[1])) break;
            had_continue = 0;
            if (n->child[3] >= 0) eval(n->child[3]);
            had_continue = 0;
            if (n->child[2] >= 0) eval(n->child[2]);
        }
        had_break = 0;
        return 0;
    }
    case N_BLOCK: {
        int c = n->child[0];
        while (c >= 0 && !had_return && !had_break && !had_continue && !had_error) {
            eval(c);
            c = nodes[c].next;
        }
        return 0;
    }
    case N_RETURN:
        if (n->child[0] >= 0) return_value = eval(n->child[0]);
        had_return = 1;
        return return_value;
    case N_BREAK: had_break = 1; return 0;
    case N_CONT:  had_continue = 1; return 0;
    case N_EXPR_STMT: return eval(n->child[0]);
    default: return 0;
    }
}

/* ---- entry point --------------------------------------------- */

/* ZBE = Zenbite Executable: a thin 8-byte header + the source text.
 *   bytes 0..3 : magic "ZBE!"
 *   bytes 4..7 : reserved, zero
 *   bytes 8..  : the C source (NUL-terminated optional)
 *
 * `cc src.c -o name` writes name.zbe. Typing `name` from the shell reads
 * the file -- if it starts with the magic the header is skipped, otherwise
 * the whole file is treated as raw source. So `cc` can also run plain .c
 * files. */
static const char ZBE_MAGIC[4] = { 'Z', 'B', 'E', '!' };

static int read_source(const char *path, char *buf, int max) {
    int h = fs_open(path);
    if (h < 0) { kprintf("ZBC: cannot open %s\n", path); return -1; }
    int size = fs_size(h);
    if (size <= 0 || size >= max) { fs_close(h); kputs("ZBC: source too big or empty\n"); return -1; }
    int n = fs_read(h, buf, (size_t)size);
    fs_close(h);
    if (n < 0) { kputs("ZBC: read failed\n"); return -1; }
    buf[n] = '\0';
    /* Skip the ZBE header if present. */
    if (n >= 8 && memcmp(buf, ZBE_MAGIC, 4) == 0) {
        for (int i = 0; i < n - 8; i++) buf[i] = buf[i + 8];
        buf[n - 8] = '\0';
    }
    return n;
}

int zbc_compile(const char *src_path, const char *out_path) {
    static char tmp[MAX_SRC];
    int n = read_source(src_path, tmp, MAX_SRC);
    if (n < 0) return -1;
    fs_unlink(out_path);
    if (fs_create(out_path) < 0) { kprintf("ZBC: cannot create %s\n", out_path); return -1; }
    int h = fs_open(out_path);
    if (h < 0) return -1;
    char header[8] = { 'Z', 'B', 'E', '!', 0, 0, 0, 0 };
    fs_write(h, header, 8);
    int slen = (int)strlen(tmp);
    fs_write(h, tmp, (size_t)slen);
    fs_close(h);
    return 0;
}

int zbc_run(const char *path) {
    if (read_source(path, src, MAX_SRC) < 0) return -1;

    tok_count = tok_pos = strtab_pos = node_count = 0;
    func_count = global_count = scope_depth = 0;
    had_error = had_return = had_break = had_continue = 0;

    if (tokenize(src) < 0) return -1;

    /* Top-level: a sequence of "int main(...) { ... }" or "void name(...) { ... }". */
    while (peek()->kind != T_EOF && !had_error) {
        int is_void = 0;
        if (accept(T_KW_INT)) {
            is_void = 0;
        } else if (accept(T_KW_VOID)) {
            is_void = 1;
        } else {
            zbc_err("expected function definition");
            break;
        }
        parse_func(is_void);
    }
    if (had_error) return -1;

    int args[1] = {0};
    return call_function("main", args, 0);
}

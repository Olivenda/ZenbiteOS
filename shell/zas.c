/* ZAS - Zenbite Assembler: two-pass NASM-compatible x86 assembler -> ELF32.
 * Supports a useful subset of x86 32-bit instructions.
 * VMA for .text starts at 0x00800000 (8 MB).
 */
#include "kernel.h"
#include "kio.h"
#include "string.h"
#include "fs.h"
#include "elf.h"
#include "types.h"

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */
#define ZAS_SRC_MAX    8192
#define ZAS_OUT_MAX    16384
#define ZAS_LABEL_MAX  128
#define ZAS_FIXUP_MAX  256

#define ZAS_TEXT_VMA   0x00800000u

/* -------------------------------------------------------------------------
 * Token types
 * ---------------------------------------------------------------------- */
enum {
    TOK_EOF, TOK_NEWLINE, TOK_IDENT, TOK_NUMBER, TOK_STRING,
    TOK_COMMA, TOK_LBRACKET, TOK_RBRACKET, TOK_PLUS, TOK_MINUS,
    TOK_COLON, TOK_STAR
};

typedef struct {
    int  type;
    char text[64];
    u32  num;
} Token;

/* -------------------------------------------------------------------------
 * Assembler state
 * ---------------------------------------------------------------------- */
static char  zas_src[ZAS_SRC_MAX];
static u8    zas_out[ZAS_OUT_MAX];

/* Labels */
static char zas_label_names[ZAS_LABEL_MAX][64];
static u32  zas_label_addrs[ZAS_LABEL_MAX];
static int  zas_label_count;

/* Fixups: locations in zas_out needing a label reference patched in */
typedef struct {
    u32  out_offset;    /* offset in zas_out where the 4-byte value goes */
    char name[64];      /* label name */
    u32  base;          /* base address: fixup value = label_addr - (base + 4) */
    int  is_rel;        /* 1 = relative (jmp/call), 0 = absolute */
} Fixup;

static Fixup zas_fixups[ZAS_FIXUP_MAX];
static int   zas_fixup_count;

/* Sections */
enum { SEC_TEXT, SEC_DATA, SEC_BSS };
static int  cur_sec;
static u32  text_size, data_size, bss_size;
static u32  text_out_start; /* start offset in zas_out for .text */
static u32  data_out_start; /* start offset in zas_out for .data */
static u32  out_pos;        /* current write pos in zas_out (text+data) */

/* Entry point */
static u32  zas_entry;
static int  zas_entry_set;

/* %define substitutions */
#define ZAS_DEF_MAX  32
static char def_names[ZAS_DEF_MAX][64];
static char def_vals [ZAS_DEF_MAX][64];
static int  def_count;

/* -------------------------------------------------------------------------
 * Lexer
 * ---------------------------------------------------------------------- */
static const char *lex_p;
static int         lex_line;

static void lex_skip_ws(void) {
    while (*lex_p == ' ' || *lex_p == '\t' || *lex_p == '\r') lex_p++;
    if (*lex_p == ';') { while (*lex_p && *lex_p != '\n') lex_p++; }
}

static Token lex_next(void) {
    Token t;
    t.text[0] = '\0';
    t.num = 0;
    lex_skip_ws();
    if (!*lex_p) { t.type = TOK_EOF; return t; }
    if (*lex_p == '\n') { lex_p++; lex_line++; t.type = TOK_NEWLINE; return t; }
    if (*lex_p == ',') { lex_p++; t.type = TOK_COMMA; return t; }
    if (*lex_p == '[') { lex_p++; t.type = TOK_LBRACKET; return t; }
    if (*lex_p == ']') { lex_p++; t.type = TOK_RBRACKET; return t; }
    if (*lex_p == '+') { lex_p++; t.type = TOK_PLUS; return t; }
    if (*lex_p == '-') { lex_p++; t.type = TOK_MINUS; return t; }
    if (*lex_p == ':') { lex_p++; t.type = TOK_COLON; return t; }
    if (*lex_p == '*') { lex_p++; t.type = TOK_STAR; return t; }
    /* String literal */
    if (*lex_p == '"') {
        lex_p++;
        int i = 0;
        while (*lex_p && *lex_p != '"' && i < 63) {
            if (*lex_p == '\\') {
                lex_p++;
                char ec = *lex_p++;
                if (ec == 'n') t.text[i++] = '\n';
                else if (ec == 't') t.text[i++] = '\t';
                else if (ec == '0') t.text[i++] = '\0';
                else t.text[i++] = ec;
            } else {
                t.text[i++] = *lex_p++;
            }
        }
        if (*lex_p == '"') lex_p++;
        t.text[i] = '\0';
        t.type = TOK_STRING;
        return t;
    }
    /* Number */
    if (*lex_p >= '0' && *lex_p <= '9') {
        u32 v = 0;
        if (*lex_p == '0' && (*(lex_p+1) == 'x' || *(lex_p+1) == 'X')) {
            lex_p += 2;
            while ((*lex_p >= '0' && *lex_p <= '9') ||
                   (*lex_p >= 'a' && *lex_p <= 'f') ||
                   (*lex_p >= 'A' && *lex_p <= 'F')) {
                char ch = *lex_p++;
                if (ch >= '0' && ch <= '9') v = v*16 + (u32)(ch-'0');
                else if (ch >= 'a' && ch <= 'f') v = v*16 + (u32)(ch-'a'+10);
                else v = v*16 + (u32)(ch-'A'+10);
            }
        } else {
            while (*lex_p >= '0' && *lex_p <= '9') v = v*10 + (u32)(*lex_p++ - '0');
        }
        t.type = TOK_NUMBER; t.num = v;
        return t;
    }
    /* Identifier */
    if ((*lex_p >= 'a' && *lex_p <= 'z') || (*lex_p >= 'A' && *lex_p <= 'Z') ||
        *lex_p == '_' || *lex_p == '.' || *lex_p == '%') {
        int i = 0;
        while (((*lex_p >= 'a' && *lex_p <= 'z') || (*lex_p >= 'A' && *lex_p <= 'Z') ||
                (*lex_p >= '0' && *lex_p <= '9') || *lex_p == '_' || *lex_p == '.') && i < 63) {
            t.text[i++] = *lex_p++;
        }
        t.text[i] = '\0';
        t.type = TOK_IDENT;
        return t;
    }
    /* Skip unknown char */
    lex_p++;
    t.type = TOK_IDENT;
    t.text[0] = '?';
    t.text[1] = '\0';
    return t;
}

/* Peek at next token without consuming */
static Token lex_peek(void) {
    const char *save = lex_p;
    int save_line = lex_line;
    Token t = lex_next();
    lex_p = save;
    lex_line = save_line;
    return t;
}

/* -------------------------------------------------------------------------
 * Register lookup
 * ---------------------------------------------------------------------- */
static int reg32_encode(const char *s) {
    static const char *r[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
    char lo[8]; int i;
    for (i = 0; s[i] && i < 7; i++) lo[i] = (char)(s[i] | 0x20);
    lo[i] = '\0';
    for (i = 0; i < 8; i++) if (strcmp(lo, r[i]) == 0) return i;
    return -1;
}

/* -------------------------------------------------------------------------
 * Label / fixup helpers
 * ---------------------------------------------------------------------- */
static int label_find(const char *name) {
    for (int i = 0; i < zas_label_count; i++)
        if (strcmp(zas_label_names[i], name) == 0) return i;
    return -1;
}

static void label_define(const char *name, u32 addr) {
    int i = label_find(name);
    if (i >= 0) { zas_label_addrs[i] = addr; return; }
    if (zas_label_count >= ZAS_LABEL_MAX) return;
    strncpy(zas_label_names[zas_label_count], name, 63);
    zas_label_names[zas_label_count][63] = '\0';
    zas_label_addrs[zas_label_count] = addr;
    zas_label_count++;
}

static void add_fixup(u32 out_off, const char *name, u32 base, int is_rel) {
    if (zas_fixup_count >= ZAS_FIXUP_MAX) return;
    zas_fixups[zas_fixup_count].out_offset = out_off;
    strncpy(zas_fixups[zas_fixup_count].name, name, 63);
    zas_fixups[zas_fixup_count].name[63] = '\0';
    zas_fixups[zas_fixup_count].base = base;
    zas_fixups[zas_fixup_count].is_rel = is_rel;
    zas_fixup_count++;
}

/* -------------------------------------------------------------------------
 * Output helpers
 * ---------------------------------------------------------------------- */
static void emit8(u8 b) {
    if (out_pos < ZAS_OUT_MAX) zas_out[out_pos++] = b;
}
static void emit32(u32 v) {
    emit8((u8)(v));
    emit8((u8)(v >> 8));
    emit8((u8)(v >> 16));
    emit8((u8)(v >> 24));
}

/* Current VMA based on section */
static u32 cur_vma(void) {
    if (cur_sec == SEC_TEXT) return ZAS_TEXT_VMA + (out_pos - text_out_start);
    if (cur_sec == SEC_DATA) {
        u32 data_vma = ZAS_TEXT_VMA + text_size;
        data_vma = (data_vma + 3) & ~3u;
        return data_vma + (out_pos - data_out_start);
    }
    /* BSS */
    u32 bss_vma = ZAS_TEXT_VMA + text_size;
    bss_vma = (bss_vma + 3) & ~3u;
    bss_vma += data_size;
    return bss_vma;
}

/* -------------------------------------------------------------------------
 * ModRM helpers
 * ---------------------------------------------------------------------- */
static void emit_modrm_rr(u8 opcode, int reg, int rm) {
    emit8(opcode);
    emit8((u8)(0xC0 | (reg << 3) | rm));
}
static void emit_modrm_mem(u8 opcode, int reg, int base, int disp, int has_disp) {
    emit8(opcode);
    if (!has_disp) {
        emit8((u8)((0 << 6) | (reg << 3) | base));
        if (base == 5) { emit8(0); }
    } else {
        if (disp >= -128 && disp <= 127) {
            emit8((u8)((1 << 6) | (reg << 3) | base));
            emit8((u8)(i8)disp);
        } else {
            emit8((u8)((2 << 6) | (reg << 3) | base));
            emit32((u32)(i32)disp);
        }
    }
}

/* -------------------------------------------------------------------------
 * %define lookup
 * ---------------------------------------------------------------------- */
static const char *def_lookup(const char *name) {
    for (int i = 0; i < def_count; i++)
        if (strcmp(def_names[i], name) == 0) return def_vals[i];
    return (const char *)0;
}

/* -------------------------------------------------------------------------
 * Expression parser
 * ---------------------------------------------------------------------- */
static int parse_expr(u32 *out_val, char *out_label, int max_label) {
    Token t = lex_peek();
    if (t.type == TOK_NUMBER) {
        lex_next();
        *out_val = t.num;
        if (out_label) out_label[0] = '\0';
        return 1;
    }
    if (t.type == TOK_MINUS) {
        lex_next();
        Token t2 = lex_next();
        if (t2.type == TOK_NUMBER) {
            *out_val = (u32)(-(i32)t2.num);
            if (out_label) out_label[0] = '\0';
            return 1;
        }
        *out_val = 0;
        return 0;
    }
    if (t.type == TOK_IDENT) {
        lex_next();
        const char *dv = def_lookup(t.text);
        if (dv) {
            u32 v = 0;
            const char *p = dv;
            if (p[0]=='0' && (p[1]=='x'||p[1]=='X')) {
                p+=2;
                while (*p) {
                    if (*p>='0'&&*p<='9') v=v*16+(u32)(*p-'0');
                    else if (*p>='a'&&*p<='f') v=v*16+(u32)(*p-'a'+10);
                    else if (*p>='A'&&*p<='F') v=v*16+(u32)(*p-'A'+10);
                    p++;
                }
            } else {
                while (*p>='0'&&*p<='9') v=v*10+(u32)(*p++-'0');
            }
            *out_val = v;
            if (out_label) out_label[0] = '\0';
            return 1;
        }
        int li = label_find(t.text);
        if (li >= 0) { *out_val = zas_label_addrs[li]; if (out_label) out_label[0]='\0'; return 1; }
        if (out_label) { strncpy(out_label, t.text, max_label-1); out_label[max_label-1]='\0'; }
        *out_val = 0;
        return 0;
    }
    *out_val = 0;
    return 0;
}

/* -------------------------------------------------------------------------
 * Skip to end of current logical line
 * ---------------------------------------------------------------------- */
static void skip_line(void) {
    for (;;) {
        Token t = lex_peek();
        if (t.type == TOK_NEWLINE || t.type == TOK_EOF) return;
        lex_next();
    }
}

static void skip_newlines(void) {
    for (;;) {
        Token t = lex_peek();
        if (t.type == TOK_NEWLINE) lex_next();
        else break;
    }
}

/* Parse a memory operand already past '['. Returns base reg. */
static int parse_mem_op(int *disp, int *has_disp) {
    *disp = 0; *has_disp = 0;
    Token t = lex_next();
    int base = reg32_encode(t.text);
    if (base < 0) base = 0;
    Token t2 = lex_peek();
    if (t2.type == TOK_PLUS) {
        lex_next();
        Token t3 = lex_next();
        if (t3.type == TOK_NUMBER) { *disp = (int)t3.num; *has_disp = 1; }
    } else if (t2.type == TOK_MINUS) {
        lex_next();
        Token t3 = lex_next();
        if (t3.type == TOK_NUMBER) { *disp = -(int)t3.num; *has_disp = 1; }
    }
    t2 = lex_peek();
    if (t2.type == TOK_RBRACKET) lex_next();
    return base;
}

/* -------------------------------------------------------------------------
 * Core instruction emitter / assembler
 * ---------------------------------------------------------------------- */
static int assemble_line(int pass) {
    skip_newlines();
    Token t = lex_peek();
    if (t.type == TOK_EOF) return 0;
    if (t.type == TOK_NEWLINE) { lex_next(); return 1; }

    t = lex_next();
    if (t.type == TOK_EOF) return 0;

    /* %define */
    if (strcmp(t.text, "%define") == 0) {
        Token name_t = lex_next();
        Token val_t  = lex_next();
        if (pass == 1 && def_count < ZAS_DEF_MAX) {
            strncpy(def_names[def_count], name_t.text, 63);
            strncpy(def_vals [def_count], val_t.text,  63);
            def_count++;
        }
        skip_line();
        return 1;
    }

    /* section directive */
    if (strcasecmp(t.text, "section") == 0) {
        Token sec = lex_next();
        if (strcmp(sec.text, ".text") == 0) {
            cur_sec = SEC_TEXT;
        } else if (strcmp(sec.text, ".data") == 0) {
            if (pass == 1 && cur_sec == SEC_TEXT) text_size = out_pos - text_out_start;
            cur_sec = SEC_DATA;
        } else if (strcmp(sec.text, ".bss") == 0) {
            if (pass == 1) {
                if (cur_sec == SEC_TEXT) text_size = out_pos - text_out_start;
                else if (cur_sec == SEC_DATA) data_size = out_pos - data_out_start;
            }
            cur_sec = SEC_BSS;
        }
        skip_line();
        return 1;
    }

    /* global directive */
    if (strcasecmp(t.text, "global") == 0) {
        skip_line();
        return 1;
    }

    /* times N db X */
    if (strcasecmp(t.text, "times") == 0) {
        u32 count = 0; char lbl[64]; lbl[0]='\0';
        parse_expr(&count, lbl, sizeof lbl);
        Token dir = lex_next();
        if (strcasecmp(dir.text, "db") == 0) {
            u32 val = 0; char lb2[64]; lb2[0]='\0';
            parse_expr(&val, lb2, sizeof lb2);
            for (u32 i = 0; i < count; i++) emit8((u8)val);
        }
        skip_line();
        return 1;
    }

    /* db / dw / dd */
    if (strcasecmp(t.text, "db") == 0) {
        for (;;) {
            Token peek = lex_peek();
            if (peek.type == TOK_NEWLINE || peek.type == TOK_EOF) break;
            if (peek.type == TOK_STRING) {
                lex_next();
                for (int i = 0; peek.text[i]; i++) emit8((u8)peek.text[i]);
            } else {
                u32 val = 0; char lb[64]; lb[0]='\0';
                parse_expr(&val, lb, sizeof lb);
                emit8((u8)val);
            }
            peek = lex_peek();
            if (peek.type == TOK_COMMA) lex_next(); else break;
        }
        skip_line();
        return 1;
    }
    if (strcasecmp(t.text, "dw") == 0) {
        u32 val = 0; char lb[64]; lb[0]='\0';
        parse_expr(&val, lb, sizeof lb);
        emit8((u8)val); emit8((u8)(val>>8));
        skip_line();
        return 1;
    }
    if (strcasecmp(t.text, "dd") == 0) {
        u32 val = 0; char lb[64]; lb[0]='\0';
        int known = parse_expr(&val, lb, sizeof lb);
        if (!known && pass == 2) {
            add_fixup(out_pos, lb, 0, 0);
        }
        emit32(val);
        skip_line();
        return 1;
    }

    /* resb N */
    if (strcasecmp(t.text, "resb") == 0) {
        u32 count = 0; char lb[64]; lb[0]='\0';
        parse_expr(&count, lb, sizeof lb);
        if (cur_sec == SEC_BSS) bss_size += count;
        else for (u32 i = 0; i < count; i++) emit8(0);
        skip_line();
        return 1;
    }

    /* label: */
    Token peek_colon = lex_peek();
    if (peek_colon.type == TOK_COLON) {
        lex_next(); /* consume ':' */
        u32 addr = cur_vma();
        if (pass == 1) {
            label_define(t.text, addr);
            if (strcmp(t.text, "_start") == 0) {
                zas_entry = addr; zas_entry_set = 1;
            }
            if (!zas_entry_set) {
                zas_entry = addr; zas_entry_set = 1;
            }
        }
        return 1;
    }

    /* -----------------------------------------------------------------------
     * Instructions
     * -------------------------------------------------------------------- */
    char mne[64];
    strncpy(mne, t.text, 63); mne[63]='\0';

    /* Simple single-byte opcodes */
    if (strcasecmp(mne, "nop")   == 0) { emit8(0x90); skip_line(); return 1; }
    if (strcasecmp(mne, "hlt")   == 0) { emit8(0xF4); skip_line(); return 1; }
    if (strcasecmp(mne, "ret")   == 0) { emit8(0xC3); skip_line(); return 1; }
    if (strcasecmp(mne, "leave") == 0) { emit8(0xC9); skip_line(); return 1; }
    if (strcasecmp(mne, "pusha") == 0) { emit8(0x60); skip_line(); return 1; }
    if (strcasecmp(mne, "popa")  == 0) { emit8(0x61); skip_line(); return 1; }
    if (strcasecmp(mne, "pushf") == 0) { emit8(0x9C); skip_line(); return 1; }
    if (strcasecmp(mne, "popf")  == 0) { emit8(0x9D); skip_line(); return 1; }
    if (strcasecmp(mne, "sti")   == 0) { emit8(0xFB); skip_line(); return 1; }
    if (strcasecmp(mne, "cli")   == 0) { emit8(0xFA); skip_line(); return 1; }
    if (strcasecmp(mne, "cld")   == 0) { emit8(0xFC); skip_line(); return 1; }
    if (strcasecmp(mne, "std")   == 0) { emit8(0xFD); skip_line(); return 1; }

    /* INT imm8 */
    if (strcasecmp(mne, "int") == 0) {
        Token it = lex_next();
        u8 ib = (u8)it.num;
        emit8(0xCD); emit8(ib);
        skip_line(); return 1;
    }

    /* PUSH */
    if (strcasecmp(mne, "push") == 0) {
        Token op = lex_peek();
        if (op.type == TOK_IDENT) {
            lex_next();
            int rd = reg32_encode(op.text);
            if (rd >= 0) { emit8((u8)(0x50 + rd)); skip_line(); return 1; }
            /* label or define */
            u32 val = 0;
            int li = label_find(op.text);
            if (li >= 0) val = zas_label_addrs[li];
            emit8(0x68); emit32(val);
            skip_line(); return 1;
        }
        if (op.type == TOK_NUMBER) {
            lex_next();
            u32 v = op.num;
            if (v <= 127) { emit8(0x6A); emit8((u8)v); }
            else { emit8(0x68); emit32(v); }
            skip_line(); return 1;
        }
        if (op.type == TOK_MINUS) {
            lex_next();
            Token neg = lex_next();
            i32 sv = -(i32)neg.num;
            emit8(0x68); emit32((u32)sv);
            skip_line(); return 1;
        }
        skip_line(); return 1;
    }

    /* POP r32 */
    if (strcasecmp(mne, "pop") == 0) {
        Token op = lex_next();
        int rd = reg32_encode(op.text);
        if (rd >= 0) emit8((u8)(0x58 + rd));
        skip_line(); return 1;
    }

    /* INC / DEC r32 */
    if (strcasecmp(mne, "inc") == 0) {
        Token op = lex_next();
        int rd = reg32_encode(op.text);
        if (rd >= 0) emit8((u8)(0x40 + rd));
        skip_line(); return 1;
    }
    if (strcasecmp(mne, "dec") == 0) {
        Token op = lex_next();
        int rd = reg32_encode(op.text);
        if (rd >= 0) emit8((u8)(0x48 + rd));
        skip_line(); return 1;
    }

    /* NEG, NOT, MUL, IMUL, IDIV, DIV: F7 /n r32 */
    if (strcasecmp(mne, "neg")  == 0) {
        Token op = lex_next();
        int rm = reg32_encode(op.text);
        if (rm >= 0) emit_modrm_rr(0xF7, 3, rm);
        skip_line(); return 1;
    }
    if (strcasecmp(mne, "not")  == 0) {
        Token op = lex_next();
        int rm = reg32_encode(op.text);
        if (rm >= 0) emit_modrm_rr(0xF7, 2, rm);
        skip_line(); return 1;
    }
    if (strcasecmp(mne, "mul")  == 0) {
        Token op = lex_next();
        int rm = reg32_encode(op.text);
        if (rm >= 0) emit_modrm_rr(0xF7, 4, rm);
        skip_line(); return 1;
    }
    if (strcasecmp(mne, "imul") == 0) {
        Token op = lex_next();
        int rm = reg32_encode(op.text);
        if (rm >= 0) emit_modrm_rr(0xF7, 5, rm);
        skip_line(); return 1;
    }
    if (strcasecmp(mne, "idiv") == 0) {
        Token op = lex_next();
        int rm = reg32_encode(op.text);
        if (rm >= 0) emit_modrm_rr(0xF7, 7, rm);
        skip_line(); return 1;
    }
    if (strcasecmp(mne, "div")  == 0) {
        Token op = lex_next();
        int rm = reg32_encode(op.text);
        if (rm >= 0) emit_modrm_rr(0xF7, 6, rm);
        skip_line(); return 1;
    }

    /* XCHG r32, r32 */
    if (strcasecmp(mne, "xchg") == 0) {
        Token op1 = lex_next();
        Token com = lex_next(); (void)com;
        Token op2 = lex_next();
        int r1 = reg32_encode(op1.text), r2 = reg32_encode(op2.text);
        if (r1 >= 0 && r2 >= 0) emit_modrm_rr(0x87, r1, r2);
        skip_line(); return 1;
    }

    /* MOV */
    if (strcasecmp(mne, "mov") == 0) {
        Token op1 = lex_next();
        lex_next(); /* comma */
        Token op2 = lex_peek();

        int r1 = reg32_encode(op1.text);
        /* MOV [mem], r32 */
        if (op1.type == TOK_LBRACKET) {
            int disp, has_disp;
            int base = parse_mem_op(&disp, &has_disp);
            /* comma already skipped? no -- parse_mem_op consumed ']'.
             * The comma was the one we lex_next()'d as the second token above.
             * But op1 was '[', so we need to read the comma again. */
            Token src = lex_next(); /* r32 source */
            int rs = reg32_encode(src.text);
            if (rs >= 0) emit_modrm_mem(0x89, rs, base, disp, has_disp);
            skip_line(); return 1;
        }

        if (r1 >= 0) {
            /* MOV r32, [mem] */
            if (op2.type == TOK_LBRACKET) {
                lex_next(); /* consume '[' */
                int disp, has_disp;
                int base = parse_mem_op(&disp, &has_disp);
                emit_modrm_mem(0x8B, r1, base, disp, has_disp);
                skip_line(); return 1;
            }
            /* MOV r32, r32 or MOV r32, imm/label */
            if (op2.type == TOK_IDENT) {
                lex_next();
                int r2 = reg32_encode(op2.text);
                if (r2 >= 0) { emit_modrm_rr(0x8B, r1, r2); skip_line(); return 1; }
                /* MOV r32, label/define */
                u32 val = 0;
                const char *dv = def_lookup(op2.text);
                if (dv) {
                    const char *p=dv;
                    if (p[0]=='0'&&(p[1]=='x'||p[1]=='X')){ p+=2; while(*p){if(*p>='0'&&*p<='9')val=val*16+(u32)(*p-'0'); else if(*p>='a'&&*p<='f')val=val*16+(u32)(*p-'a'+10); else if(*p>='A'&&*p<='F')val=val*16+(u32)(*p-'A'+10); p++;} }
                    else { while(*p>='0'&&*p<='9')val=val*10+(u32)(*p++-'0'); }
                    emit8((u8)(0xB8+r1)); emit32(val);
                    skip_line(); return 1;
                }
                int li = label_find(op2.text);
                if (li >= 0) val = zas_label_addrs[li];
                else if (pass == 2) add_fixup(out_pos+1, op2.text, 0, 0);
                emit8((u8)(0xB8+r1)); emit32(val);
                skip_line(); return 1;
            }
            if (op2.type == TOK_NUMBER) {
                lex_next();
                emit8((u8)(0xB8+r1)); emit32(op2.num);
                skip_line(); return 1;
            }
            if (op2.type == TOK_MINUS) {
                lex_next();
                Token neg = lex_next();
                emit8((u8)(0xB8+r1)); emit32((u32)(-(i32)neg.num));
                skip_line(); return 1;
            }
        }
        skip_line(); return 1;
    }

    /* LEA r32, [r32+disp] */
    if (strcasecmp(mne, "lea") == 0) {
        Token dst = lex_next();
        lex_next(); /* comma */
        int r1 = reg32_encode(dst.text);
        Token lb_t = lex_next(); /* should be '[' */
        if (lb_t.type == TOK_LBRACKET) {
            int disp, has_disp;
            int base = parse_mem_op(&disp, &has_disp);
            if (r1 >= 0) emit_modrm_mem(0x8D, r1, base, disp, has_disp);
        }
        skip_line(); return 1;
    }

    /* Two-operand arithmetic: ADD, SUB, XOR, AND, OR, CMP */
    {
        static const struct {
            const char *mn;
            u8 op_rm;
            int imm8_n;
            int imm32_n;
        } arith_ops[] = {
            {"add", 0x03, 0, 0},
            {"sub", 0x2B, 5, 5},
            {"xor", 0x33, 6, 6},
            {"and", 0x23, 4, 4},
            {"or",  0x0B, 1, 1},
            {"cmp", 0x3B, 7, 7},
        };
        size_t _ai;
        for (_ai = 0; _ai < sizeof(arith_ops)/sizeof(arith_ops[0]); _ai++) {
            if (strcasecmp(mne, arith_ops[_ai].mn) == 0) {
                Token op1 = lex_next(); lex_next(); /* comma */
                Token op2 = lex_peek();
                int r1 = reg32_encode(op1.text);
                if (r1 >= 0) {
                    if (op2.type == TOK_IDENT) {
                        lex_next();
                        int r2 = reg32_encode(op2.text);
                        if (r2 >= 0) { emit_modrm_rr(arith_ops[_ai].op_rm, r1, r2); skip_line(); return 1; }
                    }
                    if (op2.type == TOK_NUMBER) {
                        u32 v;
                        lex_next();
                        v = op2.num;
                        if (v <= 127) {
                            emit8(0x83);
                            emit8((u8)(0xC0 | ((u8)arith_ops[_ai].imm8_n << 3) | r1));
                            emit8((u8)v);
                        } else {
                            emit8(0x81);
                            emit8((u8)(0xC0 | ((u8)arith_ops[_ai].imm32_n << 3) | r1));
                            emit32(v);
                        }
                        skip_line(); return 1;
                    }
                    if (op2.type == TOK_MINUS) {
                        Token neg_tok;
                        i32 sv;
                        lex_next();
                        neg_tok = lex_next();
                        sv = -(i32)neg_tok.num;
                        if (sv >= -128) {
                            emit8(0x83);
                            emit8((u8)(0xC0 | ((u8)arith_ops[_ai].imm8_n << 3) | r1));
                            emit8((u8)(i8)sv);
                        } else {
                            emit8(0x81);
                            emit8((u8)(0xC0 | ((u8)arith_ops[_ai].imm32_n << 3) | r1));
                            emit32((u32)sv);
                        }
                        skip_line(); return 1;
                    }
                }
                skip_line(); return 1;
            }
        }
    }

    /* JMP rel32 */
    if (strcasecmp(mne, "jmp") == 0) {
        Token op = lex_next();
        if (op.type == TOK_IDENT) {
            int li = label_find(op.text);
            int known = (li >= 0);
            u32 addr = known ? zas_label_addrs[li] : 0;
            emit8(0xE9);
            u32 off_pos = out_pos;
            if (known && pass == 2) {
                u32 next_ip = cur_vma() + 4;
                emit32(addr - next_ip);
            } else {
                if (pass == 2) add_fixup(off_pos, op.text, cur_vma()+4, 1);
                emit32(0);
            }
        } else if (op.type == TOK_NUMBER) {
            emit8(0xE9);
            u32 next_ip = cur_vma() + 4;
            emit32(op.num - next_ip);
        }
        skip_line(); return 1;
    }

    /* CALL rel32 */
    if (strcasecmp(mne, "call") == 0) {
        Token op = lex_next();
        if (op.type == TOK_IDENT) {
            int li = label_find(op.text);
            int known = (li >= 0);
            u32 addr = known ? zas_label_addrs[li] : 0;
            emit8(0xE8);
            u32 off_pos = out_pos;
            if (known && pass == 2) {
                u32 next_ip = cur_vma() + 4;
                emit32(addr - next_ip);
            } else {
                if (pass == 2) add_fixup(off_pos, op.text, cur_vma()+4, 1);
                emit32(0);
            }
        }
        skip_line(); return 1;
    }

    /* Conditional jumps */
    {
        static const struct {
            const char *mn;
            u8 cc;
        } jcc[] = {
            {"je",   0x84}, {"jz",   0x84},
            {"jne",  0x85}, {"jnz",  0x85},
            {"jl",   0x8C}, {"jnge", 0x8C},
            {"jle",  0x8E}, {"jng",  0x8E},
            {"jg",   0x8F}, {"jnle", 0x8F},
            {"jge",  0x8D}, {"jnl",  0x8D},
            {"ja",   0x87},
            {"jb",   0x82},
            {"jae",  0x83},
            {"jbe",  0x86},
        };
        size_t _ji;
        for (_ji = 0; _ji < sizeof(jcc)/sizeof(jcc[0]); _ji++) {
            if (strcasecmp(mne, jcc[_ji].mn) == 0) {
                Token op = lex_next();
                if (op.type == TOK_IDENT) {
                    int li = label_find(op.text);
                    int known = (li >= 0);
                    u32 addr = known ? zas_label_addrs[li] : 0;
                    emit8(0x0F); emit8(jcc[_ji].cc);
                    u32 off_pos = out_pos;
                    if (known && pass == 2) {
                        u32 ni = cur_vma() + 4;
                        emit32(addr - ni);
                    } else {
                        if (pass == 2) add_fixup(off_pos, op.text, cur_vma()+4, 1);
                        emit32(0);
                    }
                }
                skip_line(); return 1;
            }
        }
    }

    /* Unknown instruction: skip line */
    skip_line();
    return 1;
}

/* -------------------------------------------------------------------------
 * Two-pass assembly
 * ---------------------------------------------------------------------- */
static int run_pass(int pass) {
    if (pass == 2) {
        out_pos = 0;
        text_out_start = 0;
        data_out_start = text_size;
        out_pos = 0;
    } else {
        out_pos = 0;
        text_out_start = 0;
        data_out_start = 0;
        text_size = 0;
        data_size = 0;
        bss_size  = 0;
    }
    cur_sec = SEC_TEXT;
    lex_p = zas_src;
    lex_line = 1;
    while (*lex_p) {
        if (!assemble_line(pass)) break;
    }
    if (cur_sec == SEC_TEXT)      text_size = out_pos - text_out_start;
    else if (cur_sec == SEC_DATA) data_size = out_pos - data_out_start;
    return 0;
}

/* -------------------------------------------------------------------------
 * Apply fixups
 * ---------------------------------------------------------------------- */
static void apply_fixups(void) {
    int i;
    for (i = 0; i < zas_fixup_count; i++) {
        Fixup *fx = &zas_fixups[i];
        int li = label_find(fx->name);
        if (li < 0) { kprintf("zas: undefined label '%s'\n", fx->name); continue; }
        u32 addr = zas_label_addrs[li];
        u32 val;
        if (fx->is_rel) {
            val = addr - fx->base;
        } else {
            val = addr;
        }
        u32 off = fx->out_offset;
        if (off + 4 <= ZAS_OUT_MAX) {
            zas_out[off+0] = (u8)(val);
            zas_out[off+1] = (u8)(val>>8);
            zas_out[off+2] = (u8)(val>>16);
            zas_out[off+3] = (u8)(val>>24);
        }
    }
}

/* -------------------------------------------------------------------------
 * ELF32 output
 * ---------------------------------------------------------------------- */
static int write_elf(const char *out_path) {
    u32 hdr_size = 52 + 32;
    u32 code_size = text_size + data_size;

    /* Build ELF header */
    u8 ehdr_buf[52];
    memset(ehdr_buf, 0, sizeof ehdr_buf);
    ehdr_buf[0] = 0x7F; ehdr_buf[1]='E'; ehdr_buf[2]='L'; ehdr_buf[3]='F';
    ehdr_buf[4] = 1;    /* ELFCLASS32 */
    ehdr_buf[5] = 1;    /* ELFDATA2LSB */
    ehdr_buf[6] = 1;    /* version */
    /* e_type = ET_EXEC = 2 */
    ehdr_buf[16] = 2; ehdr_buf[17] = 0;
    /* e_machine = EM_386 = 3 */
    ehdr_buf[18] = 3; ehdr_buf[19] = 0;
    /* e_version = 1 */
    ehdr_buf[20] = 1;
    /* e_entry */
    u32 entry = zas_entry_set ? zas_entry : ZAS_TEXT_VMA;
    ehdr_buf[24]=(u8)entry; ehdr_buf[25]=(u8)(entry>>8);
    ehdr_buf[26]=(u8)(entry>>16); ehdr_buf[27]=(u8)(entry>>24);
    /* e_phoff = 52 */
    ehdr_buf[28]=52;
    /* e_ehsize = 52 */
    ehdr_buf[40]=52;
    /* e_phentsize = 32 */
    ehdr_buf[42]=32;
    /* e_phnum = 1 */
    ehdr_buf[44]=1;

    /* Build program header */
    u8 phdr_buf[32];
    memset(phdr_buf, 0, sizeof phdr_buf);
    /* p_type = PT_LOAD = 1 */
    phdr_buf[0]=1;
    /* p_offset = hdr_size */
    phdr_buf[4]=(u8)hdr_size; phdr_buf[5]=(u8)(hdr_size>>8);
    phdr_buf[6]=(u8)(hdr_size>>16); phdr_buf[7]=(u8)(hdr_size>>24);
    /* p_vaddr = ZAS_TEXT_VMA */
    phdr_buf[8]=(u8)ZAS_TEXT_VMA; phdr_buf[9]=(u8)(ZAS_TEXT_VMA>>8);
    phdr_buf[10]=(u8)(ZAS_TEXT_VMA>>16); phdr_buf[11]=(u8)(ZAS_TEXT_VMA>>24);
    /* p_paddr = p_vaddr */
    phdr_buf[12]=phdr_buf[8]; phdr_buf[13]=phdr_buf[9];
    phdr_buf[14]=phdr_buf[10]; phdr_buf[15]=phdr_buf[11];
    /* p_filesz = code_size */
    phdr_buf[16]=(u8)code_size; phdr_buf[17]=(u8)(code_size>>8);
    phdr_buf[18]=(u8)(code_size>>16); phdr_buf[19]=(u8)(code_size>>24);
    /* p_memsz = code_size + bss_size */
    u32 memsz = code_size + bss_size;
    phdr_buf[20]=(u8)memsz; phdr_buf[21]=(u8)(memsz>>8);
    phdr_buf[22]=(u8)(memsz>>16); phdr_buf[23]=(u8)(memsz>>24);
    /* p_flags = PF_R|PF_W|PF_X = 7 */
    phdr_buf[24]=7;
    /* p_align = 0x1000 */
    phdr_buf[29]=0x10;

    /* Write to file */
    fs_unlink(out_path);
    if (fs_create(out_path) < 0) { kprintf("zas: cannot create %s\n", out_path); return -1; }
    int h = fs_open(out_path);
    if (h < 0) { kprintf("zas: cannot open %s\n", out_path); return -1; }
    fs_write(h, ehdr_buf, 52);
    fs_write(h, phdr_buf, 32);
    fs_write(h, zas_out, code_size);
    fs_close(h);
    return (int)(hdr_size + code_size);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
int zas_assemble(const char *src_path, const char *out_path) {
    /* Reset state */
    zas_label_count = 0;
    zas_fixup_count = 0;
    zas_entry = ZAS_TEXT_VMA;
    zas_entry_set = 0;
    def_count = 0;
    memset(zas_out, 0, sizeof zas_out);

    /* Read source */
    int h = fs_open(src_path);
    if (h < 0) { kprintf("zas: cannot open %s\n", src_path); return -1; }
    int sz = fs_size(h);
    if (sz < 0 || (u32)sz >= ZAS_SRC_MAX) {
        kprintf("zas: source too large or unreadable\n");
        fs_close(h); return -1;
    }
    memset(zas_src, 0, sizeof zas_src);
    fs_read(h, zas_src, (size_t)sz);
    fs_close(h);

    /* Pass 1: collect labels, calculate sizes */
    run_pass(1);

    /* Pass 2: emit code */
    zas_fixup_count = 0;
    run_pass(2);

    /* Apply fixups */
    apply_fixups();

    /* Write ELF */
    int result = write_elf(out_path);
    if (result < 0) return -1;
    kprintf("zas: %u bytes text, %u bytes data, %u bytes bss, entry=0x%x\n",
            text_size, data_size, bss_size, zas_entry);
    return result;
}

/* mdview - Markdown viewer for Windows 95 through Windows 11
 *
 * Parses Markdown into blocks, lays them out for the window width and draws
 * them itself with plain GDI (no RichEdit), so tables get real cell borders,
 * padding, shading and wrapping, code gets a proper box, and links are clickable.
 *
 *   mdview [file.md]    File > Open, or drag a file onto the window.
 *   F5 reload   Ctrl+wheel / Ctrl +/- zoom   Ctrl+C copies the document as text
 *
 * Supports: # headings, **bold**, *italic*, `code`, ``` fenced code, - * + and
 * 1. lists (nested by indent), > quotes, | tables (with :--: alignment), --- rules,
 * [links](url) and images (shown as a placeholder).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include "version.h"

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef MK_CONTROL
#define MK_CONTROL 0x0008
#endif
#ifndef ODS_NOACCEL
#define ODS_NOACCEL 0x0100
#endif
#ifndef ODS_HOTLIGHT
#define ODS_HOTLIGHT 0x0040
#endif
#ifndef DT_HIDEPREFIX
#define DT_HIDEPREFIX 0x00100000
#endif

#define ICON_ID 1
enum { ID_OPEN = 1, ID_EXIT, ID_RELOAD, ID_COPY, ID_ZOOMIN, ID_ZOOMOUT, ID_ZOOM100, ID_ABOUT,
       ID_COPYALL, ID_SELALL, ID_PASTE,
       ID_THEME_AUTO, ID_THEME_LIGHT, ID_THEME_DARK, ID_CTX_OPEN, ID_CTX_COPYLINK, ID_CTX_COPYCODE, ID_WIDTH,
       ID_FIND, ID_FINDNEXT, ID_FINDPREV, ID_FINDCLOSE, ID_FINDEDIT, ID_SMOOTH };

/* ---- colours ---- */
enum { P_TEXT, P_GRAY, P_HEAD, P_CODE, P_BLOCK, P_LINK, P_PAGE, P_INLINEBG, P_BLOCKBG, P_BLOCKBD,
       P_QBAR, P_RULE, P_GRID, P_THEAD, P_ZEBRA,
       P_KW, P_CTRL, P_STR, P_COM, P_NUM, P_TYPE, P_PRE, P_COUNT };
static COLORREF pal[P_COUNT];
static const COLORREF pal_light[P_COUNT] = {
    RGB(30, 30, 30), RGB(96, 96, 96), RGB(30, 30, 30), RGB(150, 0, 0), RGB(40, 40, 40), RGB(0, 0, 200),
    RGB(255, 255, 255), RGB(236, 236, 238), RGB(244, 244, 245), RGB(214, 218, 224), RGB(190, 197, 210),
    RGB(205, 210, 218), RGB(196, 202, 212), RGB(222, 229, 242), RGB(247, 249, 252),
    RGB(0, 0, 255), RGB(143, 8, 196), RGB(163, 21, 21), RGB(0, 128, 0), RGB(9, 134, 88), RGB(43, 145, 175),
    RGB(128, 128, 128)
};
static const COLORREF pal_dark[P_COUNT] = {
    RGB(215, 215, 218), RGB(150, 152, 158), RGB(215, 215, 218), RGB(255, 150, 125), RGB(215, 215, 218),
    RGB(110, 170, 255), RGB(30, 30, 32), RGB(58, 58, 63), RGB(40, 40, 44), RGB(72, 72, 80),
    RGB(84, 90, 106), RGB(70, 72, 80), RGB(82, 86, 96), RGB(48, 56, 78), RGB(36, 36, 40),
    RGB(86, 156, 214), RGB(197, 134, 192), RGB(206, 145, 120), RGB(106, 153, 85), RGB(181, 206, 168),
    RGB(78, 201, 176), RGB(155, 155, 155)
};
#define C_TEXT     pal[P_TEXT]
#define C_GRAY     pal[P_GRAY]
#define C_HEAD     pal[P_HEAD]
#define C_CODE     pal[P_CODE]
#define C_BLOCK    pal[P_BLOCK]
#define C_LINK     pal[P_LINK]
#define C_PAGE     pal[P_PAGE]
#define C_INLINEBG pal[P_INLINEBG]
#define C_BLOCKBG  pal[P_BLOCKBG]
#define C_BLOCKBD  pal[P_BLOCKBD]
#define C_QBAR     pal[P_QBAR]
#define C_RULE     pal[P_RULE]
#define C_GRID     pal[P_GRID]
#define C_THEAD    pal[P_THEAD]
#define C_ZEBRA    pal[P_ZEBRA]
static int g_theme, g_dark, g_limit_width = 0, g_smooth = 1;     /* theme: 0 follow the system, 1 light, 2 dark */
static HMENU view_menu;
#define NOBACK     0xFFFFFFFFUL

/* ---- text model ---- */

enum { F_BOLD = 1, F_ITAL = 2, F_CODE = 4, F_LINK = 8, F_IMG = 16 };

typedef struct { char *text; unsigned flags; int link; } Span;     /* link: 1-based index, 0 = none */
typedef struct { Span *s; int n, cap; } Inline;

enum { BK_PARA, BK_HEAD, BK_LIST, BK_QUOTE, BK_CODE, BK_TABLE, BK_RULE };

typedef struct {
    int kind, level, numbered;
    char num[16];
    Inline inl;                          /* PARA, HEAD, LIST, QUOTE */
    char **lines; int nlines;            /* CODE */
    Inline *cells; int rows, cols, header; int *align;   /* TABLE: cells[r * cols + c]; align 0/1 centre/2 right */
    int y;                               /* top of the block in the laid-out document */
    int inset;                           /* PARA, CODE inside a list item: that item's level + 1 */
} Block;

static Block *blocks;
static int nblocks, capblocks;
static char **links;
static int nlinks, caplinks;

static void add_span(Inline *in, const char *t, int n, unsigned flags, int link)
{
    if (n <= 0) return;
    if (in->n == in->cap) {
        in->cap = in->cap ? in->cap * 2 : 8;
        in->s = (Span *)realloc(in->s, in->cap * sizeof(Span));
    }
    in->s[in->n].text = (char *)malloc(n + 1);
    memcpy(in->s[in->n].text, t, n);
    in->s[in->n].text[n] = 0;
    in->s[in->n].flags = flags;
    in->s[in->n].link = link;
    in->n++;
}

static int add_link(const char *u, int n)
{
    if (nlinks == caplinks) {
        caplinks = caplinks ? caplinks * 2 : 16;
        links = (char **)realloc(links, caplinks * sizeof(char *));
    }
    links[nlinks] = (char *)malloc(n + 1);
    memcpy(links[nlinks], u, n);
    links[nlinks][n] = 0;
    return ++nlinks;
}

static Block *new_block(int kind)
{
    Block *b;
    if (nblocks == capblocks) {
        capblocks = capblocks ? capblocks * 2 : 64;
        blocks = (Block *)realloc(blocks, capblocks * sizeof(Block));
    }
    b = &blocks[nblocks++];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    return b;
}

static void free_inline(Inline *in)
{
    int i;
    for (i = 0; i < in->n; i++) free(in->s[i].text);
    free(in->s);
    in->s = NULL;
    in->n = in->cap = 0;
}

static void free_refs(void);

static void free_doc(void)
{
    int i, j;
    for (i = 0; i < nblocks; i++) {
        Block *b = &blocks[i];
        free_inline(&b->inl);
        for (j = 0; j < b->nlines; j++) free(b->lines[j]);
        free(b->lines);
        for (j = 0; j < b->rows * b->cols; j++) free_inline(&b->cells[j]);
        free(b->cells);
        free(b->align);
    }
    free(blocks);
    blocks = NULL;
    nblocks = capblocks = 0;
    free_refs();
    for (i = 0; i < nlinks; i++) free(links[i]);
    free(links);
    links = NULL;
    nlinks = caplinks = 0;
}

/* ---- link reference definitions: [label]: url ---- */

typedef struct { char *label, *url; } Ref;
static Ref *refs;
static int nrefs, caprefs;

static void add_ref(const char *lab, int ln, const char *url, int un)
{
    if (nrefs == caprefs) {
        caprefs = caprefs ? caprefs * 2 : 16;
        refs = (Ref *)realloc(refs, caprefs * sizeof(Ref));
    }
    refs[nrefs].label = (char *)malloc(ln + 1);
    memcpy(refs[nrefs].label, lab, ln);
    refs[nrefs].label[ln] = 0;
    refs[nrefs].url = (char *)malloc(un + 1);
    memcpy(refs[nrefs].url, url, un);
    refs[nrefs].url[un] = 0;
    nrefs++;
}

static const char *ref_find(const char *lab, int n)
{
    int i;
    for (i = 0; i < nrefs; i++)
        if ((int)strlen(refs[i].label) == n && !strnicmp(refs[i].label, lab, n))
            return refs[i].url;
    return NULL;
}

static void free_refs(void)
{
    int i;
    for (i = 0; i < nrefs; i++) { free(refs[i].label); free(refs[i].url); }
    free(refs);
    refs = NULL;
    nrefs = caprefs = 0;
}

/* a line like "[label]: url" (optionally followed by a title); returns 1 and records it */
static int ref_definition(const char *t)
{
    const char *close, *u, *e;
    int ind = 0;

    while (t[ind] == ' ') ind++;
    t += ind;
    if (ind > 3 || *t != '[' || t[1] == '^') return 0;
    close = strchr(t, ']');
    if (!close || close == t + 1 || close[1] != ':') return 0;
    u = close + 2;
    while (*u == ' ' || *u == '\t') u++;
    if (!*u) return 0;
    e = u;
    while (*e && *e != ' ' && *e != '\t') e++;
    if (*u == '<' && e > u + 1 && e[-1] == '>') { u++; e--; }
    add_ref(t + 1, (int)(close - t - 1), u, (int)(e - u));
    return 1;
}

/* ---- inline markup parser ---- */

#define RUNMAX 2048

/* find the ] matching the [ at open: skips code spans and escapes, nests */
static const char *match_bracket(const char *open)
{
    int depth = 0;
    const char *p = open;

    for (; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '`') {
            int n = 0, m;
            const char *q;
            while (p[n] == '`') n++;
            q = p + n;
            for (;;) {
                while (*q && *q != '`') q++;
                if (!*q) break;
                for (m = 0; q[m] == '`'; m++) ;
                if (m == n) { p = q + m - 1; break; }
                q += m;
            }
            if (!*q) p += n - 1;          /* unmatched backticks are plain text */
        } else if (*p == '[') {
            depth++;
        } else if (*p == ']') {
            if (--depth == 0) return p;
        }
    }
    return NULL;
}

static void parse_ex(Inline *out, const char *s, unsigned base, int link);

/* link text can hold its own markup (code spans, emphasis) */
static void add_link_text(Inline *out, const char *t, int n, unsigned base, int link)
{
    char *tmp = (char *)malloc(n + 1);
    memcpy(tmp, t, n);
    tmp[n] = 0;
    parse_ex(out, tmp, base | F_LINK, link);
    free(tmp);
}

/* <a href="url">inner</a>: pieces of the first such anchor starting at p (case-insensitive) */
static int html_anchor(const char *p, const char **href, int *hlen, const char **inner, int *ilen, const char **after)
{
    const char *gt, *h, *close;
    char q;
    if (p[0] != '<' || (p[1] != 'a' && p[1] != 'A') || (p[2] != ' ' && p[2] != '\t')) return 0;
    gt = strchr(p, '>');
    if (!gt) return 0;
    for (h = p + 2; h + 5 < gt; h++)
        if (!strnicmp(h, "href=", 5)) break;
    if (h + 5 >= gt) return 0;
    h += 5;
    q = *h;
    if (q == '"' || q == '\'') {
        const char *e = memchr(h + 1, q, gt - h - 1);
        if (!e) return 0;
        *href = h + 1; *hlen = (int)(e - h - 1);
    } else {
        const char *e = h;
        while (e < gt && *e != ' ') e++;
        *href = h; *hlen = (int)(e - h);
    }
    for (close = gt + 1; *close; close++)
        if (close[0] == '<' && close[1] == '/' && (close[2] == 'a' || close[2] == 'A') && close[3] == '>') break;
    if (!*close) return 0;
    *inner = gt + 1; *ilen = (int)(close - gt - 1);
    *after = close + 4;
    return 1;
}

static void parse_ex(Inline *out, const char *s, unsigned base, int link)
{
    const char *a_href, *a_inner, *a_after;
    int a_hlen, a_ilen;
    char run[RUNMAX];
    int n = 0, bold = 0, italic = 0, code = 0;
    const char *p = s;

#define FLUSH() do {         if (n) {             add_span(out, run, n, base | (bold ? F_BOLD : 0) | (italic ? F_ITAL : 0) | (code ? F_CODE : 0), link);             n = 0;         }     } while (0)

    while (*p) {
        if (n >= RUNMAX - 4) FLUSH();

        if (code) {
            /* inside a code span nothing is special except the closing backtick */
            if (*p == '`') { FLUSH(); code = 0; p++; }
            else run[n++] = *p++;
        } else if (*p == '\\' && p[1] && ispunct((unsigned char)p[1])) {
            run[n++] = p[1];
            p += 2;
        } else if (*p == '`') {
            FLUSH();
            code = 1;
            p++;
        } else if ((*p == '!' && p[1] == '[') || *p == '[') {
            int image = (*p == '!');
            const char *open = p + (image ? 1 : 0);
            const char *close = match_bracket(open);
            const char *end = (close && close[1] == '(') ? strchr(close + 2, ')') : NULL;
            unsigned fl = base | (bold ? F_BOLD : 0) | (italic ? F_ITAL : 0);
            if (end) {
                int len = (int)(close - open - 1);
                FLUSH();
                if (image) {
                    char label[300];
                    int l = sprintf(label, "[image: %.*s]", len > 250 ? 250 : len, open + 1);
                    add_span(out, label, l, F_IMG | F_ITAL, 0);
                } else {
                    add_link_text(out, open + 1, len, fl, add_link(close + 2, (int)(end - (close + 2))));
                }
                p = end + 1;
            } else if (close && !image) {
                /* reference style: [text][ref], [text][] or [ref] */
                const char *lab = open + 1, *after = close + 1, *url;
                int ll = (int)(close - open - 1);
                if (close[1] == '[') {
                    const char *c2 = strchr(close + 2, ']');
                    if (c2) {
                        if (c2 > close + 2) { lab = close + 2; ll = (int)(c2 - lab); }
                        after = c2 + 1;
                    }
                }
                url = ref_find(lab, ll);
                if (url) {
                    FLUSH();
                    add_link_text(out, open + 1, (int)(close - open - 1), fl, add_link(url, (int)strlen(url)));
                    p = after;
                } else {
                    run[n++] = *p++;
                }
            } else {
                run[n++] = *p++;
            }
        } else if (html_anchor(p, &a_href, &a_hlen, &a_inner, &a_ilen, &a_after)) {
            FLUSH();
            add_link_text(out, a_inner, a_ilen, base | (bold ? F_BOLD : 0) | (italic ? F_ITAL : 0),
                          add_link(a_href, a_hlen));
            p = a_after;
        } else if (*p == '<' && (isalpha((unsigned char)p[1]) || p[1] == '/' || p[1] == '!') &&
                   strchr(p, '>')) {
            /* raw HTML: <https://...> autolinks are links, other tags are dropped */
            const char *gt = strchr(p, '>');
            int auto_link = 0, mail_link = 0;
            if (!strncmp(p, "<!--", 4)) {
                const char *c = strstr(p, "-->");
                gt = c ? c + 2 : gt;
            } else {
                const char *q;
                auto_link = strstr(p, "://") && strstr(p, "://") < gt;
                for (q = p + 1; q < gt; q++) if (*q == ' ') auto_link = 0;
                if (!auto_link) {                       /* <name@host.tld> */
                    const char *at = memchr(p, '@', gt - p);
                    mail_link = at && at > p + 1 && at < gt - 1;
                    for (q = p + 1; q < gt; q++) if (*q == ' ' || *q == '"' || *q == '=' || *q == '/') mail_link = 0;
                }
            }
            if (auto_link || mail_link) {
                int len = (int)(gt - p - 1);
                char *u = (char *)malloc(len + 8);
                strcpy(u, mail_link ? "mailto:" : "");
                memcpy(u + strlen(u), p + 1, len);
                u[strlen(mail_link ? "mailto:" : "") + len] = 0;
                FLUSH();
                add_span(out, p + 1, len, base | (bold ? F_BOLD : 0) | (italic ? F_ITAL : 0) | F_LINK,
                         add_link(u, (int)strlen(u)));
                free(u);
            } else if (!strnicmp(p, "<br", 3) && n && run[n - 1] != ' ') {
                run[n++] = ' ';
            }
            p = gt + 1;
        } else if (*p == '*' || *p == '_') {
            char m = *p;
            int cnt = 0;
            const char *q = p;
            unsigned char prev = (p > s) ? (unsigned char)p[-1] : ' ';
            while (*q == m && cnt < 3) { q++; cnt++; }
            {
                unsigned char next = (unsigned char)*q;
                int intraword = (m == '_') && isalnum(prev) && isalnum(next);
                int opens = next && next != ' ';
                int closes = prev != ' ';
                int is_bold = (cnt >= 2), is_ital = (cnt == 1 || cnt == 3);
                int active = (is_bold && bold) || (is_ital && italic);
                if (!intraword && ((active && closes) || (!active && opens))) {
                    FLUSH();
                    if (is_bold) bold = !bold;
                    if (is_ital) italic = !italic;
                    p = q;
                } else {
                    while (p < q) run[n++] = *p++;
                }
            }
        } else {
            run[n++] = *p++;
        }
    }
    FLUSH();
#undef FLUSH
}

static void parse_inline(Inline *out, const char *s)
{
    parse_ex(out, s, 0, 0);
}

/* ---- block parser ---- */

static char *pend;
static int pend_len, pend_kind, pend_indent;
static char pend_num[16];
static int pend_inset;                  /* a paragraph continuing a list item */
static int list_ind = -1, list_lvl;     /* content column and level of the latest list item */
enum { P_NONE, P_PARA, P_QUOTE, P_LIST };

static void append(const char *s)
{
    int n;
    while (*s == ' ' || *s == '\t') s++;
    if (pend_len && pend[pend_len - 1] != ' ') pend[pend_len++] = ' ';
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    memcpy(pend + pend_len, s, n);
    pend_len += n;
    pend[pend_len] = 0;
}

static void flush(void)
{
    Block *b;
    if (pend_kind == P_NONE) return;
    b = new_block(pend_kind == P_PARA ? BK_PARA : pend_kind == P_QUOTE ? BK_QUOTE : BK_LIST);
    if (pend_kind == P_LIST) {
        b->level = pend_indent / 2 > 5 ? 5 : pend_indent / 2;
        b->numbered = pend_num[0] != 0;
        strcpy(b->num, pend_num);
    }
    if (pend_kind == P_PARA) b->inset = pend_inset;
    parse_inline(&b->inl, pend);
    if (b->kind == BK_PARA && b->inl.n == 0) {       /* e.g. a line of only an HTML anchor */
        free_inline(&b->inl);
        nblocks--;
    }
    pend_kind = P_NONE;
    pend_len = 0;
    pend[0] = 0;
}

static int is_rule(const char *t)
{
    char ch = 0;
    int cnt = 0;
    for (; *t; t++) {
        if (*t == ' ') continue;
        if (!ch) {
            if (*t != '-' && *t != '*' && *t != '_') return 0;
            ch = *t;
        } else if (*t != ch) {
            return 0;
        }
        cnt++;
    }
    return cnt >= 3;
}

/* "- text", "* text", "+ text", "12. text"; returns text start or NULL */
static const char *list_item(const char *t, char *num)
{
    num[0] = 0;
    if ((*t == '-' || *t == '*' || *t == '+') && t[1] == ' ') return t + 2;
    if (isdigit((unsigned char)*t)) {
        int n = 0;
        while (isdigit((unsigned char)t[n])) n++;
        if (n <= 9 && (t[n] == '.' || t[n] == ')') && t[n + 1] == ' ') {
            memcpy(num, t, n);
            num[n] = 0;
            return t + n + 2;
        }
    }
    return NULL;
}

/* split "| a | b |" into trimmed cell strings; returns the cell count */
static int split_cells(const char *line, char **out, int max)
{
    int c = 0;
    const char *p = line;

    if (*p == '|') p++;
    while (*p && c < max) {
        const char *start;
        int n;
        while (*p == ' ') p++;
        start = p;
        while (*p && *p != '|') p++;
        n = (int)(p - start);
        while (n > 0 && start[n - 1] == ' ') n--;
        if (!*p && n == 0) break;                 /* trailing "|" leaves an empty tail */
        out[c] = (char *)malloc(n + 1);
        memcpy(out[c], start, n);
        out[c][n] = 0;
        c++;
        if (*p == '|') p++;
    }
    return c;
}

static int is_separator_cells(char **cells, int n)
{
    int c, any = 0;
    for (c = 0; c < n; c++) {
        const char *s = cells[c];
        for (; *s; s++) {
            if (*s == '-') any = 1;
            else if (*s != ':' && *s != ' ') return 0;
        }
    }
    return any;
}

#define MAXCOLS 16

static void parse_table(char **lines, int first, int count)
{
    char **rowcells[1024];
    int rowlen[1024];
    int r, c, nrows = 0, ncols = 0, header = 0, sep_row = -1;
    int align[MAXCOLS] = {0};
    Block *b;

    if (count > 1024) count = 1024;
    for (r = 0; r < count; r++) {
        char *cells[MAXCOLS];
        int n = split_cells(lines[first + r], cells, MAXCOLS);
        if (r == 1 && is_separator_cells(cells, n)) {
            sep_row = r;
            for (c = 0; c < n; c++) {
                int len = strlen(cells[c]);
                int left = len && cells[c][0] == ':';
                int right = len && cells[c][len - 1] == ':';
                align[c] = (left && right) ? 1 : right ? 2 : 0;
            }
            header = 1;
            for (c = 0; c < n; c++) free(cells[c]);
            continue;
        }
        rowcells[nrows] = (char **)malloc(MAXCOLS * sizeof(char *));
        memcpy(rowcells[nrows], cells, n * sizeof(char *));
        rowlen[nrows] = n;
        if (n > ncols) ncols = n;
        nrows++;
    }
    (void)sep_row;

    b = new_block(BK_TABLE);
    b->rows = nrows;
    b->cols = ncols;
    b->header = header;
    b->align = (int *)malloc((ncols + 1) * sizeof(int));
    for (c = 0; c < ncols; c++) b->align[c] = align[c];
    b->cells = (Inline *)calloc(nrows * ncols + 1, sizeof(Inline));
    for (r = 0; r < nrows; r++) {
        for (c = 0; c < rowlen[r]; c++) {
            parse_inline(&b->cells[r * ncols + c], rowcells[r][c]);
            free(rowcells[r][c]);
        }
        free(rowcells[r]);
    }
}

/* ---- HTML lists (<ul><li>..) are rewritten as Markdown lists before parsing ---- */

typedef struct { char *s; int n, cap; } Buf;

static void buf_add(Buf *b, const char *t, int n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = (char *)realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, t, n);
    b->n += n;
    b->s[b->n] = 0;
}

/* converts from the opening <ul>/<ol> at q to just past its closing tag */
static const char *convert_list(Buf *o, const char *q)
{
    int depth = 0, ordered[16], count[16];
    while (*q) {
        if (*q == '<') {
            const char *gt = strchr(q, '>');
            char name[8];
            int closing = q[1] == '/', k = 0;
            const char *nm = q + 1 + closing;
            if (!gt) { buf_add(o, q, 1); q++; continue; }
            while (k < 7 && isalnum((unsigned char)nm[k])) { name[k] = (char)tolower((unsigned char)nm[k]); k++; }
            name[k] = 0;
            if (!strcmp(name, "ul") || !strcmp(name, "ol")) {
                if (!closing) {
                    if (depth < 15) depth++;
                    ordered[depth] = name[0] == 'o';
                    count[depth] = 0;
                } else if (--depth <= 0) {
                    buf_add(o, "\n\n", 2);
                    return gt + 1;
                }
            } else if (!strcmp(name, "li")) {
                if (!closing) {
                    char mark[16];
                    int ind = depth > 1 ? (depth - 1) * 2 : 0, m;
                    buf_add(o, "\n", 1);
                    for (m = 0; m < ind; m++) buf_add(o, " ", 1);
                    if (ordered[depth > 0 ? depth : 1]) m = sprintf(mark, "%d. ", ++count[depth > 0 ? depth : 1]);
                    else m = sprintf(mark, "- ");
                    buf_add(o, mark, m);
                }
            } else if (!strcmp(name, "code")) {
                buf_add(o, "`", 1);
            } else if (!strcmp(name, "b") || !strcmp(name, "strong")) {
                buf_add(o, "**", 2);
            } else if (!strcmp(name, "i") || !strcmp(name, "em")) {
                buf_add(o, "*", 1);
            } else {
                buf_add(o, q, (int)(gt - q + 1));       /* <a href=..> and friends go to the inline parser */
            }
            q = gt + 1;
        } else if (*q == '\n' || *q == '\r' || *q == '\t') {
            if (o->n && o->s[o->n - 1] != ' ' && o->s[o->n - 1] != '\n') buf_add(o, " ", 1);
            q++;
        } else {
            buf_add(o, q, 1);
            q++;
        }
    }
    return q;
}

static char *html_lists(const char *text)
{
    Buf o;
    const char *p = text;
    int fenced = 0;
    if (!strstr(text, "<ul") && !strstr(text, "<ol")) return NULL;
    memset(&o, 0, sizeof(o));
    while (*p) {
        const char *eol = strchr(p, '\n'), *u = p;
        int ll = eol ? (int)(eol - p) + 1 : (int)strlen(p);
        while (*u == ' ' || *u == '\t') u++;
        if (!strncmp(u, "```", 3) || !strncmp(u, "~~~", 3)) fenced = !fenced;
        if (!fenced && (!strnicmp(u, "<ul", 3) || !strnicmp(u, "<ol", 3)) &&
            (u[3] == '>' || u[3] == ' ' || u[3] == '\r' || u[3] == '\n')) {
            p = convert_list(&o, u);
            continue;
        }
        buf_add(&o, p, ll);
        p += ll;
    }
    return o.s;
}

static void parse_doc_in(char *text)
{
    char **lines, *p;
    int nl = 1, i;
    size_t total = strlen(text);        /* before the splitting below NULs the newlines */

    for (p = text; *p; p++)
        if (*p == '\n') nl++;
    lines = (char **)malloc((nl + 1) * sizeof(char *));
    nl = 0;
    lines[nl++] = text;
    for (p = text; *p; p++) {
        if (*p == '\n') {
            *p = 0;
            lines[nl++] = p + 1;
        }
    }
    for (i = 0; i < nl; i++) {
        int len = strlen(lines[i]);
        if (len && lines[i][len - 1] == '\r') lines[i][len - 1] = 0;
    }

    {   /* collect reference definitions (outside code fences) and blank their lines */
        int fenced = 0;
        for (i = 0; i < nl; i++) {
            const char *u = lines[i];
            while (*u == ' ' || *u == '\t') u++;
            if (!strncmp(u, "```", 3) || !strncmp(u, "~~~", 3)) fenced = !fenced;
            else if (!fenced && ref_definition(lines[i])) lines[i] = (char *)"";
        }
    }

    pend = (char *)malloc(total + 64);
    list_ind = -1;
    pend_inset = 0;
    pend_len = 0;
    pend_kind = P_NONE;
    pend[0] = 0;

    for (i = 0; i < nl; i++) {
        char *ln = lines[i], *t = ln;
        int ind = 0;
        const char *item;
        char num[16];

        while (*t == ' ' || *t == '\t') {
            ind += (*t == '\t') ? 4 : 1;
            t++;
        }

        if (!strncmp(t, "```", 3) || !strncmp(t, "~~~", 3)) {
            int j = i + 1, k;
            Block *b;
            flush();
            while (j < nl) {
                const char *u = lines[j];
                while (*u == ' ' || *u == '\t') u++;
                if (!strncmp(u, "```", 3) || !strncmp(u, "~~~", 3)) break;
                j++;
            }
            b = new_block(BK_CODE);
            b->inset = (list_ind >= 0 && ind >= list_ind) ? list_lvl + 1 : 0;
            {
                const char *l = t + 3;
                int q = 0;
                while (*l == '`' || *l == '~' || *l == ' ') l++;
                while (*l && q < 15 && (isalnum((unsigned char)*l) || *l == '+' || *l == '#' || *l == '-' || *l == '.'))
                    b->num[q++] = (char)tolower((unsigned char)*l++);
                b->num[q] = 0;
            }
            b->nlines = j - (i + 1);
            b->lines = (char **)malloc((b->nlines + 1) * sizeof(char *));
            for (k = 0; k < b->nlines; k++) {
                b->lines[k] = (char *)malloc(strlen(lines[i + 1 + k]) + 1);
                strcpy(b->lines[k], lines[i + 1 + k]);
            }
            i = j;                       /* skip past the closing fence */
            continue;
        }
        if (!*t) {
            flush();
            continue;
        }
        if (list_ind >= 0 && pend_kind == P_NONE && ind >= list_ind && *t != '#' && *t != '|' && *t != '>' &&
            !is_rule(t) && !list_item(t, num)) {
            if (ind >= list_ind + 4) {                      /* indented code inside a list item */
                int j = i, k, strip = list_ind + 4, last = i;
                Block *b;
                while (j < nl) {
                    const char *u = lines[j];
                    int w = 0;
                    while (*u == ' ' || *u == '\t') { w += *u == '\t' ? 4 : 1; u++; }
                    if (*u) {
                        if (w < strip) break;
                        last = j;
                    }
                    j++;
                }
                b = new_block(BK_CODE);
                b->inset = list_lvl + 1;
                b->nlines = last - i + 1;
                b->lines = (char **)malloc((b->nlines + 1) * sizeof(char *));
                for (k = 0; k < b->nlines; k++) {
                    const char *u = lines[i + k];
                    int w = 0;
                    while ((*u == ' ' || *u == '\t') && w < strip) { w += *u == '\t' ? 4 : 1; u++; }
                    b->lines[k] = (char *)malloc(strlen(u) + 1);
                    strcpy(b->lines[k], u);
                }
                i = last;
                continue;
            }
            pend_kind = P_PARA;                             /* a further paragraph of the item */
            pend_inset = list_lvl + 1;
            append(t);
            continue;
        }
        if (pend_kind == P_PARA && ind < 4 && (*t == '=' || *t == '-')) {
            /* setext heading: the paragraph so far is underlined with === or --- */
            const char *u = t;
            int cnt = 0;
            while (*u == *t) { u++; cnt++; }
            while (*u == ' ') u++;
            if (!*u && (*t == '=' || cnt >= 2)) {
                Block *b = new_block(BK_HEAD);
                b->level = (*t == '=') ? 1 : 2;
                parse_inline(&b->inl, pend);
                pend_kind = P_NONE;
                pend_len = 0;
                pend[0] = 0;
                continue;
            }
        }
        if (*t == '#') {
            int h = 0;
            while (t[h] == '#') h++;
            if (h <= 6 && (t[h] == ' ' || t[h] == 0)) {
                Block *b;
                char *s = t + h;
                int n;
                flush();
                list_ind = -1;
                while (*s == ' ') s++;
                n = strlen(s);
                while (n > 0 && (s[n - 1] == '#' || s[n - 1] == ' ')) s[--n] = 0;
                b = new_block(BK_HEAD);
                b->level = h;
                parse_inline(&b->inl, s);
                continue;
            }
        }
        if (is_rule(t)) {
            flush();
            list_ind = -1;
            new_block(BK_RULE);
            continue;
        }
        if (*t == '|') {
            int j = i;
            flush();
            list_ind = -1;
            while (j < nl) {
                const char *u = lines[j];
                while (*u == ' ' || *u == '\t') u++;
                if (*u != '|') break;
                j++;
            }
            parse_table(lines, i, j - i);
            i = j - 1;
            continue;
        }
        if (*t == '>') {
            if (pend_kind != P_QUOTE) flush();
            list_ind = -1;
            pend_kind = P_QUOTE;
            t++;
            if (*t == ' ') t++;
            if (*t) append(t);
            continue;
        }
        item = list_item(t, num);
        if (item) {
            flush();
            pend_kind = P_LIST;
            pend_indent = ind;
            list_ind = ind + (int)(item - t);
            list_lvl = ind / 2 > 5 ? 5 : ind / 2;
            strcpy(pend_num, num);
            pend_len = 0;
            pend[0] = 0;
            append(item);
            continue;
        }
        if (pend_kind == P_LIST || pend_kind == P_QUOTE) {
            append(t);               /* continuation of the current item */
            continue;
        }
        if (pend_kind != P_PARA) {
            flush();
            list_ind = -1;
            pend_inset = 0;
        }
        pend_kind = P_PARA;
        append(t);
    }
    flush();
    free(pend);
    free(lines);
}

static void parse_doc(char *text)
{
    char *c = html_lists(text);
    parse_doc_in(c ? c : text);
    free(c);
}

/* ---- fonts and metrics ---- */

static HWND hwnd;
static HDC mdc;                    /* scratch DC for measuring text */
static int g_dpi = 96, g_zoom = 100;
static int g_nt, g_uni;               /* running on NT; Unicode text APIs usable */

static int PX(int pt) { return MulDiv(pt, g_dpi * g_zoom * 8 / 11, 7200); }
static int S(int n)   { return MulDiv(n, g_dpi * g_zoom * 8 / 11, 9600); }

typedef struct { int px, b, i, m, u; HFONT h; int asc, ht, spw; int cw[256]; int pw[32]; } FontEnt;
static FontEnt fonts[128];
static int nfonts;

static int font_get(int px, int b, int i, int m, int u)
{
    int k;
    LOGFONT lf;
    TEXTMETRIC tm;
    SIZE sz;
    FontEnt *f;

    for (k = 0; k < nfonts; k++)
        if (fonts[k].px == px && fonts[k].b == b && fonts[k].i == i &&
            fonts[k].m == m && fonts[k].u == u)
            return k;
    if (nfonts >= 128) return 0;
    f = &fonts[nfonts];
    memset(f->pw, 0, sizeof(f->pw));
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = b ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (BYTE)i;
    lf.lfUnderline = (BYTE)u;
    lf.lfCharSet = ANSI_CHARSET;
    lf.lfQuality = 5;                   /* CLEARTYPE_QUALITY: smooth at every size (older Windows ignores it) */
    lf.lfPitchAndFamily = m ? (FIXED_PITCH | FF_MODERN) : (VARIABLE_PITCH | FF_SWISS);
    strcpy(lf.lfFaceName, m ? "Courier New" : "Arial");
    f->h = CreateFontIndirect(&lf);
    f->px = px; f->b = b; f->i = i; f->m = m; f->u = u;
    SelectObject(mdc, f->h);
    GetTextMetrics(mdc, &tm);
    f->asc = tm.tmAscent;
    f->ht = tm.tmHeight;
    GetTextExtentPoint32(mdc, " ", 1, &sz);
    f->spw = sz.cx;
    if (!GetCharWidth32(mdc, 0, 255, f->cw)) {                /* fall back to one call per char */
        int ch;
        for (ch = 0; ch < 256; ch++) {
            char s = (char)ch;
            GetTextExtentPoint32(mdc, &s, 1, &sz);
            f->cw[ch] = sz.cx;
        }
    }
    return nfonts++;
}

static void fonts_clear(void)
{
    int k;
    for (k = 0; k < nfonts; k++) DeleteObject(fonts[k].h);
    nfonts = 0;
}

/* ---- characters outside Windows-1252 (emoji, arrows, ...): kept as codes 1..31 in the text, drawn with Unicode APIs on NT ---- */

typedef BOOL (WINAPI *PEXTOUTW)(HDC, int, int, UINT, const RECT *, const wchar_t *, UINT, const INT *);
typedef BOOL (WINAPI *PTEXTENTW)(HDC, const wchar_t *, int, LPSIZE);
static PEXTOUTW pExtTextOutW;
static PTEXTENTW pGetTextExtentPoint32W;
static unsigned long ph_cp[32];
static int nph;

typedef struct { int px, emoji; HFONT h; } SymFont;
static SymFont symfonts[48];
static int nsym;

static void ph_init(void)
{
    HMODULE g = GetModuleHandle("gdi32.dll");
    pExtTextOutW = (PEXTOUTW)GetProcAddress(g, "ExtTextOutW");
    pGetTextExtentPoint32W = (PTEXTENTW)GetProcAddress(g, "GetTextExtentPoint32W");
    g_uni = g_nt && pExtTextOutW && pGetTextExtentPoint32W;
}

static int ph_get(unsigned long cp)
{
    int c, freec = 0;
    for (c = 1; c < 32; c++) {
        if (c >= 9 && c <= 13) continue;
        if (ph_cp[c] == cp) return c;
        if (!ph_cp[c] && !freec) freec = c;
    }
    if (!freec) return 0;
    ph_cp[freec] = cp;
    nph++;
    return freec;
}

static void ph_reset(void)
{
    int k;
    memset(ph_cp, 0, sizeof(ph_cp));
    nph = 0;
    for (k = 0; k < nfonts; k++) memset(fonts[k].pw, 0, sizeof(fonts[k].pw));
}

static int wide_of(unsigned long cp, wchar_t *w)
{
    if (cp >= 0x10000) {
        cp -= 0x10000;
        w[0] = (wchar_t)(0xD800 + (cp >> 10));
        w[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        return 2;
    }
    w[0] = (wchar_t)cp;
    return 1;
}

static HFONT sym_font(int px, unsigned long cp)
{
    int k, emoji = cp >= 0x1F000 || (cp >= 0x2300 && cp <= 0x23FF) || (cp >= 0x2600 && cp <= 0x27BF) ||
                   (cp >= 0x2B00 && cp <= 0x2BFF);
    LOGFONT lf;
    for (k = 0; k < nsym; k++)
        if (symfonts[k].px == px && symfonts[k].emoji == emoji) return symfonts[k].h;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = 5;
    strcpy(lf.lfFaceName, emoji ? "Segoe UI Emoji" : "Segoe UI Symbol");
    if (nsym == 48) {
        DeleteObject(symfonts[0].h);
        memmove(symfonts, symfonts + 1, 47 * sizeof(SymFont));
        nsym--;
    }
    symfonts[nsym].px = px; symfonts[nsym].emoji = emoji;
    symfonts[nsym].h = CreateFontIndirect(&lf);
    return symfonts[nsym++].h;
}

static int ph_width(int fnt, int c)
{
    FontEnt *f = &fonts[fnt];
    if (!ph_cp[c]) return f->cw[c];
    if (!f->pw[c]) {
        wchar_t w[2];
        SIZE sz;
        int n = wide_of(ph_cp[c], w);
        HGDIOBJ old = SelectObject(mdc, sym_font(f->px, ph_cp[c]));
        if (!pGetTextExtentPoint32W(mdc, w, n, &sz)) sz.cx = f->px;
        SelectObject(mdc, old);
        f->pw[c] = sz.cx > 0 ? sz.cx : f->px;
    }
    return f->pw[c];
}

/* text width: the sum of the font's glyph advances (GDI does no kerning), no GDI call per word */
static int text_w(int fnt, const char *s, int len)
{
    const int *cw = fonts[fnt].cw;
    int w = 0;
    while (len-- > 0) {
        unsigned char c = (unsigned char)*s++;
        w += c < 32 ? ph_width(fnt, c) : cw[c];
    }
    return w;
}

/* ---- display list ---- */

enum { IT_TEXT, IT_RECT, IT_DISC, IT_RING };
typedef struct { int kind, x, y, w, h, font, link; COLORREF col; char *text; int len; int sep; int box; } Item;
/* sep: how a copy separates this item from the previous one (0 none/auto, 1 newline, 2 tab, 3 blank line) */

static Item *items;

/* item text lives in 64 KB chunks that are freed all at once, instead of one malloc per fragment */
typedef struct TextChunk { struct TextChunk *next; int used, cap; char data[1]; } TextChunk;
static TextChunk *chunks;

static char *text_alloc(int n)
{
    if (!chunks || chunks->used + n > chunks->cap) {
        int cap = n > 65536 ? n : 65536;
        TextChunk *c = (TextChunk *)malloc(sizeof(TextChunk) + cap);
        c->next = chunks; c->used = 0; c->cap = cap;
        chunks = c;
    }
    chunks->used += n;
    return chunks->data + chunks->used - n;
}
static int nitems, capitems;
static int doc_h, view_w, view_h, g_scroll;

typedef struct { int item, off; } SelPos;
static SelPos sel_a = {-1, 0}, sel_b = {-1, 0};      /* anchor and caret of the text selection */

static Item *add_item(int kind, int x, int y, int w, int h, COLORREF col)
{
    Item *it;
    if (nitems == capitems) {
        capitems = capitems ? capitems * 2 : 256;
        items = (Item *)realloc(items, capitems * sizeof(Item));
    }
    it = &items[nitems++];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    it->x = x; it->y = y; it->w = w; it->h = h;
    it->col = col;
    return it;
}

static void rect(int x, int y, int w, int h, COLORREF col)
{
    if (w > 0 && h > 0) add_item(IT_RECT, x, y, w, h, col);
}

/* a fenced code block's box and its Copy button */
typedef struct { int x, y, w, h, bx, by, bw, bh, font, blk, hs, hmax, it0, it1; } CodeBox;   /* hs: horizontal scroll, hmax: how far it goes */
static CodeBox *cboxes;
static int ncboxes, capcboxes;
static int find_valid, find_visible;                    /* Find: text cache matches the items; panel shown */
static RECT find_rc;
static int as_on, as_x, as_y, as_hbox = -1, as_ax, as_ay;   /* middle-click autoscroll */
static DWORD as_t, as_t0;
static int sb_drag = -1, sb_grab;                        /* a code block's scrollbar thumb being dragged */
static int hot_box = -1, btn_hot, copied_box = -1;      /* box under the mouse, button hovered, box just copied */

static CodeBox *add_cbox(int x, int y, int w, int h, int blk)
{
    if (ncboxes == capcboxes) {
        capcboxes = capcboxes ? capcboxes * 2 : 16;
        cboxes = (CodeBox *)realloc(cboxes, capcboxes * sizeof(CodeBox));
    }
    cboxes[ncboxes].x = x; cboxes[ncboxes].y = y; cboxes[ncboxes].w = w; cboxes[ncboxes].h = h;
    cboxes[ncboxes].blk = blk;
    cboxes[ncboxes].hs = cboxes[ncboxes].hmax = cboxes[ncboxes].it0 = cboxes[ncboxes].it1 = 0;
    return &cboxes[ncboxes++];
}

static void free_items(void)
{
    while (chunks) { TextChunk *n = chunks->next; free(chunks); chunks = n; }
    nitems = 0;
    ncboxes = 0;
    find_valid = 0;
    hot_box = copied_box = -1;
    btn_hot = 0;
    sel_a.item = sel_b.item = -1;
}

static void mark_first(int from, int sep)
{
    int i;
    for (i = from; i < nitems; i++)
        if (items[i].kind == IT_TEXT) { items[i].sep = sep; return; }
}

/* ---- text flow ---- */

typedef struct { const char *s; int len, w, fnt, span, link, sp_before; COLORREF col, bg; } Tok;

static Tok *tokenize(const Inline *in, int px, unsigned base, COLORREF basecol, int maxw, int *count)
{
    Tok *t = NULL;
    int n = 0, cap = 0, k, carry = 0, prev_spw = 0;

    for (k = 0; k < in->n; k++) {
        const Span *sp = &in->s[k];
        unsigned fl = base | sp->flags;
        int code = (fl & F_CODE) != 0;
        int fnt = font_get(code ? px : px, (fl & F_BOLD) != 0, (fl & (F_ITAL | F_IMG)) != 0,
                           code, (fl & F_LINK) != 0);
        COLORREF col = (fl & F_LINK) ? C_LINK : code ? C_CODE : (fl & F_IMG) ? C_GRAY : basecol;
        const char *p = sp->text;

        while (*p) {
            const char *start;
            int len, first = 1;
            if (isspace((unsigned char)*p)) { carry = 1; p++; continue; }
            start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            len = (int)(p - start);

            while (len > 0) {                 /* long words are split to fit maxw */
                int w = text_w(fnt, start, len), take = len;
                Tok *tk;
                if (maxw > 0 && w > maxw) {
                    SIZE sz;
                    int fit = 1;
                    SelectObject(mdc, fonts[fnt].h);
                    GetTextExtentExPoint(mdc, start, len, maxw, &fit, NULL, &sz);
                    if (fit < 1) fit = 1;
                    take = fit;
                    w = text_w(fnt, start, take);
                }
                if (n == cap) {
                    cap = cap ? cap * 2 : 32;
                    t = (Tok *)realloc(t, cap * sizeof(Tok));
                }
                tk = &t[n++];
                tk->s = start; tk->len = take; tk->w = w; tk->fnt = fnt; tk->span = k;
                tk->link = sp->link; tk->col = col;
                tk->bg = code ? C_INLINEBG : NOBACK;
                tk->sp_before = (first && carry) ? (prev_spw ? prev_spw : fonts[fnt].spw) : 0;
                first = 0;
                carry = 0;
                prev_spw = fonts[fnt].spw;
                start += take;
                len -= take;
            }
        }
    }
    *count = n;
    return t;
}

/* lay tokens out in lines of width w; returns the height used. emit=0 only measures. */
static int layout_toks(Tok *t, int n, int x, int w, int y, int align, int emit)
{
    int i = 0, cy = y, linegap = S(3);

    while (i < n) {
        int j = i, lw = 0, maxasc = 0, maxdesc = 0, k;
        while (j < n) {
            int add = t[j].w + (j > i ? t[j].sp_before : 0);
            if (j > i && lw + add > w) break;
            lw += add;
            j++;
        }
        for (k = i; k < j; k++) {
            FontEnt *f = &fonts[t[k].fnt];
            if (f->asc > maxasc) maxasc = f->asc;
            if (f->ht - f->asc > maxdesc) maxdesc = f->ht - f->asc;
        }
        if (emit) {
            int cx = x + (align == 1 ? (w - lw) / 2 : align == 2 ? (w - lw) : 0);
            k = i;
            while (k < j) {
                int e = k, fw = t[k].w, flen = t[k].len, fx;
                char *txt;
                FontEnt *f = &fonts[t[k].fnt];
                Item *it;

                if (k > i) cx += t[k].sp_before;
                fx = cx;
                /* merge following tokens of the same span into one fragment */
                while (e + 1 < j && t[e + 1].span == t[k].span && t[e + 1].fnt == t[k].fnt) {
                    e++;
                    fw += t[e].sp_before + t[e].w;
                    flen += (t[e].sp_before ? 1 : 0) + t[e].len;
                }
                txt = text_alloc(flen + 1);
                {
                    int pos = 0, q;
                    for (q = k; q <= e; q++) {
                        if (q > k && t[q].sp_before) txt[pos++] = ' ';
                        memcpy(txt + pos, t[q].s, t[q].len);
                        pos += t[q].len;
                    }
                    txt[pos] = 0;
                    flen = pos;
                }
                if (t[k].bg != NOBACK)
                    rect(fx - S(2), cy + (maxasc - f->asc) - 1, fw + S(4), f->ht + 2, t[k].bg);
                it = add_item(IT_TEXT, fx, cy + (maxasc - f->asc), fw, f->ht, t[k].col);
                it->font = t[k].fnt;
                it->link = t[k].link;
                it->text = txt;
                it->len = flen;
                cx = fx + fw;
                k = e + 1;
            }
        }
        cy += maxasc + maxdesc + linegap;
        i = j;
    }
    return cy - y;
}

static int flow(const Inline *in, int x, int w, int y, int px, unsigned base,
                COLORREF col, int align, int emit)
{
    int n, h;
    Tok *t = tokenize(in, px, base, col, w, &n);
    if (n == 0) {
        h = fonts[font_get(px, 0, 0, 0, 0)].ht + S(3);
    } else {
        h = layout_toks(t, n, x, w, y, align, emit);
    }
    free(t);
    return h;
}

static void natural_width(const Inline *in, int px, unsigned base, int *nat, int *longest)
{
    int n, k, total = 0, lg = 0;
    Tok *t = tokenize(in, px, base, 0, 0, &n);
    for (k = 0; k < n; k++) {
        total += t[k].w + (k ? t[k].sp_before : 0);
        if (t[k].w > lg) lg = t[k].w;
    }
    free(t);
    *nat = total;
    *longest = lg;
}

/* ---- block layout ---- */

/* ---- syntax highlighting for fenced code ---- */

enum { HC_PLAIN, HC_KW, HC_CTRL, HC_STR, HC_COM, HC_NUM, HC_TYPE, HC_PRE };
static const int hl_pal[] = { P_BLOCK, P_KW, P_CTRL, P_STR, P_COM, P_NUM, P_TYPE, P_PRE };
enum { FAM_NONE, FAM_C, FAM_HASH, FAM_SQL, FAM_JSON, FAM_MARKUP };

static const char *kw_ctrl =
    " if else elif for foreach while do switch case default break continue return goto try catch except finally"
    " throw throws raise yield await in is not and or with as from import export using namespace package match"
    " then fi done esac begin end loop when unless until elsif ";
static const char *kw_main =
    " auto bool boolean byte char short int long float double void unsigned signed const static volatile extern"
    " register struct union enum typedef sizeof class interface public private protected virtual override final"
    " new delete this self super true false null nil none undefined var let val fn func function def lambda"
    " string str object dynamic readonly abstract sealed internal operator template typename inline mut pub impl"
    " trait type where mod use crate local echo set pass del global nonlocal async extends implements get"
    " defer chan go map range select fun data record init constructor ref out params checked unchecked"
    " explicit implicit event delegate partial base NULL TRUE FALSE ";
static const char *kw_sql =
    " select from where insert into values update set delete create table drop alter index primary key foreign"
    " references join inner left right outer on group by order having limit offset union distinct as and or not"
    " null is in like between exists case when then else end count sum avg min max int integer varchar text date"
    " view begin commit rollback with all asc desc ";

static int in_list(const char *list, const char *w, int n)
{
    char key[40];
    if (n > 30) return 0;
    key[0] = ' ';
    memcpy(key + 1, w, n);
    key[n + 1] = ' ';
    key[n + 2] = 0;
    return strstr(list, key) != NULL;
}

static int lang_family(const char *lang, int *pre_ok)
{
    static const char *c_like[] = { "c", "cpp", "c++", "cc", "h", "hpp", "cs", "csharp", "java", "js", "javascript",
        "jsx", "ts", "typescript", "tsx", "go", "rust", "rs", "php", "swift", "kotlin", "kt", "scala", "dart",
        "objc", "css", "scss", "glsl", "hlsl", "groovy", "gradle", "zig", "d", NULL };
    static const char *hashy[] = { "py", "python", "sh", "bash", "shell", "zsh", "ps1", "powershell", "pwsh", "yaml",
        "yml", "rb", "ruby", "toml", "ini", "conf", "perl", "pl", "r", "make", "makefile", "dockerfile", "cmake", "nim", NULL };
    static const char *pre_langs[] = { "c", "cpp", "c++", "cc", "h", "hpp", "cs", "csharp", "objc", NULL };
    int i;
    *pre_ok = 0;
    for (i = 0; pre_langs[i]; i++) if (!strcmp(lang, pre_langs[i])) *pre_ok = 1;
    for (i = 0; c_like[i]; i++) if (!strcmp(lang, c_like[i])) return FAM_C;
    for (i = 0; hashy[i]; i++) if (!strcmp(lang, hashy[i])) return FAM_HASH;
    if (!strcmp(lang, "sql")) return FAM_SQL;
    if (!strcmp(lang, "json") || !strcmp(lang, "jsonc")) return FAM_JSON;
    if (!strcmp(lang, "html") || !strcmp(lang, "htm") || !strcmp(lang, "xml") || !strcmp(lang, "svg") ||
        !strcmp(lang, "xaml")) return FAM_MARKUP;
    return FAM_NONE;
}

/* classify each character of one line; state carries multi-line constructs: 1 block comment, 2 and 3 triple strings, 4 html comment */
static void hl_line(const char *s, int n, unsigned char *cls, int fam, int pre_ok, int *state)
{
    int i = 0, j;

    if (pre_ok && !*state) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i < n && s[i] == '#') { memset(cls + i, HC_PRE, n - i); return; }
        i = 0;
    }
    while (i < n) {
        char c = s[i];
        if (*state) {                                    /* inside a multi-line construct */
            const char *end = *state == 1 ? "*/" : *state == 2 ? "\"\"\"" : *state == 3 ? "'''" : "-->";
            int el = (int)strlen(end), hc = (*state == 2 || *state == 3) ? HC_STR : HC_COM;
            for (j = i; j < n; j++) {
                cls[j] = (unsigned char)hc;
                if (j + el <= n && !memcmp(s + j, end, el)) {
                    memset(cls + j, hc, el);
                    j += el;
                    *state = 0;
                    break;
                }
            }
            i = j < n ? j : n;
            continue;
        }
        if (fam == FAM_MARKUP) {
            if (!strncmp(s + i, "<!--", 4)) { *state = 4; memset(cls + i, HC_COM, 4); i += 4; continue; }
            if (c == '<') {
                cls[i++] = HC_PLAIN;
                if (i < n && (s[i] == '/' || s[i] == '?' || s[i] == '!')) cls[i++] = HC_PLAIN;
                for (j = i; j < n && s[j] != ' ' && s[j] != '>' && s[j] != '/'; j++) cls[j] = HC_KW;
                i = j;
                while (i < n && s[i] != '>') {
                    if (s[i] == '"' || s[i] == '\'') {
                        char q = s[i];
                        cls[i++] = HC_STR;
                        while (i < n && s[i] != q) cls[i++] = HC_STR;
                        if (i < n) cls[i++] = HC_STR;
                    } else {
                        cls[i] = (isalpha((unsigned char)s[i]) || s[i] == '-' || s[i] == ':') ? HC_TYPE : HC_PLAIN;
                        i++;
                    }
                }
                continue;
            }
            cls[i++] = HC_PLAIN;
            continue;
        }
        /* comments */
        if ((fam == FAM_C || fam == FAM_JSON) && c == '/' && i + 1 < n && s[i + 1] == '/') {
            memset(cls + i, HC_COM, n - i);
            return;
        }
        if (fam == FAM_SQL && c == '-' && i + 1 < n && s[i + 1] == '-') {
            memset(cls + i, HC_COM, n - i);
            return;
        }
        if ((fam == FAM_C || fam == FAM_SQL || fam == FAM_JSON) && c == '/' && i + 1 < n && s[i + 1] == '*') {
            *state = 1; cls[i] = cls[i + 1] = HC_COM; i += 2;
            continue;
        }
        if (fam == FAM_HASH && c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            memset(cls + i, HC_COM, n - i);
            return;
        }
        /* strings */
        if (fam == FAM_HASH && (c == '"' || c == '\'') && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
            *state = c == '"' ? 2 : 3; memset(cls + i, HC_STR, 3); i += 3;
            continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && fam == FAM_C)) {
            cls[i++] = HC_STR;
            while (i < n && s[i] != c) {
                if (s[i] == '\\' && i + 1 < n) cls[i++] = HC_STR;
                cls[i++] = HC_STR;
            }
            if (i < n) cls[i++] = HC_STR;
            continue;
        }
        if (isdigit((unsigned char)c) &&
            (i == 0 || !(isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_' || s[i - 1] == '$'))) {
            for (j = i; j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_'); j++) cls[j] = HC_NUM;
            i = j;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            int hc = HC_PLAIN;
            for (j = i; j < n && (isalnum((unsigned char)s[j]) || s[j] == '_'); j++) ;
            if (fam == FAM_JSON) {
                if (in_list(" true false null ", s + i, j - i)) hc = HC_KW;
            } else if (fam == FAM_SQL) {
                char low[40];
                int q, len = j - i < 39 ? j - i : 39;
                for (q = 0; q < len; q++) low[q] = (char)tolower((unsigned char)s[i + q]);
                if (in_list(kw_sql, low, len)) hc = HC_KW;
            } else if (in_list(kw_ctrl, s + i, j - i)) {
                hc = HC_CTRL;
            } else if (in_list(kw_main, s + i, j - i)) {
                hc = HC_KW;
            } else if (isupper((unsigned char)c)) {
                int q;
                for (q = 1; q < j - i; q++) if (islower((unsigned char)s[i + q])) { hc = HC_TYPE; break; }
            }
            memset(cls + i, hc, j - i);
            i = j;
            continue;
        }
        cls[i++] = HC_PLAIN;
    }
}

static int head_pt(int level)
{
    static const int pts[] = {0, 20, 16, 14, 12, 11, 11};
    return pts[level];
}

static int block_gap(const Block *prev, const Block *b)
{
    if (!prev) return 0;
    if (b->kind == BK_LIST && prev->kind == BK_LIST) {
        if (b->level == 0 && prev->level == 0 && b->numbered != prev->numbered) return S(12);
        return S(4);
    }
    if (b->inset || (prev->inset && b->kind == BK_LIST)) return S(8);
    if (prev->kind == BK_HEAD) return S(8);
    if (b->kind == BK_HEAD) return S(b->level <= 2 ? 22 : 16);
    return S(12);
}

static void layout_table(Block *b, int x0, int avail, int *py)
{
    int cols = b->cols, rows = b->rows, r, c;
    int px = PX(11), padx = S(9), pady = S(2);
    int nat[MAXCOLS] = {0}, mn[MAXCOLS] = {0}, cw[MAXCOLS] = {0};
    int *rh = (int *)malloc((rows + 1) * sizeof(int));
    int sumnat = 0, summin = 0, tw = 0, y = *py, x;

    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            int n, l;
            natural_width(&b->cells[r * cols + c], px, (b->header && r == 0) ? F_BOLD : 0, &n, &l);
            n += 2 * padx; l += 2 * padx;
            if (n > nat[c]) nat[c] = n;
            if (l > mn[c]) mn[c] = l;
        }
    for (c = 0; c < cols; c++) { sumnat += nat[c]; summin += mn[c]; }
    if (sumnat <= avail) {
        for (c = 0; c < cols; c++) cw[c] = nat[c];
    } else if (summin >= avail) {
        for (c = 0; c < cols; c++) cw[c] = mn[c];
    } else {
        int extra = avail - summin, need = sumnat - summin;
        for (c = 0; c < cols; c++) cw[c] = mn[c] + (int)((double)(nat[c] - mn[c]) * extra / need);
    }
    for (c = 0; c < cols; c++) tw += cw[c];

    for (r = 0; r < rows; r++) {                     /* row heights */
        int h = 0;
        for (c = 0; c < cols; c++) {
            int ch = flow(&b->cells[r * cols + c], 0, cw[c] - 2 * padx, 0, px,
                          (b->header && r == 0) ? F_BOLD : 0, C_TEXT, 0, 0);
            if (ch > h) h = ch;
        }
        rh[r] = h + 2 * pady;
    }

    /* backgrounds, then grid, then text */
    for (r = 0, y = *py; r < rows; y += rh[r], r++) {
        if (b->header && r == 0) rect(x0, y, tw, rh[r], C_THEAD);
        else if ((r - (b->header ? 1 : 0)) % 2 == 1) rect(x0, y, tw, rh[r], C_ZEBRA);
    }
    for (r = 0, y = *py; r <= rows; r++) {
        rect(x0, y, tw + 1, 1, C_GRID);
        if (r < rows) y += rh[r];
    }
    for (c = 0, x = x0; c <= cols; c++) {
        rect(x, *py, 1, y - *py + 1, C_GRID);
        if (c < cols) x += cw[c];
    }
    for (r = 0, y = *py; r < rows; y += rh[r], r++) {
        for (c = 0, x = x0; c < cols; x += cw[c], c++) {
            int first = nitems;
            flow(&b->cells[r * cols + c], x + padx, cw[c] - 2 * padx, y + pady, px,
                 (b->header && r == 0) ? F_BOLD : 0, C_TEXT, b->align[c], 1);
            mark_first(first, c == 0 ? 1 : 2);
        }
    }
    *py = y + 1;
    free(rh);
}

static int lay_cwid, lay_x0;
static int g_diag_layout;
static DWORD g_layout_ms;      /* how long the last full layout took: decides whether to defer relayouts */

static void content_box(int cw, int *pcwid, int *px0)
{
    int cwid = cw - 2 * S(22), x0;
    if (g_limit_width && cwid > S(1200)) cwid = S(1200);
    if (cwid < S(120)) cwid = S(120);
    x0 = (cw - cwid) / 2;
    if (x0 < S(8)) x0 = S(8);
    *pcwid = cwid; *px0 = x0;
}

/* a wider window with the same content width only moves everything sideways */
static int shift_layout(int cw)
{
    int cwid, x0, i, dx;
    content_box(cw, &cwid, &x0);
    if (!nitems || cwid != lay_cwid) return 0;
    dx = x0 - lay_x0;
    for (i = 0; i < nitems; i++) items[i].x += dx;
    for (i = 0; i < ncboxes; i++) { cboxes[i].x += dx; cboxes[i].bx += dx; }
    lay_x0 = x0;
    return 1;
}

static void layout_all(int cw)
{
    int cwid, x0, y, i;
    const Block *prev = NULL;
    DWORD t0 = GetTickCount();

    free_items();
    content_box(cw, &cwid, &x0);
    lay_cwid = cwid; lay_x0 = x0;
    y = S(12);

    for (i = 0; i < nblocks; i++) {
        Block *b = &blocks[i];
        int start = nitems, ix = 0;
        y += block_gap(prev, b);
        b->y = y;
        if (b->inset && (b->kind == BK_PARA || b->kind == BK_CODE)) {       /* inside a list item */
            ix = S(8) + (b->inset - 1) * S(26) + S(24);
            x0 += ix;
            cwid -= ix;
        }

        switch (b->kind) {
        case BK_PARA:
            y += flow(&b->inl, x0, cwid, y, PX(11), 0, C_TEXT, 0, 1);
            break;
        case BK_HEAD: {
            int h = flow(&b->inl, x0, cwid, y, PX(head_pt(b->level)), F_BOLD, C_HEAD, 0, 1);
            y += h;
            if (b->level <= 2) {
                rect(x0, y + S(3), cwid, 1, C_RULE);
                y += S(5);
            }
            break;
        }
        case BK_LIST: {
            int px = PX(11);
            int base = S(8) + b->level * S(26);
            int tx = x0 + base + S(24);
            int first_ht = fonts[font_get(px, 0, 0, 0, 0)].ht;
            int h;
            if (b->numbered) {
                char label[24];
                int n = sprintf(label, "%s.", b->num);
                int f = font_get(px, 0, 0, 0, 0);
                int w = text_w(f, label, n);
                Item *it = add_item(IT_TEXT, tx - S(6) - w, y, w, first_ht, C_GRAY);
                it->font = f;
                it->text = text_alloc(n + 1);
                strcpy(it->text, label);
                it->len = n;
            } else {
                int d = S(6), cy = y + first_ht / 2 - d / 2;
                if (b->level == 0)      add_item(IT_DISC, tx - S(14), cy, d, d, C_TEXT);
                else if (b->level == 1) add_item(IT_RING, tx - S(14), cy, d, d, C_TEXT);
                else                    rect(tx - S(14), cy, d - 1, d - 1, C_TEXT);
            }
            h = flow(&b->inl, tx, cwid - (tx - x0), y, px, 0, C_TEXT, 0, 1);
            y += h;
            break;
        }
        case BK_QUOTE: {
            int qx = x0 + S(18), h;
            h = flow(&b->inl, qx, cwid - S(18), y, PX(11), F_ITAL, C_GRAY, 0, 0);
            rect(x0 + S(2), y, S(4), h, C_QBAR);
            flow(&b->inl, qx, cwid - S(18), y, PX(11), F_ITAL, C_GRAY, 0, 1);
            y += h;
            break;
        }
        case BK_CODE: {
            int pad = S(10), fnt = font_get(PX(11), 0, 0, 1, 0);
            int lh = fonts[fnt].ht + S(1), charw = text_w(fnt, "M", 1);
            int k, h, ly, pre_ok, hstate = 0, maxcols = 0, hmax, it0;
            int fam = lang_family(b->num, &pre_ok);
            if (!charw) charw = 1;
            for (k = 0; k < b->nlines; k++) {
                int cols = 0;
                const char *q;
                for (q = b->lines[k]; *q; q++) cols += *q == '\t' ? 4 : 1;
                if (cols > maxcols) maxcols = cols;
            }
            hmax = maxcols * charw + 2 * pad - cwid;            /* long lines scroll sideways instead of wrapping */
            if (hmax < 0) hmax = 0;
            h = (b->nlines ? b->nlines : 1) * lh + 2 * pad + (hmax ? S(12) : 0);
            rect(x0, y, cwid, h, C_BLOCKBD);
            rect(x0 + 1, y + 1, cwid - 2, h - 2, C_BLOCKBG);
            it0 = nitems;
            ly = y + pad;
            for (k = 0; k < b->nlines; k++) {
                char *src = b->lines[k], *exp = (char *)malloc(strlen(src) * 4 + 1);
                unsigned char *cls;
                int len = 0, first, r0 = 0;
                for (; *src; src++) {
                    if (*src == '\t') { int t = 4; while (t--) exp[len++] = ' '; }
                    else exp[len++] = *src;
                }
                exp[len] = 0;
                cls = (unsigned char *)calloc(len + 1, 1);
                if (fam) hl_line(exp, len, cls, fam, pre_ok, &hstate);
                first = nitems;
                while (r0 < len) {                              /* one item per run of the same colour */
                    int r1 = r0 + 1;
                    Item *it;
                    while (r1 < len && cls[r1] == cls[r0]) r1++;
                    it = add_item(IT_TEXT, x0 + pad + r0 * charw, ly, (r1 - r0) * charw, fonts[fnt].ht,
                                  pal[hl_pal[cls[r0]]]);
                    it->font = fnt;
                    it->box = ncboxes + 1;
                    it->text = text_alloc(r1 - r0 + 1);
                    memcpy(it->text, exp + r0, r1 - r0);
                    it->text[r1 - r0] = 0;
                    it->len = r1 - r0;
                    r0 = r1;
                }
                ly += lh;
                if (k > 0) mark_first(first, 1);
                free(exp);
                free(cls);
            }
            {
                CodeBox *cb = add_cbox(x0, y, cwid, h, i);
                cb->font = font_get(PX(9), 0, 0, 0, 0);
                cb->bh = fonts[cb->font].ht + S(11);
                cb->bw = cb->bh;
                cb->bx = x0 + cwid - cb->bw - S(6);
                cb->by = y + S(5);
                cb->hmax = hmax;
                cb->it0 = it0;
                cb->it1 = nitems;
            }
            y += h;
            break;
        }
        case BK_TABLE:
            layout_table(b, x0, cwid, &y);
            break;
        case BK_RULE:
            rect(x0, y + S(4), cwid, S(2) > 1 ? S(2) : 1, C_RULE);
            y += S(8);
            break;
        }
        x0 -= ix;
        cwid += ix;
        mark_first(start, b->kind == BK_LIST ? 1 : 3);
        prev = b;
    }
    doc_h = y + S(28);
    g_layout_ms = GetTickCount() - t0;
    if (g_diag_layout) {
        char t[80];
        wsprintf(t, "layout %lu ms, %d items, %d blocks", (unsigned long)g_layout_ms, nitems, nblocks);
        SetWindowText(hwnd, t);
    }
}

/* ---- document text (for Copy) ---- */

static char *sb;
static int sb_len, sb_cap;

static void sb_add(const char *s)
{
    int n = strlen(s);
    if (sb_len + n + 1 > sb_cap) {
        sb_cap = (sb_len + n + 1) * 2;
        sb = (char *)realloc(sb, sb_cap);
    }
    memcpy(sb + sb_len, s, n + 1);
    sb_len += n;
}

static void sb_inline(const Inline *in)
{
    int k;
    for (k = 0; k < in->n; k++) sb_add(in->s[k].text);
}

static char *doc_text(void)
{
    int i, r, c, k;
    sb = NULL; sb_len = sb_cap = 0;
    sb_add("");
    for (i = 0; i < nblocks; i++) {
        Block *b = &blocks[i];
        switch (b->kind) {
        case BK_LIST:
            if (b->numbered) { sb_add(b->num); sb_add(". "); } else sb_add("- ");
            sb_inline(&b->inl);
            sb_add("\r\n");
            break;
        case BK_CODE:
            for (k = 0; k < b->nlines; k++) { sb_add(b->lines[k]); sb_add("\r\n"); }
            sb_add("\r\n");
            break;
        case BK_TABLE:
            for (r = 0; r < b->rows; r++) {
                for (c = 0; c < b->cols; c++) {
                    if (c) sb_add("\t");
                    sb_inline(&b->cells[r * b->cols + c]);
                }
                sb_add("\r\n");
            }
            sb_add("\r\n");
            break;
        case BK_RULE:
            sb_add("---\r\n\r\n");
            break;
        default:
            if (b->kind == BK_QUOTE) sb_add("> ");
            sb_inline(&b->inl);
            sb_add("\r\n\r\n");
        }
    }
    return sb;
}

/* ---- text decoding: UTF-8 -> Windows-1252 ---- */

static char *utf8_to_ansi(const unsigned char *s, long n)
{
    char *o = (char *)malloc(n + 1), *p = o;
    long i = 0;

    while (i < n) {
        unsigned long c = s[i], cp;
        int len;
        if (c < 0x80) { *p++ = (char)c; i++; continue; }
        if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3FUL) << 6) | (s[i + 2] & 0x3F); len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cp = ((c & 0x07UL) << 18) | ((s[i + 1] & 0x3FUL) << 12) | ((s[i + 2] & 0x3FUL) << 6) | (s[i + 3] & 0x3F); len = 4;
        } else {
            *p++ = '?'; i++; continue;
        }
        i += len;
        if (cp >= 0xA0 && cp < 0x100) { *p++ = (char)cp; continue; }
        switch (cp) {
        case 0xFEFF: break;
        case 0x2018: *p++ = (char)0x91; break;
        case 0x2019: *p++ = (char)0x92; break;
        case 0x201C: *p++ = (char)0x93; break;
        case 0x201D: *p++ = (char)0x94; break;
        case 0x2022: *p++ = (char)0x95; break;
        case 0x2013: *p++ = (char)0x96; break;
        case 0x2014: *p++ = (char)0x97; break;
        case 0x2026: *p++ = (char)0x85; break;
        case 0x2122: *p++ = (char)0x99; break;
        case 0x20AC: *p++ = (char)0x80; break;
        case 0x2192: *p++ = '-'; *p++ = '>'; break;
        case 0x2190: *p++ = '<'; *p++ = '-'; break;
        default:
            if (cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || cp == 0x200B || (cp >= 0x1F3FB && cp <= 0x1F3FF)) break;
            {
                int code = g_uni && cp >= 0x100 ? ph_get(cp) : 0;
                *p++ = code ? (char)code : '?';
            }
            break;
        }
    }
    *p = 0;
    return o;
}

/* ---- window: scrolling, painting, input ---- */

static char curfile[MAX_PATH];
static HBITMAP backbmp;
static HDC backdc;
static HCURSOR cur_arrow, cur_hand, cur_ibeam;

static int sb_factor(void) { return doc_h > 30000 ? doc_h / 30000 + 1 : 1; }

/* like VS Code, the document can scroll until just its last lines are left at the top */
static int max_scroll(void)
{
    int m = doc_h - S(32) - fonts[font_get(PX(11), 0, 0, 0, 0)].ht;     /* the last line reaches the top */
    return m > 0 ? m : 0;
}

static void update_scrollbar(void)
{
    SCROLLINFO si;
    int f = sb_factor();
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nPage = view_h / f;
    si.nMax = max_scroll() / f + (int)si.nPage - 1;
    si.nPos = g_scroll / f;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

static void update_hover(void);
static void tip_reset(void);

static int anim_target = -1, anim_t0, in_anim;

static void set_scroll(int pos)
{
    int max = max_scroll();
    if (!in_anim && anim_target >= 0) { KillTimer(hwnd, 5); anim_target = -1; }
    if (max < 0) max = 0;
    if (pos > max) pos = max;
    if (pos < 0) pos = 0;
    if (pos != g_scroll) {
        g_scroll = pos;
        tip_reset();
        InvalidateRect(hwnd, NULL, FALSE);
    }
    update_scrollbar();
    update_hover();
}

/* smooth scrolling: ease toward a target, with steps sized by elapsed time so a slow machine just skips frames */
static int scroll_base(void) { return anim_target >= 0 ? anim_target : g_scroll; }

static void smooth_scroll(int pos)
{
    int max = max_scroll();
    if (pos > max) pos = max;
    if (pos < 0) pos = 0;
    if (!g_smooth || abs(pos - g_scroll) < 2) { set_scroll(pos); return; }
    anim_target = pos;
    anim_t0 = (int)GetTickCount();
    SetTimer(hwnd, 5, 15, NULL);
}

static void anim_step(void)
{
    int now = (int)GetTickCount(), dt = now - anim_t0, diff, mv;
    int tgt = anim_target;
    if (tgt < 0) { KillTimer(hwnd, 5); return; }
    anim_t0 = now;
    if (dt < 1) dt = 1;
    if (dt > 200) dt = 200;
    diff = tgt - g_scroll;
    mv = diff * dt / (dt + 28);
    if (abs(mv) < S(3)) mv = diff > 0 ? S(3) : -S(3);       /* no long slow tail */
    if (abs(mv) >= abs(diff)) mv = diff;
    in_anim = 1;
    set_scroll(g_scroll + mv);
    in_anim = 0;
    if (g_scroll == tgt || !mv) { KillTimer(hwnd, 5); anim_target = -1; }
}

static void jump_to(int y)
{
    set_scroll(y - S(12));
}

static void relayout(void)
{
    layout_all(view_w);
    set_scroll(g_scroll);
}

static void make_backbuffer(void)
{
    HDC dc = GetDC(hwnd);
    if (backbmp) { SelectObject(backdc, GetStockObject(BLACK_PEN)); DeleteObject(backbmp); }
    backbmp = CreateCompatibleBitmap(dc, view_w > 0 ? view_w : 1, view_h > 0 ? view_h : 1);
    SelectObject(backdc, backbmp);
    ReleaseDC(hwnd, dc);
}

/* ---- text selection ---- */

static int sel_norm(SelPos *a, SelPos *b)
{
    if (sel_a.item < 0 || sel_b.item < 0) return 0;
    if (sel_a.item < sel_b.item || (sel_a.item == sel_b.item && sel_a.off <= sel_b.off)) {
        *a = sel_a; *b = sel_b;
    } else {
        *a = sel_b; *b = sel_a;
    }
    return a->item != b->item || a->off != b->off;
}

/* text with embedded Unicode characters: plain runs with TextOut, the others with the Unicode symbol fonts */
static void draw_run(HDC dc, int x, int y, const char *s, int len, int fnt)
{
    const char *e = s + len;
    if (!nph) { TextOut(dc, x, y, s, len); return; }
    while (s < e) {
        const char *r = s;
        while (r < e && ((unsigned char)*r >= 32 || !ph_cp[(unsigned char)*r])) r++;
        if (r > s) {
            TextOut(dc, x, y, s, (int)(r - s));
            x += text_w(fnt, s, (int)(r - s));
        }
        if (r < e) {
            unsigned c = (unsigned char)*r;
            wchar_t w[2];
            int n = wide_of(ph_cp[c], w);
            SelectObject(dc, sym_font(fonts[fnt].px, ph_cp[c]));
            SetTextAlign(dc, TA_BASELINE | TA_LEFT);
            pExtTextOutW(dc, x, y + fonts[fnt].asc, 0, NULL, w, n, NULL);
            SetTextAlign(dc, TA_TOP | TA_LEFT);
            SelectObject(dc, fonts[fnt].h);
            x += ph_width(fnt, c);
            r++;
        }
        s = r;
    }
}

/* width of the first n chars of an item */
static int prefix_w(HDC dc, const Item *it, int n)
{
    (void)dc;
    return n > 0 ? text_w(it->font, it->text, n) : 0;
}

/* the char range of item i that is selected; returns 0 if none */
static int sel_range(int i, const SelPos *a, const SelPos *b, int *s0, int *s1)
{
    if (i < a->item || i > b->item) return 0;
    *s0 = (i == a->item) ? a->off : 0;
    *s1 = (i == b->item) ? b->off : items[i].len;
    return *s0 < *s1;
}

/* the text position nearest to a client-area point */
static SelPos pos_at(int x, int y)
{
    SelPos p = {-1, 0};
    int i, bdy = 0x7fffffff, bdx = 0x7fffffff;
    Item *it;

    y += g_scroll;
    for (i = 0; i < nitems; i++) {
        int dy, dx;
        it = &items[i];
        if (it->kind != IT_TEXT || !it->len) continue;
        dy = y < it->y ? it->y - y : (y >= it->y + it->h ? y - (it->y + it->h) + 1 : 0);
        dx = x < it->x ? it->x - x : (x > it->x + it->w ? x - it->x - it->w : 0);
        if (dy < bdy || (dy == bdy && dx < bdx)) { bdy = dy; bdx = dx; p.item = i; }
    }
    if (p.item < 0) return p;
    it = &items[p.item];
    if (x <= it->x) {
        p.off = 0;
    } else if (x >= it->x + it->w) {
        p.off = it->len;
    } else {
        int fit = 0, w1 = 0, w2 = 0, dx = x - it->x;
        while (fit < it->len) {
            w2 = w1 + text_w(it->font, it->text + fit, 1);
            if (w2 > dx) break;
            w1 = w2;
            fit++;
        }
        if (fit >= it->len) w2 = w1;
        p.off = (dx - w1 < w2 - dx) ? fit : (fit < it->len ? fit + 1 : fit);
    }
    return p;
}

static int is_word_char(unsigned char ch)
{
    return isalnum(ch) || ch == '_' || ch >= 128;
}

static void select_word(SelPos p)
{
    const Item *it = &items[p.item];
    int s = p.off, e = p.off;
    while (s > 0 && is_word_char((unsigned char)it->text[s - 1])) s--;
    while (e < it->len && is_word_char((unsigned char)it->text[e])) e++;
    if (s == e && e < it->len) e++;
    sel_a.item = sel_b.item = p.item;
    sel_a.off = s;
    sel_b.off = e;
}

/* a whole paragraph, list item or table cell */
static void select_block(int i)
{
    int s = i, e = i, j;
    while (s > 0 && !(items[s].kind == IT_TEXT && items[s].sep)) s--;
    for (j = i + 1; j < nitems; j++) {
        if (items[j].kind != IT_TEXT) continue;
        if (items[j].sep) break;
        e = j;
    }
    sel_a.item = s; sel_a.off = 0;
    sel_b.item = e; sel_b.off = items[e].len;
}

static void select_all(void)
{
    int i, first = -1, last = -1;
    for (i = 0; i < nitems; i++)
        if (items[i].kind == IT_TEXT && items[i].len) { if (first < 0) first = i; last = i; }
    if (first < 0) return;
    sel_a.item = first; sel_a.off = 0;
    sel_b.item = last;  sel_b.off = items[last].len;
}

static void sb_addn(const char *s, int n)
{
    if (sb_len + n + 1 > sb_cap) {
        sb_cap = (sb_len + n + 1) * 2;
        sb = (char *)realloc(sb, sb_cap);
    }
    memcpy(sb + sb_len, s, n);
    sb_len += n;
    sb[sb_len] = 0;
}

/* the selected text, as a malloc'd string (NULL when nothing is selected) */
static char *selection_text(void)
{
    SelPos a, b;
    const Item *prev = NULL;
    int i;

    if (!sel_norm(&a, &b)) return NULL;
    sb = NULL; sb_len = sb_cap = 0;
    sb_addn("", 0);
    for (i = a.item; i <= b.item && i < nitems; i++) {
        const Item *it = &items[i];
        int s0, s1;
        if (it->kind != IT_TEXT || !sel_range(i, &a, &b, &s0, &s1)) continue;
        if (prev) {
            if (it->sep == 3)      sb_addn("\r\n\r\n", 4);
            else if (it->sep == 1) sb_addn("\r\n", 2);
            else if (it->sep == 2) sb_addn("\t", 1);
            else if (it->y - prev->y > prev->h / 2) sb_addn(" ", 1);       /* wrapped line */
            else if (it->x - (prev->x + prev->w) > 1) sb_addn(" ", 1);
        }
        sb_addn(it->text + s0, s1 - s0);
        prev = it;
    }
    return sb;
}

/* sideways scrolling of one code block: its items just move */
static void box_hscroll(int bi, int hs)
{
    CodeBox *cb = &cboxes[bi];
    int i, d;
    if (hs > cb->hmax) hs = cb->hmax;
    if (hs < 0) hs = 0;
    d = hs - cb->hs;
    if (!d) return;
    for (i = cb->it0; i < cb->it1; i++) items[i].x -= d;
    cb->hs = hs;
    InvalidateRect(hwnd, NULL, FALSE);
}

/* the horizontal scrollbar of a code block, in document coordinates */
static int sbar_geom(const CodeBox *cb, RECT *tr, RECT *th)
{
    int view = cb->w - 2 * S(10), tw, thw, hh = S(8) > 6 ? S(8) : 6;
    if (!cb->hmax) return 0;
    tr->left = cb->x + S(8); tr->right = cb->x + cb->w - S(8);
    tr->bottom = cb->y + cb->h - S(4); tr->top = tr->bottom - hh;
    tw = tr->right - tr->left;
    thw = (int)((long)tw * view / (view + cb->hmax));
    if (thw < S(24)) thw = S(24);
    if (thw > tw) thw = tw;
    th->top = tr->top; th->bottom = tr->bottom;
    th->left = tr->left + (tw - thw) * cb->hs / cb->hmax;
    th->right = th->left + thw;
    return 1;
}

/* the code block whose scrollbar is under a client point; *thumb tells whether it is on the thumb */
static int sbar_hit(int x, int y, int *thumb)
{
    int i, slop = S(3);
    y += g_scroll;
    for (i = 0; i < ncboxes; i++) {
        RECT tr, th;
        if (!sbar_geom(&cboxes[i], &tr, &th)) continue;
        if (x >= tr.left && x < tr.right && y >= tr.top - slop && y < tr.bottom + slop) {
            *thumb = x >= th.left && x < th.right;
            return i;
        }
    }
    return -1;
}

/* an outlined square, for the Copy icon */
static void icon_page(int x, int y, int n, int t, COLORREF line, COLORREF fill)
{
    RECT r;
    r.left = x; r.top = y; r.right = x + n; r.bottom = y + n;
    SetBkMode(backdc, OPAQUE);
    SetBkColor(backdc, line);
    ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
    r.left += t; r.top += t; r.right -= t; r.bottom -= t;
    SetBkColor(backdc, fill);
    ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
    SetBkMode(backdc, TRANSPARENT);
}

static void paint(HDC dc, const RECT *clip)
{
    RECT all;
    int i, lastfont = -1, clipbox = 0;
    SelPos sa, sb2;
    int have_sel = sel_norm(&sa, &sb2);

    all.left = 0; all.top = 0; all.right = view_w; all.bottom = view_h;
    SetBkColor(backdc, C_PAGE);
    ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &all, NULL, 0, NULL);
    SetBkMode(backdc, TRANSPARENT);

    for (i = 0; i < nitems; i++) {
        Item *it = &items[i];
        int top = it->y - g_scroll;
        if (top > view_h || top + it->h < 0) continue;
        if (it->box != clipbox) {                           /* code block text is clipped to its box */
            clipbox = it->box;
            SelectClipRgn(backdc, NULL);
            if (clipbox) {
                const CodeBox *cb = &cboxes[clipbox - 1];
                int cy = cb->y - g_scroll;
                IntersectClipRect(backdc, cb->x + 1, cy + 1, cb->x + cb->w - 1, cy + cb->h - 1);
            }
        }
        switch (it->kind) {
        case IT_RECT: {
            RECT r;
            r.left = it->x; r.top = top; r.right = it->x + it->w; r.bottom = top + it->h;
            SetBkMode(backdc, OPAQUE);
            SetBkColor(backdc, it->col);
            ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
            SetBkMode(backdc, TRANSPARENT);
            break;
        }
        case IT_TEXT:
            if (it->font != lastfont) {
                SelectObject(backdc, fonts[it->font].h);
                lastfont = it->font;
            }
            {
                int s0, s1;
                if (have_sel && sel_range(i, &sa, &sb2, &s0, &s1)) {
                    int x0 = it->x + prefix_w(backdc, it, s0), x1 = it->x + prefix_w(backdc, it, s1);
                    RECT r;
                    if (s0 == 0 && i > sa.item) {         /* fill the gap after the previous selected word */
                        int j = i - 1;
                        while (j >= 0 && items[j].kind != IT_TEXT) j--;
                        if (j >= sa.item && abs(items[j].y - it->y) <= it->h / 2 && items[j].x + items[j].w < it->x)
                            x0 = items[j].x + items[j].w;
                    }
                    r.left = x0; r.top = top; r.right = x1; r.bottom = top + it->h;
                    SetBkMode(backdc, OPAQUE);
                    SetBkColor(backdc, GetSysColor(COLOR_HIGHLIGHT));
                    ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
                    SetBkMode(backdc, TRANSPARENT);
                    SetTextColor(backdc, it->col);
                    draw_run(backdc, it->x, top, it->text, s0, it->font);
                    draw_run(backdc, x1, top, it->text + s1, it->len - s1, it->font);
                    SetTextColor(backdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
                    draw_run(backdc, x0, top, it->text + s0, s1 - s0, it->font);
                } else {
                    SetTextColor(backdc, it->col);
                    draw_run(backdc, it->x, top, it->text, it->len, it->font);
                }
            }
            break;
        case IT_DISC:
        case IT_RING: {
            HBRUSH br = it->kind == IT_DISC ? CreateSolidBrush(it->col) : (HBRUSH)GetStockObject(NULL_BRUSH);
            HPEN pen = CreatePen(PS_SOLID, 1, it->col);
            HGDIOBJ ob = SelectObject(backdc, br), op = SelectObject(backdc, pen);
            Ellipse(backdc, it->x, top, it->x + it->w, top + it->h);
            SelectObject(backdc, ob);
            SelectObject(backdc, op);
            DeleteObject(pen);
            if (it->kind == IT_DISC) DeleteObject(br);
            lastfont = -1;
            break;
        }
        }
    }
    SelectClipRgn(backdc, NULL);
    for (i = 0; i < ncboxes; i++) {                     /* sideways scrollbars of long code blocks */
        RECT tr, th;
        int top = cboxes[i].y - g_scroll;
        if (top > view_h || top + cboxes[i].h < 0 || !sbar_geom(&cboxes[i], &tr, &th)) continue;
        tr.top -= g_scroll; tr.bottom -= g_scroll; th.top -= g_scroll; th.bottom -= g_scroll;
        SetBkMode(backdc, OPAQUE);
        SetBkColor(backdc, C_INLINEBG);
        ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &tr, NULL, 0, NULL);
        SetBkColor(backdc, (i == hot_box || i == sb_drag) ? C_GRAY : C_BLOCKBD);
        ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &th, NULL, 0, NULL);
        SetBkMode(backdc, TRANSPARENT);
    }
    if (find_visible) {
        RECT fr = find_rc;
        SetBkMode(backdc, OPAQUE);
        SetBkColor(backdc, C_BLOCKBD);
        ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &fr, NULL, 0, NULL);
        fr.left++; fr.top++; fr.right--; fr.bottom--;
        SetBkColor(backdc, C_BLOCKBG);
        ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &fr, NULL, 0, NULL);
        SetBkMode(backdc, TRANSPARENT);
    }
    for (i = 0; i < ncboxes; i++) {                     /* Copy buttons, shown while a code block is hovered */
        CodeBox *cb = &cboxes[i];
        int top = cb->by - g_scroll, hot = (i == hot_box && btn_hot);
        int done = (i == copied_box);
        RECT r;
        if ((i != hot_box && i != copied_box) || top > view_h || top + cb->bh < 0) continue;
        r.left = cb->bx; r.top = top; r.right = cb->bx + cb->bw; r.bottom = top + cb->bh;
        SetBkMode(backdc, OPAQUE);
        SetBkColor(backdc, C_BLOCKBD);
        ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
        r.left++; r.top++; r.right--; r.bottom--;
        SetBkColor(backdc, hot ? C_INLINEBG : C_PAGE);
        ExtTextOut(backdc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
        SetBkMode(backdc, TRANSPARENT);
        {
            COLORREF ic = done ? pal[P_COM] : (hot ? C_TEXT : C_GRAY), bg = hot ? C_INLINEBG : C_PAGE;
            int sz = cb->bw * 13 / 24, ix = cb->bx + (cb->bw - sz) / 2, iy = top + (cb->bh - sz) / 2;
            if (done) {                                 /* a check mark */
                HPEN pen = CreatePen(PS_SOLID, S(2) > 1 ? S(2) : 1, ic);
                HGDIOBJ op = SelectObject(backdc, pen);
                MoveToEx(backdc, ix, iy + sz / 2, NULL);
                LineTo(backdc, ix + sz * 2 / 5, iy + sz - 1);
                LineTo(backdc, ix + sz, iy + 1);
                SelectObject(backdc, op);
                DeleteObject(pen);
            } else {                                    /* two overlapping pages */
                int pg = sz * 3 / 4, t = S(1) > 1 ? S(1) : 1;
                icon_page(ix, iy, pg, t, ic, bg);
                icon_page(ix + sz - pg, iy + sz - pg, pg, t, ic, bg);
            }
        }
    }
    if (as_on) {                                        /* the middle-click scroll marker */
        int r = MulDiv(13, g_dpi, 96), t = r / 4 + 1;
        HPEN pen = CreatePen(PS_SOLID, 1, C_GRAY);
        HBRUSH page = CreateSolidBrush(C_PAGE), ink = CreateSolidBrush(C_GRAY);
        HGDIOBJ op = SelectObject(backdc, pen), ob = SelectObject(backdc, page);
        POINT tri[3];
        Ellipse(backdc, as_x - r, as_y - r, as_x + r + 1, as_y + r + 1);
        SelectObject(backdc, ink);
        tri[0].x = as_x; tri[0].y = as_y - r + 3; tri[1].x = as_x - t; tri[1].y = as_y - r + 3 + t; tri[2].x = as_x + t; tri[2].y = tri[1].y;
        Polygon(backdc, tri, 3);
        tri[0].y = as_y + r - 3; tri[1].y = as_y + r - 3 - t; tri[2].y = tri[1].y;
        Polygon(backdc, tri, 3);
        if (as_hbox >= 0) {
            tri[0].y = as_y; tri[0].x = as_x - r + 3; tri[1].x = as_x - r + 3 + t; tri[1].y = as_y - t; tri[2].x = tri[1].x; tri[2].y = as_y + t;
            Polygon(backdc, tri, 3);
            tri[0].x = as_x + r - 3; tri[1].x = as_x + r - 3 - t; tri[2].x = tri[1].x;
            Polygon(backdc, tri, 3);
        }
        SelectObject(backdc, ob);
        SelectObject(backdc, op);
        DeleteObject(pen); DeleteObject(page); DeleteObject(ink);
    }
    BitBlt(dc, clip->left, clip->top, clip->right - clip->left, clip->bottom - clip->top,
           backdc, clip->left, clip->top, SRCCOPY);
}

static int swallow_up;
static int sel_dragging, sel_moved, click_count, down_x, down_y, last_click_x, last_click_y;
static DWORD last_click_t;

static Item *link_at(int x, int y)
{
    int i;
    y += g_scroll;
    for (i = 0; i < nitems; i++) {
        Item *it = &items[i];
        if (it->kind == IT_TEXT && it->link && x >= it->x && x < it->x + it->w &&
            y >= it->y && y < it->y + it->h)
            return it;
    }
    return NULL;
}

static Item *text_at(int x, int y)
{
    int i;
    y += g_scroll;
    for (i = 0; i < nitems; i++) {
        Item *it = &items[i];
        if (it->kind == IT_TEXT && x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h)
            return it;
    }
    return NULL;
}

static void load_file(const char *path);

/* ---- #anchor links: GitHub-style heading slugs ---- */

static void heading_slug(const Block *b, char *out, int cap)
{
    int i, n = 0;
    const char *t;
    for (i = 0; i < b->inl.n; i++)
        for (t = b->inl.s[i].text; *t && n < cap - 1; t++) {
            unsigned char c = (unsigned char)*t;
            if (c < 128 && isalnum(c)) out[n++] = (char)tolower(c);
            else if (c == ' ') out[n++] = '-';
            else if (c == '-' || c == '_') out[n++] = (char)c;
        }
    out[n] = 0;
}

static int goto_anchor(const char *frag)
{
    char want[160], slug[160], other[160];
    int i, j, n = 0;
    for (; *frag && n < 150; frag++) {
        if (frag[0] == '%' && isxdigit((unsigned char)frag[1]) && isxdigit((unsigned char)frag[2])) {
            int v;
            sscanf(frag + 1, "%2x", &v);
            if (v < 128) want[n++] = (char)tolower(v);
            frag += 2;
        } else want[n++] = (char)tolower((unsigned char)*frag);
    }
    want[n] = 0;
    for (i = 0; i < nblocks; i++) {
        int dup = 0;
        if (blocks[i].kind != BK_HEAD) continue;
        heading_slug(&blocks[i], slug, sizeof(slug) - 8);
        for (j = 0; j < i; j++) {
            if (blocks[j].kind != BK_HEAD) continue;
            heading_slug(&blocks[j], other, sizeof(other));
            if (!strcmp(slug, other)) dup++;
        }
        if (dup) sprintf(slug + strlen(slug), "-%d", dup);
        if (!strcmp(slug, want)) { jump_to(blocks[i].y); return 1; }
    }
    return 0;
}

static int has_ph(const char *s)
{
    if (!nph) return 0;
    for (; *s; s++)
        if ((unsigned char)*s < 32 && ph_cp[(unsigned char)*s]) return 1;
    return 0;
}

/* the text as UTF-16, with the Unicode characters put back */
static wchar_t *widen(const char *s)
{
    wchar_t *w = (wchar_t *)malloc((strlen(s) * 2 + 2) * sizeof(wchar_t));
    int n = 0;
    while (*s) {
        const char *r = s;
        while (*r && !((unsigned char)*r < 32 && ph_cp[(unsigned char)*r])) r++;
        if (r > s) {
            n += MultiByteToWideChar(CP_ACP, 0, s, (int)(r - s), w + n, (int)(r - s));
            s = r;
        }
        if (*s) n += wide_of(ph_cp[(unsigned char)*s++], w + n);
    }
    w[n] = 0;
    return w;
}

static void put_clipboard(const char *s)
{
    HGLOBAL g;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    if (has_ph(s)) {
        wchar_t *w = widen(s);
        g = GlobalAlloc(GMEM_MOVEABLE, (wcslen(w) + 1) * sizeof(wchar_t));
        if (g) {
            memcpy(GlobalLock(g), w, (wcslen(w) + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            SetClipboardData(13 /* CF_UNICODETEXT */, g);
        }
        free(w);
    } else {
        g = GlobalAlloc(GMEM_MOVEABLE, strlen(s) + 1);
        if (g) {
            memcpy(GlobalLock(g), s, strlen(s) + 1);
            GlobalUnlock(g);
            SetClipboardData(CF_TEXT, g);
        }
    }
    CloseClipboard();
}

static void shell_open(const char *target)
{
    char msg[MAX_PATH + 200];
    if ((long)ShellExecute(hwnd, "open", target, NULL, NULL, SW_SHOWNORMAL) > 32) return;
    put_clipboard(target);
    wsprintf(msg, "No program on this computer can open:\n\n%s\n\nThe address was copied to the clipboard.", target);
    MessageBox(hwnd, msg, "mdview", MB_OK | MB_ICONINFORMATION);
}

static void open_link(const char *url)
{
    char path[MAX_PATH], *slash, *hash;
    DWORD attr;

    if (!*url) return;
    if (url[0] == '#') { goto_anchor(url + 1); return; }
    if (!strstr(url, "://") && strncmp(url, "mailto:", 7) != 0) {
        /* a relative link: open Markdown files in this viewer */
        strncpy(path, curfile, MAX_PATH - 1);
        path[MAX_PATH - 1] = 0;
        slash = strrchr(path, '\\');
        if (slash) {
            strncpy(slash + 1, url, MAX_PATH - 1 - (slash + 1 - path));
            path[MAX_PATH - 1] = 0;
            hash = strchr(path, '#');
            if (hash) *hash = 0;
            for (slash = path; *slash; slash++) if (*slash == '/') *slash = '\\';
            attr = GetFileAttributes(path);
            if (attr != 0xFFFFFFFFUL) {
                if (strlen(path) > 3 && (!stricmp(path + strlen(path) - 3, ".md") ||
                                         !stricmp(path + strlen(path) - 4, ".txt") ||
                                         !stricmp(path + strlen(path) - 9, ".markdown")))
                    load_file(path);
                else
                    shell_open(path);
                return;
            }
        }
    }
    shell_open(url);
}

static void copy_text(int whole)
{
    char *t = whole ? NULL : selection_text();
    HGLOBAL g;
    if (!t) t = doc_text();
    if (!OpenClipboard(hwnd)) { free(t); return; }
    EmptyClipboard();
    g = GlobalAlloc(GMEM_MOVEABLE, strlen(t) + 1);
    if (g) {
        memcpy(GlobalLock(g), t, strlen(t) + 1);
        GlobalUnlock(g);
        SetClipboardData(CF_TEXT, g);
    }
    CloseClipboard();
    free(t);
}

static void show_text(char *text, const char *title);

/* view the clipboard's text as a Markdown document */
static void paste_text(void)
{
    HANDLE h;
    char *t;
    if (!OpenClipboard(hwnd)) return;
    h = GetClipboardData(CF_TEXT);
    if (!h) { CloseClipboard(); return; }
    t = (char *)GlobalLock(h);
    if (t) {
        char *copy = (char *)malloc(strlen(t) + 1);
        strcpy(copy, t);
        GlobalUnlock(h);
        CloseClipboard();
        curfile[0] = 0;
        show_text(copy, "Clipboard - mdview " VER_STR);
        free(copy);
        return;
    }
    CloseClipboard();
}

#define SETTINGS_KEY "Software\\mdview"

/* ---- theme and width switches ---- */

static void save_setting(const char *name, int val)
{
    HKEY k;
    DWORD v = (DWORD)val;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueEx(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

static int load_setting(const char *name, int def)
{
    HKEY k;
    DWORD v = 0, n = sizeof(v), type = 0;
    int r = def;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueEx(k, name, NULL, &type, (BYTE *)&v, &n) == ERROR_SUCCESS && type == REG_DWORD)
            r = (int)v;
        RegCloseKey(k);
    }
    return r;
}

static HBRUSH br_panel, br_edit;

static int system_uses_dark(void)
{
    HKEY k;
    DWORD v = 1, n = sizeof(v), type = 0;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueEx(k, "AppsUseLightTheme", NULL, &type, (BYTE *)&v, &n) != ERROR_SUCCESS || type != REG_DWORD)
            v = 1;
        RegCloseKey(k);
    }
    return v == 0;
}

static void apply_palette(void)
{
    g_dark = g_theme == 2 || (g_theme == 0 && system_uses_dark());
    memcpy(pal, g_dark ? pal_dark : pal_light, sizeof(pal));
    if (br_panel) DeleteObject(br_panel);
    if (br_edit) DeleteObject(br_edit);
    br_panel = CreateSolidBrush(C_BLOCKBG);
    br_edit = CreateSolidBrush(C_PAGE);
}

static DWORD win_build(void)
{
    typedef struct { DWORD size, major, minor, build, platform; WCHAR csd[128]; } OsVer;
    typedef LONG (WINAPI *RtlVer)(OsVer *);
    HMODULE nt = GetModuleHandle("ntdll.dll");
    RtlVer f = nt ? (RtlVer)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OsVer v;
    if (!f) return 0;
    memset(&v, 0, sizeof(v));
    v.size = sizeof(v);
    return f(&v) == 0 ? v.build : 0;
}

/* dark title bar and scroll bar where Windows supports them (silently ignored elsewhere) */
static void apply_window_theme(void)
{
    typedef HRESULT (WINAPI *DwmSetFn)(HWND, DWORD, LPCVOID, DWORD);
    typedef HRESULT (WINAPI *ThemeFn)(HWND, LPCWSTR, LPCWSTR);
    HMODULE m;
    BOOL on = g_dark;

    m = LoadLibrary("dwmapi.dll");
    if (m) {
        DwmSetFn f = (DwmSetFn)GetProcAddress(m, "DwmSetWindowAttribute");
        if (f && f(hwnd, 20, &on, sizeof(on)) != 0) f(hwnd, 19, &on, sizeof(on));
        FreeLibrary(m);
    }
    m = LoadLibrary("uxtheme.dll");
    if (m) {
        ThemeFn f = (ThemeFn)GetProcAddress(m, "SetWindowTheme");
        if (f) f(hwnd, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
        if (win_build() >= 17763) {             /* undocumented: 135 SetPreferredAppMode, 136 FlushMenuThemes */
            typedef int (WINAPI *ModeFn)(int);
            typedef void (WINAPI *FlushFn)(void);
            ModeFn sm = (ModeFn)GetProcAddress(m, MAKEINTRESOURCE(135));
            FlushFn fl = (FlushFn)GetProcAddress(m, MAKEINTRESOURCE(136));
            if (sm) sm(g_dark ? 2 : 0);
            if (fl) fl();
        }
        FreeLibrary(m);
    }
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    DrawMenuBar(hwnd);
}

static void update_view_menu(void)
{
    CheckMenuRadioItem(view_menu, ID_THEME_AUTO, ID_THEME_DARK, ID_THEME_AUTO + g_theme, MF_BYCOMMAND);
    CheckMenuItem(view_menu, ID_WIDTH, MF_BYCOMMAND | (g_limit_width ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(view_menu, ID_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
}

static void set_theme(int t)
{
    g_theme = t;
    save_setting("Theme", t);
    apply_palette();
    apply_window_theme();
    update_view_menu();
    relayout();                                 /* colours are baked into the laid-out items */
    InvalidateRect(hwnd, NULL, FALSE);
}

static void toggle_width(void)
{
    int old_doc = doc_h > 0 ? doc_h : 1, old_scroll = g_scroll;
    g_limit_width = !g_limit_width;
    save_setting("LimitWidth", g_limit_width);
    update_view_menu();
    layout_all(view_w);
    g_scroll = (int)((double)old_scroll * doc_h / old_doc);
    set_scroll(g_scroll);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* ---- hover: Copy buttons on code blocks, link tooltips ---- */

typedef struct { UINT cbSize, uFlags; HWND hwnd; UINT uId; RECT rect; HINSTANCE hinst; LPSTR lpszText; } MdToolInfo;
#define MD_TTM_ACTIVATE       (WM_USER + 1)
#define MD_TTM_ADDTOOL        (WM_USER + 4)
#define MD_TTM_RELAYEVENT     (WM_USER + 7)
#define MD_TTM_UPDATETIPTEXT  (WM_USER + 12)
#define MD_TTM_SETMAXTIPWIDTH (WM_USER + 24)

static HWND tip;
static int tip_link = -1;              /* link index the tooltip is armed for, 0 = none, -1 = needs re-arming */
static char tip_buf[1100];

static void tip_init(HINSTANCE inst)
{
    HMODULE cc = LoadLibrary("comctl32.dll");
    FARPROC icc = cc ? GetProcAddress(cc, "InitCommonControls") : NULL;
    MdToolInfo ti;
    if (!icc) return;
    ((void (WINAPI *)(void))icc)();
    tip = CreateWindowEx(WS_EX_TOPMOST, "tooltips_class32", NULL, WS_POPUP | 0x03,
                         0, 0, 0, 0, hwnd, NULL, inst, NULL);
    if (!tip) return;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = 40;
    ti.hwnd = hwnd;
    ti.uId = 1;
    ti.rect.right = ti.rect.bottom = 32000;
    ti.lpszText = tip_buf;
    SendMessage(tip, MD_TTM_ADDTOOL, 0, (LPARAM)&ti);
    SendMessage(tip, MD_TTM_SETMAXTIPWIDTH, 0, MulDiv(520, g_dpi, 96));
    SendMessage(tip, MD_TTM_ACTIVATE, FALSE, 0);
    tip_link = 0;
}

static void tip_show_for(int link)
{
    if (!tip || link == tip_link) return;
    SendMessage(tip, MD_TTM_ACTIVATE, FALSE, 0);        /* hides a visible tip and resets the delay */
    tip_link = link;
    if (link > 0 && link <= nlinks) {
        MdToolInfo ti;
        strncpy(tip_buf, links[link - 1], sizeof(tip_buf) - 1);
        tip_buf[sizeof(tip_buf) - 1] = 0;
        memset(&ti, 0, sizeof(ti));
        ti.cbSize = 40;
        ti.hwnd = hwnd;
        ti.uId = 1;
        ti.lpszText = tip_buf;
        SendMessage(tip, MD_TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);
        SendMessage(tip, MD_TTM_ACTIVATE, TRUE, 0);
    } else {
        tip_link = 0;
    }
}

static void tip_reset(void)
{
    if (!tip || tip_link == 0) return;
    SendMessage(tip, MD_TTM_ACTIVATE, FALSE, 0);
    tip_link = -1;
}

static void tip_relay(UINT msg, WPARAM wp, LPARAM lp)
{
    MSG m;
    if (!tip) return;
    m.hwnd = hwnd; m.message = msg; m.wParam = wp; m.lParam = lp;
    m.time = GetMessageTime();
    m.pt.x = (short)LOWORD(lp); m.pt.y = (short)HIWORD(lp);
    SendMessage(tip, MD_TTM_RELAYEVENT, 0, (LPARAM)&m);
}

static int box_at(int x, int y)
{
    int i;
    y += g_scroll;
    for (i = 0; i < ncboxes; i++)
        if (x >= cboxes[i].x && x < cboxes[i].x + cboxes[i].w && y >= cboxes[i].y && y < cboxes[i].y + cboxes[i].h)
            return i;
    return -1;
}

static int in_btn(int i, int x, int y)
{
    y += g_scroll;
    return x >= cboxes[i].bx && x < cboxes[i].bx + cboxes[i].bw && y >= cboxes[i].by && y < cboxes[i].by + cboxes[i].bh;
}

static void update_hover(void)
{
    POINT pt;
    int box = -1, bh = 0, lk = 0;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    if (pt.x >= 0 && pt.y >= 0 && pt.x < view_w && pt.y < view_h) {
        Item *it;
        box = box_at(pt.x, pt.y);
        if (box >= 0) bh = in_btn(box, pt.x, pt.y);
        it = link_at(pt.x, pt.y);
        if (it) lk = it->link;
    }
    if (box != hot_box || bh != btn_hot) {
        hot_box = box;
        btn_hot = bh;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    tip_show_for(lk);
}

/* ---- middle-click autoscroll (Windows 98+/NT: older IntelliPoint drivers do their own) ---- */

static void as_stop(void)
{
    if (!as_on) return;
    as_on = 0;
    KillTimer(hwnd, 6);
    if (GetCapture() == hwnd) ReleaseCapture();
    InvalidateRect(hwnd, NULL, FALSE);
}

static void as_start(int x, int y)
{
    int bx = box_at(x, y);
    as_on = 1;
    as_x = x; as_y = y;
    as_ax = as_ay = 0;
    as_hbox = bx >= 0 && cboxes[bx].hmax ? bx : -1;
    as_t = as_t0 = GetTickCount();
    SetCapture(hwnd);
    SetTimer(hwnd, 6, 15, NULL);
    InvalidateRect(hwnd, NULL, FALSE);
}

static int as_dead(void) { return MulDiv(8, g_dpi, 96); }

static void as_step(void)
{
    POINT pt;
    DWORD now = GetTickCount();
    int dt = (int)(now - as_t), dy, dx, dead = as_dead(), n;
    as_t = now;
    if (dt > 100) dt = 100;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    dy = pt.y - as_y;
    dx = pt.x - as_x;
    dy = abs(dy) > dead ? (dy < 0 ? dy + dead : dy - dead) : 0;
    dx = abs(dx) > dead ? (dx < 0 ? dx + dead : dx - dead) : 0;
    as_ay += dy * 8 * dt;
    n = as_ay / 1000;
    as_ay -= n * 1000;
    if (n) set_scroll(g_scroll + n);
    if (as_hbox >= 0) {
        as_ax += dx * 8 * dt;
        n = as_ax / 1000;
        as_ax -= n * 1000;
        if (n) box_hscroll(as_hbox, cboxes[as_hbox].hs + n);
    }
}

/* shift+wheel or a tilt wheel over a code block scrolls it sideways */
static int hwheel(LPARAM lp, int amount)
{
    POINT pt;
    int bx;
    pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
    ScreenToClient(hwnd, &pt);
    bx = box_at(pt.x, pt.y);
    if (bx < 0 || !cboxes[bx].hmax) return 0;
    box_hscroll(bx, cboxes[bx].hs + amount);
    return 1;
}

static void sb_move(int x)
{
    CodeBox *cb = &cboxes[sb_drag];
    RECT tr, th;
    int tw, thw;
    if (!sbar_geom(cb, &tr, &th)) return;
    tw = tr.right - tr.left;
    thw = th.right - th.left;
    if (tw > thw) box_hscroll(sb_drag, (x - sb_grab - tr.left) * cb->hmax / (tw - thw));
}

static int mouse_tracking;

static void track_leave(void)
{
    typedef struct { DWORD cbSize, dwFlags; HWND hwndTrack; DWORD dwHoverTime; } Tme;
    FARPROC f;
    Tme t;
    if (mouse_tracking) return;
    f = GetProcAddress(GetModuleHandle("user32.dll"), "TrackMouseEvent");
    if (!f) return;
    t.cbSize = sizeof(t); t.dwFlags = 2; t.hwndTrack = hwnd; t.dwHoverTime = 0;
    if (((BOOL (WINAPI *)(Tme *))f)(&t)) mouse_tracking = 1;
}

static void copy_code(int box)
{
    const Block *b = &blocks[cboxes[box].blk];
    int k;
    sb = NULL; sb_len = sb_cap = 0;
    sb_addn("", 0);
    for (k = 0; k < b->nlines; k++) {
        sb_addn(b->lines[k], (int)strlen(b->lines[k]));
        if (k + 1 < b->nlines) sb_addn("\r\n", 2);
    }
    put_clipboard(sb);
    free(sb);
    sb = NULL;
    copied_box = box;
    SetTimer(hwnd, 4, 1500, NULL);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* ---- right-click menu ---- */

/* what "copy address" puts on the clipboard, and what to call it */
static void link_payload(const char *url, char *buf, int cap, const char **label)
{
    const char *p = url;
    int n;
    *label = "Copy &link address";
    if (!strnicmp(url, "mailto:", 7)) {
        p = url + 7;
        *label = "Copy &email address";
        n = (int)strcspn(p, "?");
    } else if (!strnicmp(url, "tel:", 4)) {
        p = url + 4;
        *label = "Copy &phone number";
        n = (int)strlen(p);
    } else {
        n = (int)strlen(p);
    }
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, p, n);
    buf[n] = 0;
}

static void context_menu(HWND h, LPARAM lp)
{
    POINT sp, cp;
    HMENU m = CreatePopupMenu();
    Item *it;
    int box, cmd, link = 0;
    SelPos a, b;
    const char *label = "";
    char payload[1100];

    sp.x = (short)LOWORD(lp); sp.y = (short)HIWORD(lp);
    if (sp.x == -1 && sp.y == -1) {             /* from the keyboard */
        cp.x = S(40); cp.y = S(40);
        sp = cp;
        ClientToScreen(h, &sp);
    } else {
        cp = sp;
        ScreenToClient(h, &cp);
    }
    it = link_at(cp.x, cp.y);
    if (it && it->link > 0 && it->link <= nlinks) link = it->link;
    box = box_at(cp.x, cp.y);

    if (link) {
        const char *url = links[link - 1];
        link_payload(url, payload, sizeof(payload), &label);
        AppendMenu(m, MF_STRING, ID_CTX_OPEN, !strnicmp(url, "mailto:", 7) ? "Send &email" : "&Open link");
        AppendMenu(m, MF_STRING, ID_CTX_COPYLINK, label);
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
    }
    if (box >= 0) {
        AppendMenu(m, MF_STRING, ID_CTX_COPYCODE, "Copy code &block");
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenu(m, MF_STRING | (sel_norm(&a, &b) ? 0 : MF_GRAYED), ID_COPY, "&Copy");
    AppendMenu(m, MF_STRING, ID_SELALL, "Select &all");
    AppendMenu(m, MF_STRING, ID_FIND, "&Find...");
    tip_reset();
    cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, sp.x, sp.y, 0, h, NULL);
    DestroyMenu(m);
    if (cmd == ID_CTX_OPEN) open_link(links[link - 1]);
    else if (cmd == ID_CTX_COPYLINK) put_clipboard(payload);
    else if (cmd == ID_CTX_COPYCODE) copy_code(box);
    else if (cmd) SendMessage(h, WM_COMMAND, cmd, 0);
    update_hover();
}

/* ---- find (Ctrl+F) ---- */

static HWND find_edit, find_count, find_prev, find_next, find_close;
static WNDPROC find_edit_old;
static int find_cur = -1;
static char *ftext;
static int *fitem, *foff, fcount;

static void find_build(void)
{
    int i, total = 0, k = 0, n = 0;
    free(ftext); free(fitem); free(foff);
    for (i = 0; i < nitems; i++) if (items[i].kind == IT_TEXT && items[i].len) { total += items[i].len + 1; k++; }
    ftext = (char *)malloc(total + 1);
    fitem = (int *)malloc((k + 1) * sizeof(int));
    foff = (int *)malloc((k + 1) * sizeof(int));
    fcount = 0;
    for (i = 0; i < nitems; i++) {
        int c;
        if (items[i].kind != IT_TEXT || !items[i].len) continue;
        fitem[fcount] = i;
        foff[fcount++] = n;
        for (c = 0; c < items[i].len; c++) ftext[n++] = (char)tolower((unsigned char)items[i].text[c]);
        ftext[n++] = ' ';
    }
    ftext[n] = 0;
    find_valid = 1;
}

static SelPos find_locate(int pos, int end)
{
    SelPos r;
    int lo = 0, hi = fcount - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (foff[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    r.item = fitem[lo];
    r.off = pos - foff[lo];
    if (r.off > items[r.item].len) r.off = items[r.item].len;
    if (end) r.off++;
    return r;
}

/* dir: 1 next, -1 previous; stay: re-find at the current match (typing in the box) */
static void find_do(int dir, int stay)
{
    char q[256], status[48];
    int ql, pos = -1, first = -1, last = -1, before = -1, after = -1, atafter = -1, total = 0, idx = 0;
    const char *f, *hit;

    GetWindowText(find_edit, q, sizeof(q));
    for (ql = 0; q[ql]; ql++) q[ql] = (char)tolower((unsigned char)q[ql]);
    if (!ql) {
        sel_a.item = sel_b.item = -1;
        SetWindowText(find_count, "");
        find_cur = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    if (!find_valid) { find_build(); find_cur = -1; }
    if (!fcount) { SetWindowText(find_count, "No matches"); return; }
    f = ftext;
    for (hit = strstr(f, q); hit; hit = strstr(hit + 1, q)) {
        int at = (int)(hit - f);
        total++;
        if (first < 0) first = at;
        last = at;
        if (find_cur >= 0 && at < find_cur) before = at;
        if (find_cur >= 0 && at > find_cur && after < 0) after = at;
        if (find_cur >= 0 && at >= find_cur && atafter < 0) atafter = at;
    }
    if (!total) {
        SetWindowText(find_count, "No matches");
        sel_a.item = sel_b.item = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    if (find_cur < 0) pos = dir > 0 ? first : last;
    else if (stay) pos = atafter >= 0 ? atafter : first;
    else if (dir > 0) pos = after >= 0 ? after : first;
    else pos = before >= 0 ? before : last;
    find_cur = pos;
    for (hit = strstr(f, q); hit && (int)(hit - f) <= pos; hit = strstr(hit + 1, q)) idx++;
    sprintf(status, "%d of %d", idx, total);
    SetWindowText(find_count, status);
    sel_a = find_locate(pos, 0);
    sel_b = find_locate(pos + ql - 1, 1);
    {
        int y = items[sel_a.item].y;
        if (y < g_scroll + S(50) || y > g_scroll + view_h - S(50)) set_scroll(y - view_h / 3);
    }
    if (items[sel_a.item].box) {
        const Item *fi = &items[sel_a.item];
        const CodeBox *cb = &cboxes[fi->box - 1];
        int mx = fi->x + text_w(fi->font, fi->text, sel_a.off);
        if (mx < cb->x + S(10) || mx > cb->x + cb->w - S(10))
            box_hscroll(fi->box - 1, cb->hs + mx - (cb->x + cb->w / 3));
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static void find_hide(void)
{
    if (!find_visible) return;
    ShowWindow(find_edit, SW_HIDE); ShowWindow(find_count, SW_HIDE); ShowWindow(find_prev, SW_HIDE);
    ShowWindow(find_next, SW_HIDE); ShowWindow(find_close, SW_HIDE);
    find_visible = 0;
    SetFocus(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void find_layout(void)
{
    int ew = MulDiv(190, g_dpi, 96), cw = MulDiv(74, g_dpi, 96), bw = MulDiv(26, g_dpi, 96);
    int h = MulDiv(22, g_dpi, 96), pad = MulDiv(6, g_dpi, 96), x, y = MulDiv(8, g_dpi, 96);
    int total = ew + cw + 3 * bw + 6 * pad;
    if (!find_edit) return;
    x = view_w - total - MulDiv(14, g_dpi, 96);
    if (x < 0) x = 0;
    find_rc.left = x - pad; find_rc.top = y - pad; find_rc.right = x + total - pad; find_rc.bottom = y + h + pad;
    x += 0;
    SetWindowPos(find_edit, NULL, x, y, ew, h, SWP_NOZORDER); x += ew + pad;
    SetWindowPos(find_count, NULL, x, y + 3, cw, h - 3, SWP_NOZORDER); x += cw + pad;
    SetWindowPos(find_prev, NULL, x, y, bw, h, SWP_NOZORDER); x += bw + pad;
    SetWindowPos(find_next, NULL, x, y, bw, h, SWP_NOZORDER); x += bw + pad;
    SetWindowPos(find_close, NULL, x, y, bw, h, SWP_NOZORDER);
}

static LRESULT CALLBACK find_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { find_hide(); return 0; }
        if (w == VK_RETURN || w == VK_F3) { find_do(GetKeyState(VK_SHIFT) < 0 ? -1 : 1, 0); return 0; }
        if (w == 'A' && GetKeyState(VK_CONTROL) < 0) { SendMessage(h, EM_SETSEL, 0, -1); return 0; }   /* older edit controls lack Ctrl+A */
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1)) return 0;
    return CallWindowProc(find_edit_old, h, m, w, l);
}

static void find_show(void)
{
    HFONT gui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!find_edit) {
        HINSTANCE inst = (HINSTANCE)GetWindowLong(hwnd, GWL_HINSTANCE);
        find_edit = CreateWindowEx(0, "EDIT", "", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                   hwnd, (HMENU)ID_FINDEDIT, inst, NULL);
        find_count = CreateWindowEx(0, "STATIC", "", WS_CHILD | SS_RIGHT, 0, 0, 10, 10, hwnd, NULL, inst, NULL);
        find_prev = CreateWindowEx(0, "BUTTON", "<", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                                   hwnd, (HMENU)ID_FINDPREV, inst, NULL);
        find_next = CreateWindowEx(0, "BUTTON", ">", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                                   hwnd, (HMENU)ID_FINDNEXT, inst, NULL);
        find_close = CreateWindowEx(0, "BUTTON", "x", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                                    hwnd, (HMENU)ID_FINDCLOSE, inst, NULL);
        SendMessage(find_edit, WM_SETFONT, (WPARAM)gui, 0);
        SendMessage(find_count, WM_SETFONT, (WPARAM)gui, 0);
        SendMessage(find_prev, WM_SETFONT, (WPARAM)gui, 0);
        SendMessage(find_next, WM_SETFONT, (WPARAM)gui, 0);
        SendMessage(find_close, WM_SETFONT, (WPARAM)gui, 0);
        find_edit_old = (WNDPROC)SetWindowLong(find_edit, GWL_WNDPROC, (LONG)find_edit_proc);
    }
    find_layout();
    find_visible = 1;
    ShowWindow(find_edit, SW_SHOW); ShowWindow(find_count, SW_SHOW); ShowWindow(find_prev, SW_SHOW);
    ShowWindow(find_next, SW_SHOW); ShowWindow(find_close, SW_SHOW);
    SetFocus(find_edit);
    SendMessage(find_edit, EM_SETSEL, 0, -1);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* ---- the window's size, position and maximized state are kept between runs ---- */

static void save_window(void)
{
    WINDOWPLACEMENT wp;
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;
    save_setting("WinL", wp.rcNormalPosition.left);
    save_setting("WinT", wp.rcNormalPosition.top);
    save_setting("WinW", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
    save_setting("WinH", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
    save_setting("WinMax", wp.showCmd == SW_SHOWMAXIMIZED);
}

static void restore_window(int show)
{
    WINDOWPLACEMENT wp;
    int l = load_setting("WinL", 0), t = load_setting("WinT", 0);
    int w = load_setting("WinW", 0), h = load_setting("WinH", 0);
    int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
    int ox, oy;

    if (!vw || !vh) { vx = vy = 0; vw = GetSystemMetrics(SM_CXSCREEN); vh = GetSystemMetrics(SM_CYSCREEN); }
    ox = (l + w < vx + vw ? l + w : vx + vw) - (l > vx ? l : vx);       /* how much of it is on screen */
    oy = (t + h < vy + vh ? t + h : vy + vh) - (t > vy ? t : vy);
    if (w < 200 || h < 150 || ox < 120 || oy < 80 || t < vy - 20 || t > vy + vh - 40) {
        ShowWindow(hwnd, show);                                         /* nothing saved, or it would be off-screen */
        return;
    }
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    wp.rcNormalPosition.left = l;
    wp.rcNormalPosition.top = t;
    wp.rcNormalPosition.right = l + w;
    wp.rcNormalPosition.bottom = t + h;
    wp.showCmd = (show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT) && load_setting("WinMax", 0) ? SW_SHOWMAXIMIZED : show;
    SetWindowPlacement(hwnd, &wp);
}

static void save_zoom(void) { save_setting("Zoom", g_zoom); }

static void load_zoom(void)
{
    int v = load_setting("Zoom", 0);
    if (v >= 40 && v <= 400) g_zoom = v;
}

/* zoom requests are applied once input pauses, so a run of wheel notches costs one relayout */
static int pending_zoom;

static int zoom_now(void) { return pending_zoom ? pending_zoom : g_zoom; }

static void apply_zoom(void)
{
    int z = pending_zoom, old_doc = doc_h > 0 ? doc_h : 1, old_scroll = g_scroll;
    pending_zoom = 0;
    if (!z || z == g_zoom) return;
    g_zoom = z;
    save_zoom();
    fonts_clear();
    layout_all(view_w);
    g_scroll = (int)((double)old_scroll * doc_h / old_doc);
    set_scroll(g_scroll);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void set_zoom(int z)
{
    if (z < 40) z = 40;
    if (z > 400) z = 400;
    if (z == zoom_now()) return;
    pending_zoom = z;
    if (g_layout_ms < 40) apply_zoom();         /* cheap layout: zoom right away */
    else SetTimer(hwnd, 3, 80, NULL);           /* slow layout: let a run of notches coalesce */
}

static const char *welcome =
    "# mdview\n\n"
    "A Markdown viewer for Windows 95 through Windows 11.\n\n"
    "Open a file with **File > Open** (Ctrl+O) or drag one onto this window.\n\n"
    "- F5 reloads the current file\n"
    "- Ctrl + mouse wheel (or Ctrl + and Ctrl -) zooms\n"
    "- Links are clickable, and relative links to other Markdown files open here\n"
    "- Drag to select text; double-click selects a word, triple-click a paragraph\n"
    "- Ctrl+C copies the selection (or the whole document if nothing is selected)\n"
    "- Ctrl+A selects all; Ctrl+V shows the clipboard as Markdown\n";

static void show_text(char *text, const char *title)
{
    free_doc();
    parse_doc(text);
    g_scroll = 0;
    relayout();
    InvalidateRect(hwnd, NULL, FALSE);
    SetWindowText(hwnd, title);
}

static void load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *raw;
    char *text;
    char title[MAX_PATH + 32];
    const char *base;

    if (!f) {
        MessageBox(hwnd, "Can't open that file.", "mdview", MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 8 * 1024 * 1024) {
        fclose(f);
        MessageBox(hwnd, "File is too big (8 MB max).", "mdview", MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    raw = (unsigned char *)malloc(size + 1);
    size = (long)fread(raw, 1, size, f);
    fclose(f);
    ph_reset();
    text = utf8_to_ansi(raw, size);
    free(raw);

    strncpy(curfile, path, MAX_PATH - 1);
    curfile[MAX_PATH - 1] = 0;
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    sprintf(title, "%s - mdview " VER_STR, base);

    SetCursor(LoadCursor(NULL, IDC_WAIT));
    show_text(text, title);
    free(text);
    SetCursor(cur_arrow);
}

static void open_dialog(void)
{
    OPENFILENAME ofn;
    char path[MAX_PATH] = "";

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = 76;   /* the Win95-era OPENFILENAME size */
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Markdown (*.md;*.markdown;*.txt)\0*.md;*.markdown;*.txt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (GetOpenFileName(&ofn)) load_file(path);
}

/* "mdview /diag": show the last few input events in the title bar (for debugging wheel/Ctrl problems) */
static int g_diag;
static char diag_log[3][80];

static void diag_note(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    char ev[80] = "", title[320];
    if (msg == WM_MOUSEWHEEL)
        wsprintf(ev, "WHEEL d=%d mk=%x ctrl=%d", (int)(short)HIWORD(wp), (unsigned)LOWORD(wp), GetKeyState(VK_CONTROL) < 0);
    else if (msg == WM_VSCROLL)
        wsprintf(ev, "VSCROLL code=%d ctrl=%d", (int)LOWORD(wp), GetKeyState(VK_CONTROL) < 0);
    else if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (lp & 0x40000000))
        return;                                  /* auto-repeat: would flood the log */
    else if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
        wsprintf(ev, "%s vk=%x", (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? "KEYDN" : "KEYUP", (unsigned)wp);
    else if (msg >= 0xC000 && msg <= 0xFFFF)
        wsprintf(ev, "REGMSG %x wp=%x", msg, (unsigned)wp);
    else
        return;
    memmove(diag_log[1], diag_log[0], sizeof(diag_log[0]) * 2);
    strcpy(diag_log[0], ev);
    wsprintf(title, "diag: %s | %s | %s", diag_log[0], diag_log[1], diag_log[2]);
    SetWindowText(h, title);
}

/* ---- dark menu bar (undocumented Windows 10+ owner-draw hooks; only used when the theme is dark) ---- */

typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UahMenu;
typedef struct { DRAWITEMSTRUCT dis; UahMenu um; int iPosition; DWORD metrics[17]; } UahDrawItem;
typedef struct { DWORD cbSize; RECT rcBar; HMENU hMenu; HWND hwndMenu; DWORD flags; } MdMenuBarInfo;

static HBRUSH br_bar, br_barhot;

static BOOL menubar_rect(HWND h, RECT *bar)
{
    typedef BOOL (WINAPI *GetMBI)(HWND, LONG, LONG, MdMenuBarInfo *);
    GetMBI f = (GetMBI)GetProcAddress(GetModuleHandle("user32.dll"), "GetMenuBarInfo");
    MdMenuBarInfo mbi;
    RECT wr;
    if (!f) return FALSE;
    memset(&mbi, 0, sizeof(mbi));
    mbi.cbSize = sizeof(mbi);
    if (!f(h, (LONG)0xFFFFFFFD, 0, &mbi)) return FALSE;          /* OBJID_MENU */
    GetWindowRect(h, &wr);
    *bar = mbi.rcBar;
    OffsetRect(bar, -wr.left, -wr.top);
    return TRUE;
}

static void menubar_brushes(void)
{
    if (!br_bar) br_bar = CreateSolidBrush(RGB(32, 32, 32));
    if (!br_barhot) br_barhot = CreateSolidBrush(RGB(64, 64, 64));
}

/* the 1 pixel light line the system draws under a menu bar */
static void paint_menu_line(HWND h)
{
    RECT bar;
    HDC dc;
    if (!menubar_rect(h, &bar)) return;
    menubar_brushes();
    bar.top = bar.bottom;
    bar.bottom++;
    dc = GetWindowDC(h);
    FillRect(dc, &bar, br_bar);
    ReleaseDC(h, dc);
}

static void draw_menubar_item(UahDrawItem *d)
{
    char buf[64];
    MENUITEMINFO mii;
    NONCLIENTMETRICS ncm;
    HFONT f;
    HGDIOBJ of;
    DWORD flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;

    menubar_brushes();
    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = 0x40;                                   /* MIIM_STRING */
    mii.dwTypeData = buf;
    mii.cch = sizeof(buf) - 1;
    buf[0] = 0;
    GetMenuItemInfo(d->um.hmenu, d->iPosition, TRUE, &mii);
    if (d->dis.itemState & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
    FillRect(d->um.hdc, &d->dis.rcItem, (d->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) ? br_barhot : br_bar);
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    f = CreateFontIndirect(&ncm.lfMenuFont);
    of = SelectObject(d->um.hdc, f);
    SetBkMode(d->um.hdc, TRANSPARENT);
    SetTextColor(d->um.hdc, RGB(235, 235, 235));
    DrawText(d->um.hdc, buf, -1, &d->dis.rcItem, flags);
    SelectObject(d->um.hdc, of);
    DeleteObject(f);
}

static UINT msh_wheel;      /* the wheel message that Win9x IntelliPoint drivers send ("MSWHEEL_ROLLMSG") */

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_diag) diag_note(h, msg, wp, lp);
    if (msh_wheel && msg == msh_wheel) {         /* wp is the wheel delta, 120 per notch */
        int delta = (short)wp;
        if (GetKeyState(VK_CONTROL) < 0) set_zoom(zoom_now() + (delta > 0 ? 10 : -10));
        else smooth_scroll(scroll_base() - delta * S(60) / 120);
        return 0;
    }
    switch (msg) {
    case WM_SIZE:
        view_w = LOWORD(lp);
        view_h = HIWORD(lp);
        if (view_w > 0 && view_h > 0) {
            make_backbuffer();
            find_layout();
            if (shift_layout(view_w)) set_scroll(g_scroll);
            else if (g_layout_ms < 40) relayout();
            else SetTimer(h, 1, 60, NULL);      /* relayout once the dragging pauses */
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wp == 6) { as_step(); return 0; }
        if (wp == 2) {                          /* drag-selecting beyond the top or bottom edge */
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (sel_dragging && sel_moved && (pt.y < 0 || pt.y > view_h)) {
                SelPos p;
                set_scroll(g_scroll + (pt.y < 0 ? -S(30) : S(30)));
                p = pos_at(pt.x, pt.y < 0 ? 0 : view_h - 1);
                if (p.item >= 0) sel_b = p;
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        if (wp == 3) { KillTimer(h, 3); apply_zoom(); return 0; }
        if (wp == 5) { anim_step(); return 0; }
        if (wp == 4) { KillTimer(h, 4); copied_box = -1; InvalidateRect(h, NULL, FALSE); return 0; }
        KillTimer(h, 1);
        relayout();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (backbmp) paint(dc, &ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_VSCROLL: {
        int f = sb_factor(), pos = scroll_base();
        /* Win9x wheel drivers send the wheel as scrollbar line messages: with Ctrl held that means zoom */
        if (GetKeyState(VK_CONTROL) < 0 && (LOWORD(wp) == SB_LINEUP || LOWORD(wp) == SB_LINEDOWN)) {
            set_zoom(zoom_now() + (LOWORD(wp) == SB_LINEUP ? 10 : -10));
            return 0;
        }
        switch (LOWORD(wp)) {
        case SB_LINEUP:        pos -= S(40); break;
        case SB_LINEDOWN:      pos += S(40); break;
        case SB_PAGEUP:        pos -= view_h - S(40); break;
        case SB_PAGEDOWN:      pos += view_h - S(40); break;
        case SB_TOP:           pos = 0; break;
        case SB_BOTTOM:        pos = doc_h; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = (short)HIWORD(wp) * f; break;
        }
        if (LOWORD(wp) == SB_THUMBTRACK || LOWORD(wp) == SB_THUMBPOSITION) set_scroll(pos);
        else smooth_scroll(pos);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = (short)HIWORD(wp);
        if ((LOWORD(wp) & MK_CONTROL) || GetKeyState(VK_CONTROL) < 0) set_zoom(zoom_now() + (delta > 0 ? 10 : -10));
        else if ((LOWORD(wp) & MK_SHIFT) && hwheel(lp, -delta * S(60) / 120)) { }
        else smooth_scroll(scroll_base() - delta * S(60) / 120);
        return 0;
    }
    case 0x020E: {                              /* WM_MOUSEHWHEEL */
        int delta = (short)HIWORD(wp);
        hwheel(lp, delta * S(60) / 120);
        return 0;
    }
    case WM_KEYDOWN:
        as_stop();
        switch (wp) {
        case VK_UP:    smooth_scroll(scroll_base() - S(40)); break;
        case VK_DOWN:  smooth_scroll(scroll_base() + S(40)); break;
        case VK_PRIOR: smooth_scroll(scroll_base() - view_h + S(40)); break;
        case VK_NEXT:
        case VK_SPACE: smooth_scroll(scroll_base() + view_h - S(40)); break;
        case VK_HOME:  smooth_scroll(0); break;
        case VK_END:   smooth_scroll(doc_h); break;
        case VK_ESCAPE:
            if (find_visible) find_hide();
            sel_a.item = sel_b.item = -1;
            InvalidateRect(h, NULL, FALSE);
            break;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        DWORD now = GetMessageTime();
        SelPos p = pos_at(x, y);
        int bx = box_at(x, y);

        tip_relay(msg, wp, lp);
        if (as_on) { as_stop(); swallow_up = 1; return 0; }
        {
            int onthumb, sb = sbar_hit(x, y, &onthumb);
            if (sb >= 0) {                                      /* a code block's scrollbar */
                RECT tr, th;
                sbar_geom(&cboxes[sb], &tr, &th);
                if (onthumb) {
                    sb_drag = sb;
                    sb_grab = x - th.left;
                    SetCapture(h);
                } else {
                    swallow_up = 1;
                    box_hscroll(sb, cboxes[sb].hs + (x < th.left ? -1 : 1) * (cboxes[sb].w - S(40)));
                }
                InvalidateRect(h, NULL, FALSE);
                return 0;
            }
        }
        if (bx >= 0 && (bx == hot_box || bx == copied_box) && in_btn(bx, x, y)) {     /* a Copy button */
            swallow_up = 1;
            copy_code(bx);
            return 0;
        }

        if (now - last_click_t <= GetDoubleClickTime() && abs(x - last_click_x) <= 4 &&
            abs(y - last_click_y) <= 4 && click_count < 3)
            click_count++;
        else
            click_count = 1;
        last_click_t = now; last_click_x = x; last_click_y = y;
        down_x = x; down_y = y;
        sel_moved = 0;
        SetCapture(h);
        if (p.item < 0) {
            sel_a.item = sel_b.item = -1;
        } else if (click_count == 1) {
            sel_a = sel_b = p;
            sel_dragging = 1;
            SetTimer(h, 2, 50, NULL);           /* scroll while dragging past the edge */
        } else if (click_count == 2) {
            select_word(p);
            sel_moved = 1;
        } else {
            select_block(p.item);
            sel_moved = 1;
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        track_leave();
        if (sb_drag >= 0) { sb_move((short)LOWORD(lp)); return 0; }
        update_hover();
        tip_relay(msg, wp, lp);
        if (sel_dragging && (wp & MK_LBUTTON)) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            if (!sel_moved && (abs(x - down_x) >= 3 || abs(y - down_y) >= 3)) sel_moved = 1;
            if (sel_moved) {
                SelPos p = pos_at(x, y);
                if (p.item >= 0) sel_b = p;
                InvalidateRect(h, NULL, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        int was_drag = sel_moved;
        if (sb_drag >= 0) { sb_drag = -1; ReleaseCapture(); InvalidateRect(h, NULL, FALSE); return 0; }
        if (swallow_up) { swallow_up = 0; return 0; }
        if (GetCapture() == h) ReleaseCapture();
        KillTimer(h, 2);
        sel_dragging = 0;
        if (!was_drag) {                        /* a plain click: follow a link, drop any selection */
            Item *it = link_at((short)LOWORD(lp), (short)HIWORD(lp));
            sel_a.item = sel_b.item = -1;
            InvalidateRect(h, NULL, FALSE);
            if (it && it->link > 0 && it->link <= nlinks) open_link(links[it->link - 1]);
        }
        return 0;
    }
    case 0x0207:                                /* WM_MBUTTONDOWN */
        if (as_on) as_stop();
        else if (g_nt) as_start((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case 0x0208:                                /* WM_MBUTTONUP: holding and dragging ends on release */
        if (as_on && (GetTickCount() - as_t0 > 350 || abs((short)LOWORD(lp) - as_x) > as_dead() ||
                      abs((short)HIWORD(lp) - as_y) > as_dead()))
            as_stop();
        return 0;
    case WM_RBUTTONDOWN:
        if (as_on) { as_stop(); return 0; }
        break;
    case WM_CAPTURECHANGED:
        sb_drag = -1;
        if (as_on) as_stop();
        sel_dragging = 0;
        KillTimer(h, 2);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            int th;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (as_on) SetCursor(LoadCursor(NULL, IDC_SIZEALL));
            else if (sb_drag >= 0 || sbar_hit(pt.x, pt.y, &th) >= 0) SetCursor(cur_arrow);
            else SetCursor((link_at(pt.x, pt.y) || btn_hot) ? cur_hand : (text_at(pt.x, pt.y) ? cur_ibeam : cur_arrow));
            return TRUE;
        }
        break;
    case 0x02A3:                                /* WM_MOUSELEAVE */
        mouse_tracking = 0;
        hot_box = -1; btn_hot = 0;
        tip_show_for(0);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_CONTEXTMENU:
        context_menu(h, lp);
        return 0;
    case WM_SETTINGCHANGE:
        if (g_theme == 0 && !wp && lp && !strcmp((const char *)lp, "ImmersiveColorSet")) {
            int was = g_dark;
            apply_palette();
            if (was != g_dark) {
                apply_window_theme();
                relayout();
                InvalidateRect(h, NULL, FALSE);
            }
        }
        break;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_PAGE);
        return (LRESULT)br_edit;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_BLOCKBG);
        return (LRESULT)br_panel;
    case 0x91:                                  /* WM_UAHDRAWMENU: the menu bar background */
        if (g_dark) {
            RECT bar;
            if (menubar_rect(h, &bar)) {
                menubar_brushes();
                FillRect(((UahMenu *)lp)->hdc, &bar, br_bar);
                return TRUE;
            }
        }
        break;
    case 0x92:                                  /* WM_UAHDRAWMENUITEM */
        if (g_dark) { draw_menubar_item((UahDrawItem *)lp); return TRUE; }
        break;
    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        LRESULT r = DefWindowProc(h, msg, wp, lp);
        if (g_dark) paint_menu_line(h);
        return r;
    }
    case WM_SETFOCUS:
        return 0;
    case WM_DROPFILES: {
        char path[MAX_PATH];
        DragQueryFile((HDROP)wp, 0, path, sizeof(path));
        DragFinish((HDROP)wp);
        load_file(path);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_OPEN:    open_dialog(); break;
        case ID_EXIT:    DestroyWindow(h); break;
        case ID_RELOAD:  if (curfile[0]) load_file(curfile); break;
        case ID_COPY:    copy_text(0); break;
        case ID_COPYALL: copy_text(1); break;
        case ID_SELALL:  select_all(); InvalidateRect(h, NULL, FALSE); break;
        case ID_PASTE:   paste_text(); break;
        case ID_FIND:      find_show(); break;
        case ID_FINDNEXT:  if (!find_visible) find_show(); else find_do(1, 0); break;
        case ID_FINDPREV:  if (!find_visible) find_show(); else find_do(-1, 0); break;
        case ID_FINDCLOSE: find_hide(); break;
        case ID_FINDEDIT:  if (HIWORD(wp) == EN_CHANGE) find_do(1, 1); break;
        case ID_THEME_AUTO:  set_theme(0); break;
        case ID_THEME_LIGHT: set_theme(1); break;
        case ID_THEME_DARK:  set_theme(2); break;
        case ID_WIDTH:   toggle_width(); break;
        case ID_SMOOTH:  g_smooth = !g_smooth; save_setting("Smooth", g_smooth); update_view_menu(); break;
        case ID_ZOOMIN:  set_zoom(zoom_now() + 10); break;
        case ID_ZOOMOUT: set_zoom(zoom_now() - 10); break;
        case ID_ZOOM100: set_zoom(100); break;
        case ID_ABOUT:
            MessageBox(h, "mdview " VER_STR "\n\nA Markdown viewer for Windows 95 through Windows 11\n\n"
                          "Open a file, drag one onto the window, or press F5 to reload.",
                       "About mdview", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;
    case WM_DESTROY:
        save_window();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEX wc;
    MSG m;
    HMENU bar, file, edit, view, help;
    ACCEL acc[16];
    HACCEL accel;
    char arg[MAX_PATH], *w;
    HDC sdc;
    FARPROC dpiaware;

    dpiaware = GetProcAddress(GetModuleHandle("user32.dll"), "SetProcessDPIAware");
    if (dpiaware) dpiaware();            /* crisp text on scaled displays (absent on Win9x) */

    sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    if (g_dpi < 48) g_dpi = 96;
    g_nt = !(GetVersion() & 0x80000000UL);
    ph_init();
    load_zoom();
    g_theme = load_setting("Theme", 0);
    if (g_theme < 0 || g_theme > 2) g_theme = 0;
    g_limit_width = load_setting("LimitWidth", 0) != 0;
    g_smooth = load_setting("Smooth", 1) != 0;
    apply_palette();
    mdc =CreateCompatibleDC(sdc);
    backdc = CreateCompatibleDC(sdc);
    ReleaseDC(NULL, sdc);

    cur_arrow = LoadCursor(NULL, IDC_ARROW);
    msh_wheel = RegisterWindowMessage("MSWHEEL_ROLLMSG");
    cur_ibeam = LoadCursor(NULL, IDC_IBEAM);
    cur_hand = LoadCursor(NULL, MAKEINTRESOURCE(32649));     /* IDC_HAND: Win98+ */
    if (!cur_hand) cur_hand = cur_arrow;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = cur_arrow;
    wc.hbrBackground = NULL;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(ICON_ID));
    wc.hIconSm = (HICON)LoadImage(inst, MAKEINTRESOURCE(ICON_ID), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = "MdViewWnd";
    RegisterClassEx(&wc);

    bar = CreateMenu();
    file = CreatePopupMenu();
    AppendMenu(file, MF_STRING, ID_OPEN, "&Open...\tCtrl+O");
    AppendMenu(file, MF_STRING, ID_RELOAD, "&Reload\tF5");
    AppendMenu(file, MF_SEPARATOR, 0, NULL);
    AppendMenu(file, MF_STRING, ID_EXIT, "E&xit");
    edit = CreatePopupMenu();
    AppendMenu(edit, MF_STRING, ID_COPY, "&Copy\tCtrl+C");
    AppendMenu(edit, MF_STRING, ID_COPYALL, "Copy &whole document");
    AppendMenu(edit, MF_SEPARATOR, 0, NULL);
    AppendMenu(edit, MF_STRING, ID_SELALL, "Select &all\tCtrl+A");
    AppendMenu(edit, MF_SEPARATOR, 0, NULL);
    AppendMenu(edit, MF_STRING, ID_PASTE, "&Paste as Markdown\tCtrl+V");
    AppendMenu(edit, MF_SEPARATOR, 0, NULL);
    AppendMenu(edit, MF_STRING, ID_FIND, "&Find...\tCtrl+F");
    AppendMenu(edit, MF_STRING, ID_FINDNEXT, "Find &next\tF3");
    AppendMenu(edit, MF_STRING, ID_FINDPREV, "Find pre&vious\tShift+F3");
    view = CreatePopupMenu();
    AppendMenu(view, MF_STRING, ID_ZOOMIN, "Zoom &in\tCtrl++");
    AppendMenu(view, MF_STRING, ID_ZOOMOUT, "Zoom &out\tCtrl+-");
    AppendMenu(view, MF_STRING, ID_ZOOM100, "&Actual size\tCtrl+0");
    AppendMenu(view, MF_SEPARATOR, 0, NULL);
    AppendMenu(view, MF_STRING, ID_WIDTH, "Limit &line width");
    AppendMenu(view, MF_STRING, ID_SMOOTH, "&Smooth scrolling");
    AppendMenu(view, MF_SEPARATOR, 0, NULL);
    AppendMenu(view, MF_STRING, ID_THEME_AUTO, "Theme: &Automatic");
    AppendMenu(view, MF_STRING, ID_THEME_LIGHT, "Theme: &Light");
    AppendMenu(view, MF_STRING, ID_THEME_DARK, "Theme: &Dark");
    view_menu = view;
    help = CreatePopupMenu();
    AppendMenu(help, MF_STRING, ID_ABOUT, "&About");
    AppendMenu(bar, MF_POPUP, (UINT)file, "&File");
    AppendMenu(bar, MF_POPUP, (UINT)edit, "&Edit");
    AppendMenu(bar, MF_POPUP, (UINT)view, "&View");
    AppendMenu(bar, MF_POPUP, (UINT)help, "&Help");

    hwnd = CreateWindow("MdViewWnd", "mdview", WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(800, g_dpi, 96), MulDiv(600, g_dpi, 96),
                        NULL, bar, inst, NULL);
    DragAcceptFiles(hwnd, TRUE);
    apply_window_theme();
    update_view_menu();
    tip_init(inst);

    acc[0].fVirt = FCONTROL | FVIRTKEY; acc[0].key = 'O';              acc[0].cmd = ID_OPEN;
    acc[1].fVirt = FVIRTKEY;            acc[1].key = VK_F5;            acc[1].cmd = ID_RELOAD;
    acc[2].fVirt = FCONTROL | FVIRTKEY; acc[2].key = 'C';              acc[2].cmd = ID_COPY;
    acc[3].fVirt = FCONTROL | FVIRTKEY; acc[3].key = 0xBB;             acc[3].cmd = ID_ZOOMIN;
    acc[4].fVirt = FCONTROL | FVIRTKEY; acc[4].key = VK_ADD;           acc[4].cmd = ID_ZOOMIN;
    acc[5].fVirt = FCONTROL | FVIRTKEY; acc[5].key = 0xBD;             acc[5].cmd = ID_ZOOMOUT;
    acc[6].fVirt = FCONTROL | FVIRTKEY; acc[6].key = VK_SUBTRACT;      acc[6].cmd = ID_ZOOMOUT;
    acc[7].fVirt = FCONTROL | FVIRTKEY; acc[7].key = '0';              acc[7].cmd = ID_ZOOM100;
    acc[8].fVirt = FCONTROL | FVIRTKEY; acc[8].key = 'A';              acc[8].cmd = ID_SELALL;
    acc[9].fVirt = FCONTROL | FVIRTKEY; acc[9].key = 'V';              acc[9].cmd = ID_PASTE;
    acc[10].fVirt = FSHIFT | FVIRTKEY;  acc[10].key = VK_INSERT;       acc[10].cmd = ID_PASTE;
    acc[11].fVirt = FCONTROL | FVIRTKEY; acc[11].key = VK_INSERT;      acc[11].cmd = ID_COPY;
    acc[12].fVirt = FCONTROL | FVIRTKEY; acc[12].key = 'F';          acc[12].cmd = ID_FIND;
    acc[13].fVirt = FVIRTKEY;            acc[13].key = VK_F3;         acc[13].cmd = ID_FINDNEXT;
    acc[14].fVirt = FSHIFT | FVIRTKEY;   acc[14].key = VK_F3;         acc[14].cmd = ID_FINDPREV;
    accel = CreateAcceleratorTable(acc, 15);

    restore_window(show);
    UpdateWindow(hwnd);

    /* command line: strip surrounding quotes */
    strncpy(arg, cmd, MAX_PATH - 1);
    arg[MAX_PATH - 1] = 0;
    w = arg;
    {
        int n;
        while (*w == ' ') w++;
        if (!strnicmp(w, "/diag", 5)) { g_diag = 1; g_diag_layout = 1; w += 5; while (*w == ' ') w++; }
        if (*w == '"') w++;
        n = strlen(w);
        while (n > 0 && (w[n - 1] == '"' || w[n - 1] == ' ')) w[--n] = 0;
    }
    if (*w) {
        load_file(w);
    } else {
        char *t = (char *)malloc(strlen(welcome) + 1);
        strcpy(t, welcome);
        show_text(t, "mdview " VER_STR);
        free(t);
    }

    while (GetMessage(&m, NULL, 0, 0)) {
        if ((find_edit && m.hwnd == find_edit) || !TranslateAccelerator(hwnd, accel, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    return 0;
}

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
       ID_FIND, ID_FINDNEXT, ID_FINDPREV, ID_FINDCLOSE, ID_FINDEDIT, ID_SMOOTH, ID_CONSOLAS, ID_AUTORELOAD };

/* ---- colours ---- */
enum { P_TEXT, P_GRAY, P_HEAD, P_CODE, P_BLOCK, P_LINK, P_PAGE, P_INLINEBG, P_BLOCKBG, P_BLOCKBD,
       P_QBAR, P_RULE, P_GRID, P_THEAD, P_ZEBRA,
       P_KW, P_CTRL, P_STR, P_COM, P_NUM, P_TYPE, P_PRE, P_ATTR, P_DELBG, P_ADDBG, P_KEY, P_COUNT };
static COLORREF pal[P_COUNT];
static const COLORREF pal_light[P_COUNT] = {
    RGB(30, 30, 30), RGB(96, 96, 96), RGB(30, 30, 30), RGB(30, 30, 30), RGB(40, 40, 40), RGB(0, 128, 0),
    RGB(255, 255, 255), RGB(242, 242, 244), RGB(249, 249, 250), RGB(220, 222, 228), RGB(190, 197, 210),
    RGB(205, 210, 218), RGB(196, 202, 212), RGB(222, 229, 242), RGB(247, 249, 252),
    RGB(0, 0, 255), RGB(143, 8, 196), RGB(163, 21, 21), RGB(0, 128, 0), RGB(9, 134, 88), RGB(43, 145, 175),
    RGB(128, 128, 128), RGB(229, 0, 0), RGB(255, 208, 208), RGB(208, 238, 208), RGB(4, 81, 165)
};
static const COLORREF pal_dark[P_COUNT] = {
    RGB(215, 215, 218), RGB(150, 152, 158), RGB(215, 215, 218), RGB(215, 215, 218), RGB(215, 215, 218),
    RGB(110, 200, 110), RGB(30, 30, 32), RGB(46, 46, 50), RGB(35, 35, 38), RGB(62, 62, 70),
    RGB(84, 90, 106), RGB(70, 72, 80), RGB(82, 86, 96), RGB(48, 56, 78), RGB(36, 36, 40),
    RGB(86, 156, 214), RGB(197, 134, 192), RGB(206, 145, 120), RGB(106, 153, 85), RGB(181, 206, 168),
    RGB(78, 201, 176), RGB(155, 155, 155), RGB(240, 110, 110), RGB(92, 40, 40), RGB(36, 78, 40), RGB(156, 220, 254)
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
static int g_theme, g_dark, g_limit_width = 0, g_smooth = 1, g_consolas = 0, g_autoreload = 1;     /* theme: 0 follow the system, 1 light, 2 dark */
static HMENU view_menu;
#define NOBACK     0xFFFFFFFFUL

/* ---- text model ---- */

enum { F_BOLD = 1, F_ITAL = 2, F_CODE = 4, F_LINK = 8, F_IMG = 16, F_BREAK = 32,   /* F_BREAK: a line break (<br>) */
       F_STRIKE = 64, F_UL = 128, F_SUB = 256, F_SUP = 512, F_MARK = 1024 };

typedef struct { char *text; unsigned flags; int link; } Span;     /* link: 1-based index, 0 = none */
typedef struct { Span *s; int n, cap; } Inline;

enum { BK_PARA, BK_HEAD, BK_LIST, BK_QUOTE, BK_CODE, BK_TABLE, BK_RULE, BK_DETAILS };

struct Block;
typedef struct { struct Block *b; int n; } Cell;     /* a table cell: its own little document */

typedef struct Block {
    int kind, level, numbered;
    char num[16];
    Inline inl;                          /* PARA, HEAD, LIST, QUOTE */
    char **lines; int nlines;            /* CODE */
    Cell *cells; int rows, cols, header; int *align;     /* TABLE: cells[r * cols + c]; align 0/1 centre/2 right */
    int y;                               /* top of the block in the laid-out document */
    int inset;                           /* PARA, CODE inside a list item: that item's level + 1 */
    int loose;                           /* LIST: separated from its neighbours by blank lines */
    int task;                            /* LIST: 1 an open [ ] task, 2 a done [x] one */
    int halign;                          /* PARA, HEAD: 1 centred, 2 right (from align=".." in HTML) */
    int did;                             /* DETAILS: index into det_open */
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

static void free_blocks(Block *bl, int n)
{
    int i, j;
    for (i = 0; i < n; i++) {
        Block *b = &bl[i];
        free_inline(&b->inl);
        for (j = 0; j < b->nlines; j++) free(b->lines[j]);
        free(b->lines);
        for (j = 0; j < b->rows * b->cols; j++) free_blocks(b->cells[j].b, b->cells[j].n);
        free(b->cells);
        free(b->align);
    }
    free(bl);
}

static void free_doc(void)
{
    int i;
    free_blocks(blocks, nblocks);
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
static int head_pt(int level);
static int put_cp(unsigned long cp, char *o);

/* &amp; &copy; &#169; &#xA9;: the length of the entity at p ('&'), and its code point; 0 if it is not one */
static int entity_at(const char *p, unsigned long *cp)
{
    static const struct { const char *n; unsigned long cp; } ent[] = {
        {"amp", 38}, {"lt", 60}, {"gt", 62}, {"quot", 34}, {"apos", 39}, {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE},
        {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"laquo", 0xAB}, {"raquo", 0xBB},
        {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022}, {"middot", 0xB7},
        {"times", 0xD7}, {"divide", 0xF7}, {"deg", 0xB0}, {"plusmn", 0xB1}, {"para", 0xB6}, {"sect", 0xA7}, {"euro", 0x20AC},
        {"cent", 0xA2}, {"pound", 0xA3}, {"yen", 0xA5}, {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191},
        {"darr", 0x2193}, {"harr", 0x2194}, {"hearts", 0x2665}, {"infin", 0x221E}, {"ne", 0x2260}, {"le", 0x2264},
        {"ge", 0x2265}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"micro", 0xB5}, {"dagger", 0x2020},
        {"permil", 0x2030}, {NULL, 0}
    };
    const char *q = p + 1;
    int k;
    if (*q == '#') {
        int hex = q[1] == 'x' || q[1] == 'X', digits = 0;
        unsigned long v = 0;
        q += hex ? 2 : 1;
        while (digits < 8 && (hex ? isxdigit((unsigned char)*q) : isdigit((unsigned char)*q))) {
            int d = isdigit((unsigned char)*q) ? *q - '0' : (tolower((unsigned char)*q) - 'a' + 10);
            v = v * (hex ? 16 : 10) + d;
            q++; digits++;
        }
        if (!digits || *q != ';' || !v || v > 0x10FFFFUL) return 0;
        *cp = v;
        return (int)(q + 1 - p);
    }
    for (k = 0; ent[k].n; k++) {
        size_t l = strlen(ent[k].n);
        if (!strncmp(q, ent[k].n, l) && q[l] == ';') {
            *cp = ent[k].cp;
            return (int)(l + 2);
        }
    }
    return 0;
}

/* <br> inside a paragraph or table cell */
static void html_break(Inline *out)
{
    if (out->n) add_span(out, " ", 1, F_BREAK, 0);
}

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

/* length of a bare URL (http://, https://, www.) starting at p, without trailing punctuation; 0 if none */
static int bare_url_len(const char *p)
{
    int pre = !strnicmp(p, "https://", 8) ? 8 : !strnicmp(p, "http://", 7) ? 7 : !strnicmp(p, "www.", 4) ? 4 : 0;
    const char *q = p + pre;
    int n, open = 0;
    if (!pre) return 0;
    while (*q && !isspace((unsigned char)*q) && *q != '<' && *q != '`') {
        if (*q == '(') open++;
        if (*q == ')') open--;
        q++;
    }
    n = (int)(q - p);
    while (n > pre) {                                   /* sentence punctuation and emphasis after the URL */
        char c = p[n - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '\'' || c == '"' || c == '*' || c == '_' ||
            c == '~' || (c == ')' && open < 0)) {
            if (c == ')') open++;
            n--;
        } else {
            break;
        }
    }
    return n > pre + 1 ? n : 0;
}

static void parse_ex(Inline *out, const char *s, unsigned base, int link)
{
    const char *a_href, *a_inner, *a_after;
    int a_hlen, a_ilen;
    char run[RUNMAX];
    int n = 0, bold = 0, italic = 0, code = 0;
    unsigned xf = 0;                    /* strike, underline, sub/superscript, mark from ~~ and HTML tags */
    unsigned long ecp;
    int el;
    const char *p = s;

#define FLUSH() do { if (n) { add_span(out, run, n, base | xf | (bold ? F_BOLD : 0) | (italic ? F_ITAL : 0) | (code ? F_CODE : 0), link); n = 0; } } while (0)

    while (*p) {
        if (n >= RUNMAX - 8) FLUSH();

        if (code) {
            /* inside <code> / <kbd> nothing is special except the closing tag */
            if (*p == '<' && p[1] == '/' && (!strnicmp(p + 2, "code>", 5) || !strnicmp(p + 2, "kbd>", 4))) {
                FLUSH();
                code = 0;
                p = strchr(p, '>') + 1;
            } else {
                run[n++] = *p++;
            }
        } else if (*p == '\\' && p[1] && ispunct((unsigned char)p[1])) {
            run[n++] = p[1];
            p += 2;
        } else if (*p == '`') {
            int bn = 0, m;
            const char *q, *cl = NULL;
            while (p[bn] == '`') bn++;
            q = p + bn;
            for (;;) {
                while (*q && *q != '`') q++;
                if (!*q) break;
                for (m = 0; q[m] == '`'; m++) ;
                if (m == bn) { cl = q; break; }
                q += m;
            }
            if (cl) {                                   /* a code span, closed by a run of the same length */
                const char *cs = p + bn;
                int cn = (int)(cl - cs);
                FLUSH();
                if (cn > 2 && cs[0] == ' ' && cs[cn - 1] == ' ') { cs++; cn -= 2; }
                add_span(out, cs, cn, base | xf | (bold ? F_BOLD : 0) | (italic ? F_ITAL : 0) | F_CODE, link);
                p = cl + bn;
            } else {                                    /* unmatched backticks are plain text */
                for (m = 0; m < bn; m++)
                    if (n < RUNMAX - 1) run[n++] = '`';
                p += bn;
            }
        } else if (*p == '&' && (el = entity_at(p, &ecp)) > 0) {
            n += put_cp(ecp, run + n);
            p += el;
        } else if (*p == '~' && p[1] == '~' && ((xf & F_STRIKE) || (p[2] && p[2] != ' ' && strstr(p + 2, "~~")))) {
            FLUSH();
            xf ^= F_STRIKE;
            p += 2;
        } else if (!link && (*p == 'h' || *p == 'H' || *p == 'w' || *p == 'W') && (p == s || !isalnum((unsigned char)p[-1])) &&
                   bare_url_len(p) > 0) {
            int ul = bare_url_len(p), www = (p[0] == 'w' || p[0] == 'W');
            char *u = (char *)malloc(ul + 8);
            sprintf(u, "%s%.*s", www ? "http://" : "", ul, p);
            FLUSH();
            add_span(out, p, ul, base | xf | (bold ? F_BOLD : 0) | (italic ? F_ITAL : 0) | F_LINK, add_link(u, (int)strlen(u)));
            free(u);
            p += ul;
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
            } else if (strncmp(p, "<!--", 4)) {
                const char *nm = p + 1;
                char name[8];
                int closing = *nm == '/', k = 0;
                if (closing) nm++;
                while (k < 7 && isalnum((unsigned char)nm[k])) { name[k] = (char)tolower((unsigned char)nm[k]); k++; }
                name[k] = 0;
                if (!closing && !strcmp(name, "br")) {
                    FLUSH();
                    html_break(out);
                } else if (!closing && !strcmp(name, "img")) {     /* <img alt=".."> shows like ![alt](..) */
                    const char *q, *alt = NULL;
                    char label[300];
                    int l, alen = 0;
                    for (q = p + 1; q < gt && !alt; q++)
                        if ((q[-1] == ' ' || q[-1] == 9 || q[-1] == 10) && !strnicmp(q, "alt=", 4)) {
                            alt = q + 4;
                            if (*alt == '"' || *alt == 39) {
                                char qc = *alt++;
                                while (alt + alen < gt && alt[alen] != qc) alen++;
                            } else {
                                while (alt + alen < gt && alt[alen] != ' ' && alt[alen] != '/') alen++;
                            }
                        }
                    FLUSH();
                    l = alen ? sprintf(label, "[image: %.*s]", alen > 250 ? 250 : alen, alt) : sprintf(label, "[image]");
                    add_span(out, label, l, F_IMG | F_ITAL, 0);
                } else {
                    unsigned bit = 0;
                    if (!strcmp(name, "u") || !strcmp(name, "ins")) bit = F_UL;
                    else if (!strcmp(name, "s") || !strcmp(name, "del") || !strcmp(name, "strike")) bit = F_STRIKE;
                    else if (!strcmp(name, "sub")) bit = F_SUB;
                    else if (!strcmp(name, "sup")) bit = F_SUP;
                    else if (!strcmp(name, "mark")) bit = F_MARK;
                    if (!strcmp(name, "b") || !strcmp(name, "strong")) {
                        FLUSH();
                        bold = !closing;
                    } else if (!strcmp(name, "i") || !strcmp(name, "em")) {
                        FLUSH();
                        italic = !closing;
                    } else if (bit) {
                        FLUSH();
                        if (closing) xf &= ~bit; else xf |= bit;
                    } else if (!closing && (!strcmp(name, "code") || !strcmp(name, "kbd"))) {
                        FLUSH();
                        code = 1;
                    }
                }
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
static int pend_loose;                  /* the list item being collected follows a blank line */
static int pend_task;                   /* the list item being collected is a task: 1 open, 2 done */
static int pend_hard;                   /* the last line of the paragraph ended with a hard break */
static int ord_next[6];                 /* the number the next ordered item gets, per level (0 = start a new list) */
static int lv_mark[8], lv_cont[8], lv_n;     /* marker and content columns of the open list items, outermost first */
static int list_ind = -1, list_lvl;     /* content column and level of the latest list item */
enum { P_NONE, P_PARA, P_QUOTE, P_LIST };

static void append(const char *s)
{
    int n, len, hard = 0;
    while (*s == ' ' || *s == '\t') s++;
    len = (int)strlen(s);
    n = len;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n > 0 && len - n >= 2) hard = 1;                        /* two spaces at the end of the line */
    else if (n > 0 && s[n - 1] == '\\' && (n < 2 || s[n - 2] != '\\')) { hard = 1; n--; }     /* or a backslash */
    if (pend_hard && pend_len) { memcpy(pend + pend_len, "<br>", 4); pend_len += 4; }
    else if (pend_len && pend[pend_len - 1] != ' ') pend[pend_len++] = ' ';
    pend_hard = hard && n > 0;
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
        int cont = nblocks > 1 && (blocks[nblocks - 2].kind == BK_LIST || blocks[nblocks - 2].inset), lv;
        b->level = pend_indent / 2 > 5 ? 5 : pend_indent / 2;
        b->numbered = pend_num[0] != 0;
        strcpy(b->num, pend_num);
        b->loose = pend_loose;
        pend_loose = 0;
        if (!cont) memset(ord_next, 0, sizeof(ord_next));
        if (b->numbered) {                                  /* 1. 1. 1. counts 1. 2. 3. */
            int v = ord_next[b->level] > 0 ? ord_next[b->level] : atoi(pend_num);
            sprintf(b->num, "%d", v);
            ord_next[b->level] = v + 1;
        } else {
            ord_next[b->level] = 0;
        }
        for (lv = b->level + 1; lv < 6; lv++) ord_next[lv] = 0;
    }
    if (pend_kind == P_PARA) b->inset = pend_inset;
    if (pend_kind == P_LIST) { b->task = pend_task; pend_task = 0; }
    pend_hard = 0;
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
    if ((*t == '-' || *t == '*' || *t == '+') && (t[1] == ' ' || t[1] == '	')) return t + 2;
    if ((*t == '*' || *t == '+') && !t[1]) return t + 1;          /* a lone marker is an empty item */
    if (isdigit((unsigned char)*t)) {
        int n = 0;
        while (isdigit((unsigned char)t[n])) n++;
        if (n <= 9 && (t[n] == '.' || t[n] == ')') && (t[n + 1] == ' ' || t[n + 1] == '	')) {
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

    while (*p == ' ' || *p == '	') p++;
    if (*p == '|') p++;
    while (*p && c < max) {
        const char *start;
        int n;
        while (*p == ' ') p++;
        start = p;
        while (*p && *p != '|') {
            if (*p == '\\' && p[1] == '|') p++;              /* \| is a pipe inside the cell */
            p++;
        }
        n = (int)(p - start);
        while (n > 0 && start[n - 1] == ' ') n--;
        if (!*p && n == 0) break;                 /* trailing "|" leaves an empty tail */
        out[c] = (char *)malloc(n + 1);
        memcpy(out[c], start, n);
        out[c][n] = 0;
        {
            char *w = out[c], *q = out[c];
            for (; *q; q++) {
                if (*q == '\\' && q[1] == '|') q++;
                *w++ = *q;
            }
            *w = 0;
        }
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

static void parse_cell(Cell *c, const char *text);

static int has_pipe(const char *s)
{
    for (; *s; s++) {
        if (*s == '\\' && s[1]) s++;
        else if (*s == '|') return 1;
    }
    return 0;
}

static int ends_pipe(const char *s)
{
    const char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == 9 || e[-1] == 13)) e--;
    return e > s && e[-1] == '|';
}

/* a table whose header row is followed by a delimiter row of the same width; pipes at the ends are optional */
static int table_starts(char **lines, int i, int nl)
{
    char *a[MAXCOLS], *b[MAXCOLS];
    int na, nb, k, ok;
    const char *u;
    if (i + 1 >= nl || !has_pipe(lines[i])) return 0;
    u = lines[i + 1];
    while (*u == ' ' || *u == '	') u++;
    if (!*u) return 0;
    na = split_cells(lines[i], a, MAXCOLS);
    nb = split_cells(lines[i + 1], b, MAXCOLS);
    ok = na > 0 && na == nb && is_separator_cells(b, nb);
    for (k = 0; k < na; k++) free(a[k]);
    for (k = 0; k < nb; k++) free(b[k]);
    return ok;
}

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
    b->cells = (Cell *)calloc(nrows * ncols + 1, sizeof(Cell));
    for (r = 0; r < nrows; r++) {
        for (c = 0; c < rowlen[r]; c++) {
            parse_cell(&b->cells[r * ncols + c], rowcells[r][c]);
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

static void md_indent(Buf *o, int n)
{
    while (n-- > 0) buf_add(o, " ", 1);
}

/* converts from the opening <ul>/<ol> at q to just past its closing tag; whole: convert everything to the end */
static const char *convert_list(Buf *o, const char *q, int whole)
{
    int depth = 0, ordered[16] = {0}, count[16] = {0}, mark_end = -1, hbold = 0;
    while (*q) {
        if (*q == '<') {
            const char *gt = strchr(q, '>');
            char name[8];
            int closing = q[1] == '/', k = 0;
            const char *nm = q + 1 + closing;
            if (!gt) { buf_add(o, q, 1); q++; continue; }
            while (k < 7 && isalnum((unsigned char)nm[k])) { name[k] = (char)tolower((unsigned char)nm[k]); k++; }
            name[k] = 0;
            if (!k) { buf_add(o, q, 1); q++; continue; }        /* a bare "<" */
            if (!strcmp(name, "ul") || !strcmp(name, "ol")) {
                if (!closing) {
                    if (depth < 15) depth++;
                    ordered[depth] = name[0] == 'o';
                    count[depth] = 0;
                } else if (--depth <= 0) {
                    depth = 0;
                    buf_add(o, "\n\n", 2);
                    if (!whole) return gt + 1;
                }
            } else if (!strcmp(name, "li")) {
                if (!closing) {
                    char mark[16];
                    int ind = depth > 1 ? (depth - 1) * 2 : 0, m;
                    buf_add(o, "\n", 1);
                    md_indent(o, ind);
                    if (ordered[depth > 0 ? depth : 1]) m = sprintf(mark, "%d. ", ++count[depth > 0 ? depth : 1]);
                    else m = sprintf(mark, "- ");
                    buf_add(o, mark, m);
                    mark_end = o->n;
                } else if (depth > 0) {
                    const char *nx = gt + 1;
                    while (*nx == ' ' || *nx == '\n' || *nx == '\r' || *nx == '\t') nx++;
                    if (!(nx[0] == '<' && (!strnicmp(nx + 1, "li", 2) || !strnicmp(nx + 1, "/ul", 3) || !strnicmp(nx + 1, "/ol", 3)))) {
                        buf_add(o, "\n\n", 2);                 /* text after </li> carries on under the item */
                        md_indent(o, depth * 2);
                    }
                }
            } else if ((name[0] == 'p' && !name[1]) || !strcmp(name, "div") ||
                       (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2])) {
                if (name[0] == 'h' && closing && hbold) {       /* a heading that opened on a bullet stays on it, in bold */
                    buf_add(o, "**", 2);
                    hbold = 0;
                } else if (closing || mark_end != o->n) {       /* a block of its own, unless it opens right after a bullet */
                    buf_add(o, "\n\n", 2);
                    md_indent(o, depth * 2);
                }
                if (name[0] == 'h' && !closing) {
                    if (mark_end == o->n) {
                        buf_add(o, "**", 2);
                        hbold = 1;
                    } else {
                        int h = name[1] - '0';
                        while (h--) buf_add(o, "#", 1);
                        buf_add(o, " ", 1);
                    }
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
        } else if (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') {
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
            p = convert_list(&o, u, 0);
            continue;
        }
        buf_add(&o, p, ll);
        p += ll;
    }
    return o.s;
}

static void parse_doc_in(char *text);

/* is there an HTML tag at p ('<') that makes block structure: <ul> <ol> <li> <p> <div> <h1>..<h6> */
static int block_tag_at(const char *p)
{
    char name[8];
    int k = 0;
    const char *nm = p + 1 + (p[1] == '/');
    while (k < 7 && isalnum((unsigned char)nm[k])) { name[k] = (char)tolower((unsigned char)nm[k]); k++; }
    name[k] = 0;
    if (nm[k] != '>' && nm[k] != '/' && nm[k] != ' ' && nm[k] != '\t' && nm[k] != '\n' && nm[k]) return 0;     /* <p:sldIdLst> is no <p> */
    return !strcmp(name, "ul") || !strcmp(name, "ol") || !strcmp(name, "li") || !strcmp(name, "div") ||
           (name[0] == 'p' && !name[1]) || (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]);
}

/* parse md (modified in place) as a document of its own into c, keeping the state of the document being parsed */
static void parse_nested(Cell *c, char *md)
{
    Block *sb_blocks = blocks;
    int sb_n = nblocks, sb_cap = capblocks, sb_plen = pend_len, sb_kind = pend_kind, sb_ind = pend_indent;
    int sb_inset = pend_inset, sb_lind = list_ind, sb_llvl = list_lvl, sb_lvn = lv_n, sb_lvm[8], sb_lvc[8];
    char *sb_pend = pend, sb_num[16];
    memcpy(sb_num, pend_num, sizeof(sb_num));
    memcpy(sb_lvm, lv_mark, sizeof(sb_lvm)); memcpy(sb_lvc, lv_cont, sizeof(sb_lvc));
    blocks = NULL; nblocks = capblocks = 0;
    parse_doc_in(md);
    c->b = blocks; c->n = nblocks;
    blocks = sb_blocks; nblocks = sb_n; capblocks = sb_cap;
    pend = sb_pend; pend_len = sb_plen; pend_kind = sb_kind; pend_indent = sb_ind;
    pend_inset = sb_inset; list_ind = sb_lind; list_lvl = sb_llvl;
    memcpy(pend_num, sb_num, sizeof(sb_num));
    lv_n = sb_lvn; memcpy(lv_mark, sb_lvm, sizeof(sb_lvm)); memcpy(lv_cont, sb_lvc, sizeof(sb_lvc));
}

static int has_block_tag(const char *text)
{
    const char *p;
    for (p = text; *p; p++) {
        if (*p == '`') {                                    /* tags inside a code span are text */
            int n = 0, m;
            const char *q;
            while (p[n] == '`') n++;
            for (q = p + n; *q; q += m) {
                while (*q && *q != '`') q++;
                for (m = 0; q[m] == '`'; m++) ;
                if (m == n) { p = q + m - 1; break; }
                if (!*q) break;
            }
            if (!*q) p += n - 1;
        } else if (*p == '<' && block_tag_at(p)) {
            return 1;
        }
    }
    return 0;
}

/* Markdown whose HTML blocks (<p> <ul> <h2> ..) are rewritten as Markdown, parsed as a document of its own */
static void parse_body(Cell *c, const char *text)
{
    char *md;
    if (has_block_tag(text)) {
        Buf o;
        memset(&o, 0, sizeof(o));
        convert_list(&o, text, 1);
        md = o.s;
    } else {
        md = (char *)malloc(strlen(text) + 1);
        strcpy(md, text);
    }
    if (md) {
        parse_nested(c, md);
        free(md);
    }
}

/* a cell with plain text is one paragraph; one with <ul>, <p>, <h2>.. is parsed like a document of its own */
static void parse_cell(Cell *c, const char *text)
{
    if (has_block_tag(text)) parse_body(c, text);
    if (!c->n) {
        free(c->b);
        c->b = (Block *)calloc(1, sizeof(Block));
        c->b->kind = BK_PARA;
        c->n = 1;
        parse_inline(&c->b->inl, text);
    }
}

static int ph_enc(unsigned long cp, char *o);
static int nph;

#define MAXDET 1024
static char det_open[MAXDET];           /* which <details> are open, by Block.did */
static int ndet;

/* the "</name" that closes the element whose opening tag has just been read */
static const char *matching_close(const char *s, const char *name)
{
    size_t n = strlen(name);
    int depth = 1;
    for (; *s; s++) {
        if (*s != '<') continue;
        if (s[1] == '/' && !strnicmp(s + 2, name, n) && !isalnum((unsigned char)s[2 + n])) {
            if (--depth == 0) return s;
        } else if (!strnicmp(s + 1, name, n) && !isalnum((unsigned char)s[1 + n])) {
            depth++;
        }
    }
    return NULL;
}

/* <details>..</details>, and <p>/<div>/<center>/<h1>.. with align="center|right", starting at line i.
   Returns the last line used, or -1 if the line starts neither (nothing is changed then). */
static int html_block(char **lines, int i, int nl)
{
    const char *t = lines[i], *gt, *close = NULL, *body;
    char name[8];
    int k = 0, j, align = 0, details, hn, open = 0;
    Buf all;
    Cell c;

    while (*t == ' ' || *t == '\t') t++;
    if (*t != '<') return -1;
    while (k < 7 && isalnum((unsigned char)t[1 + k])) { name[k] = (char)tolower((unsigned char)t[1 + k]); k++; }
    name[k] = 0;
    details = !strcmp(name, "details");
    hn = name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2];
    if (!details && strcmp(name, "p") && strcmp(name, "div") && strcmp(name, "center") && !hn) return -1;
    gt = strchr(t, '>');
    if (!gt) return -1;
    if (details) {
        const char *a;
        for (a = t + 8; a < gt; a++) if (!strnicmp(a, "open", 4)) open = 1;
    } else if (!strcmp(name, "center")) {
        align = 1;
    } else {
        const char *a;
        for (a = t; a < gt; a++)
            if (!strnicmp(a, "align=", 6)) {
                a += 6;
                if (*a == '"' || *a == '\'') a++;
                align = !strnicmp(a, "center", 6) ? 1 : !strnicmp(a, "right", 5) ? 2 : 0;
                break;
            }
        if (!align) return -1;
    }

    memset(&all, 0, sizeof(all));
    for (j = i; j < nl; j++) {                                  /* join lines until the element closes */
        const char *u = j == i ? t : lines[j];
        buf_add(&all, u, (int)strlen(u));
        buf_add(&all, "\n", 1);
        close = matching_close(strchr(all.s, '>') + 1, name);
        if (close) break;
    }
    if (!close) { free(all.s); return -1; }
    body = strchr(all.s, '>') + 1;
    flush();
    list_ind = -1;
    memset(&c, 0, sizeof(c));

    {
        char *inner = (char *)malloc(close - body + 8 + 1), *pre = inner;
        Block *b;
        memcpy(inner, body, (size_t)(close - body));
        inner[close - body] = 0;
        {   /* the tags' own indentation is layout, not an indented code block */
            char *rd = inner, *wr = inner;
            int bol = 1;
            for (; *rd; rd++) {
                if (bol && (*rd == ' ' || *rd == 9)) continue;
                bol = *rd == 10;
                *wr++ = *rd;
            }
            *wr = 0;
        }
        if (details) {
            const char *sp = NULL, *q, *sgt, *sc;
            char *sum, *rest;
            int link, tri;
            char url[32], mark[8];
            for (q = inner; *q && !sp; q++)
                if (q[0] == '<' && !strnicmp(q + 1, "summary", 7) && !isalnum((unsigned char)q[8])) sp = q;
            sgt = sp ? strchr(sp, '>') : NULL;
            sc = sgt ? matching_close(sgt + 1, "summary") : NULL;
            sum = (char *)malloc(strlen(inner) + 16);
            rest = (char *)malloc(strlen(inner) + 16);
            if (sc) {
                memcpy(sum, sgt + 1, (size_t)(sc - sgt - 1));
                sum[sc - sgt - 1] = 0;
                memcpy(rest, inner, (size_t)(sp - inner));
                strcpy(rest + (sp - inner), strchr(sc, '>') ? strchr(sc, '>') + 1 : "");
            } else {
                strcpy(sum, "Details");
                strcpy(rest, inner);
            }
            parse_body(&c, rest);
            b = new_block(BK_DETAILS);
            b->did = ndet < MAXDET ? ndet : MAXDET - 1;
            ndet++;
            det_open[b->did] = (char)open;
            sprintf(url, "mdview:details:%d", b->did);
            link = add_link(url, (int)strlen(url));
            tri = ph_enc(0x25B6, mark);
            nph = 1;
            add_span(&b->inl, mark, tri, 0, link);              /* the triangle, redrawn to match the state in the layout */
            add_span(&b->inl, " ", 1, 0, link);
            parse_ex(&b->inl, sum, 0, link);
            b->cells = (Cell *)malloc(sizeof(Cell));
            b->cells[0] = c;
            b->rows = b->cols = 1;
            free(sum);
            free(rest);
        } else {
            if (hn) {                                           /* <h1 align="center">Title</h1> */
                int h = name[1] - '0';
                pre = inner + 8;
                memmove(pre, inner, strlen(inner) + 1);
                memset(inner, '#', h);
                inner[h] = ' ';
                memmove(inner + h + 1, pre, strlen(pre) + 1);
            }
            parse_body(&c, inner);
            for (k = 0; k < c.n; k++) {                         /* its blocks join the document, aligned */
                b = new_block(c.b[k].kind);
                *b = c.b[k];
                if (b->kind == BK_PARA || b->kind == BK_HEAD) b->halign = align;
            }
            free(c.b);
        }
        free(inner);
    }
    free(all.s);
    return j;
}

static void parse_doc_in(char *text)
{
    char **lines, *p;
    int nl = 1, i, was_blank = 0, prev_blank = 0;
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

    pend = (char *)malloc(total * 3 + 64);                 /* hard breaks add a <br> per line */
    pend_hard = 0;
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

        was_blank = prev_blank;
        prev_blank = !*t;
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
                const char *u = lines[i + 1 + k];
                int w = 0;
                while ((*u == ' ' || *u == '\t') && w < ind) { w += *u == '\t' ? 4 : 1; u++; }
                b->lines[k] = (char *)malloc(strlen(u) + 1);
                strcpy(b->lines[k], u);
            }
            i = j;                       /* skip past the closing fence */
            continue;
        }
        if (!*t) {
            flush();
            continue;
        }
        if (*t == '<') {
            int e = html_block(lines, i, nl);
            if (e >= 0) { i = e; continue; }
        }
        if (pend_kind == P_NONE && list_ind >= 0 && ind < list_ind && !list_item(t, num)) {
            int hk = lv_n - 1;              /* text indented less than the newest item belongs to the nearest enclosing one */
            while (hk >= 0 && ind < lv_cont[hk]) hk--;
            if (hk >= 0) { lv_n = hk + 1; list_ind = lv_cont[hk]; list_lvl = hk; }
        }
        if (pend_kind == P_NONE && ((list_ind < 0 && ind >= 4) ||      /* 4+ spaces outside a list is code, whatever it starts with */
            (list_ind >= 0 && ind >= list_ind && *t != '#' && *t != '|' && *t != '>' && !is_rule(t) && !list_item(t, num)))) {   /* 4+ spaces outside a list is code, even when it looks like a bullet */
            if (ind >= (list_ind >= 0 ? list_ind : 0) + 4) {        /* indented code, also inside a list item */
                int j = i, k, strip = (list_ind >= 0 ? list_ind : 0) + 4, last = i;
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
                b->inset = list_ind >= 0 ? list_lvl + 1 : 0;
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
        if (table_starts(lines, i, nl) && (*t == '|' || pend_kind == P_NONE) ||
            *t == '|' && ends_pipe(t)) {            /* a headerless "| a | b |" row, which the front matter table relies on */
            int j = i, tin = (list_ind >= 0 && ind >= list_ind) ? list_lvl + 1 : 0;
            flush();
            if (!tin) list_ind = -1;
            while (j < nl) {
                const char *u = lines[j];
                while (*u == ' ' || *u == '\t') u++;
                if (*u != '|' && !has_pipe(u)) break;
                j++;
            }
            parse_table(lines, i, j - i);
            if (tin && nblocks > 0) blocks[nblocks - 1].inset = tin;
            i = j - 1;
            continue;
        }
        if (*t == '>') {
            Buf qb;
            Cell qc;
            Block *qbk;
            char qnum[16];
            int j = i, prev_blank = 1;
            int qin = (list_ind >= 0 && ind >= list_ind) ? list_lvl + 1 : 0;     /* quoted inside a list item */
            flush();
            if (!qin) list_ind = -1;
            memset(&qb, 0, sizeof(qb));
            memset(&qc, 0, sizeof(qc));
            while (j < nl) {                                /* the quote's lines, with the "> " taken off */
                const char *u = lines[j];
                while (*u == ' ' || *u == '\t') u++;
                if (*u == '>') {
                    u++;
                    if (*u == ' ') u++;
                    buf_add(&qb, u, (int)strlen(u));
                    buf_add(&qb, "\n", 1);
                    prev_blank = !*u;
                } else if (*u && !prev_blank && *u != '#' && *u != '|' && strncmp(u, "```", 3) && strncmp(u, "~~~", 3) &&
                           !is_rule(u) && !list_item(u, qnum)) {        /* a lazy continuation line */
                    buf_add(&qb, u, (int)strlen(u));
                    buf_add(&qb, "\n", 1);
                } else {
                    break;
                }
                j++;
            }
            if (qb.s) parse_nested(&qc, qb.s);
            qbk = new_block(BK_QUOTE);
            qbk->inset = qin;
            qbk->cells = (Cell *)malloc(sizeof(Cell));
            qbk->cells[0] = qc;
            qbk->rows = qbk->cols = 1;
            free(qb.s);
            i = j - 1;
            continue;
        }
        item = list_item(t, num);
        if (item) {
            flush();
            pend_loose = 0;
            if (was_blank && nblocks > 0 && (blocks[nblocks - 1].kind == BK_LIST || blocks[nblocks - 1].inset)) {
                blocks[nblocks - 1].loose = 1;               /* blank lines between items make the list loose */
                pend_loose = 1;
            }
            pend_task = 0;
            if (item[0] == '[' && (item[1] == ' ' || item[1] == 'x' || item[1] == 'X') && item[2] == ']' && (item[3] == ' ' || !item[3])) {
                pend_task = item[1] == ' ' ? 1 : 2;             /* - [ ] a task */
                item += item[3] ? 4 : 3;
            }
            pend_kind = P_LIST;
            {   /* nest by the item's content column, like CommonMark: 4 spaces under a "- " is one level down, not two */
                int lv, cont = ind + (int)(item - t);
                if (list_ind < 0) lv_n = 0;
                lv = lv_n;
                while (lv > 0 && ind < lv_cont[lv - 1]) {
                    if (ind >= lv_mark[lv - 1]) { lv--; break; }       /* a sibling of that item */
                    lv--;
                }
                if (lv > 5) lv = 5;
                lv_mark[lv] = ind;
                lv_cont[lv] = cont;
                lv_n = lv + 1;
                pend_indent = lv * 2;
                list_ind = cont;
                list_lvl = lv;
            }
            strcpy(pend_num, num);
            pend_len = 0;
            pend[0] = 0;
            append(item);
            continue;
        }
        if (pend_kind == P_LIST) {
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

/* YAML front matter ("---" ... "---" at the very top) becomes a two-column table of key | value, like VS Code's preview;
   indented lines under a key are joined into its value as code. Returns NULL when the file has none. */
static char *front_matter(const char *text)
{
    Buf o;
    const char *p, *end = NULL, *q;
    int rows = 0, open = 0;
    if (strncmp(text, "---", 3)) return NULL;
    for (p = text + 3; *p == ' ' || *p == '\r'; p++) ;
    if (*p != '\n') return NULL;
    for (q = p + 1; *q; ) {                                 /* the closing delimiter line */
        const char *e = strchr(q, '\n');
        if (!strncmp(q, "---", 3) || !strncmp(q, "...", 3)) {
            const char *r = q + 3;
            while (*r == ' ' || *r == '\r') r++;
            if (!*r || *r == '\n') { end = r; break; }
        }
        if (!e) break;
        q = e + 1;
    }
    if (!end) return NULL;
    memset(&o, 0, sizeof(o));
    for (p++; p < q; ) {
        const char *eol = strchr(p, '\n'), *s = p, *c;
        int ll = (int)(eol - p), ind = *p == ' ' || *p == '\t';
        while (ll > 0 && (p[ll - 1] == '\r' || p[ll - 1] == ' ')) ll--;
        p = eol + 1;
        if (!ll) continue;
        if (!ind && (c = (const char *)memchr(s, ':', ll)) != NULL && c > s && (c + 1 == s + ll || c[1] == ' ')) {
            const char *v = c + 1, *ve = s + ll;
            while (v < ve && *v == ' ') v++;
            if (v + 1 < ve && (*v == '"' || *v == '\'') && ve[-1] == *v) { v++; ve--; }
            if (open) { if (o.s[o.n - 1] == ' ') o.n--; buf_add(&o, "` |\n", 4); }
            buf_add(&o, "| **", 4);
            buf_add(&o, s, (int)(c - s));
            buf_add(&o, "** | ", 5);
            for (; v < ve; v++) {
                if (*v == '|') buf_add(&o, "\\", 1);
                buf_add(&o, v, 1);
            }
            open = 0;
            if (c + 1 == s + ll) { buf_add(&o, "`", 1); open = 1; }   /* children follow, shown as code */
            else buf_add(&o, " |\n", 3);
            rows++;
        } else if (rows && ind) {
            const char *ve = s + ll;
            while (s < ve && (*s == ' ' || *s == '\t')) s++;
            if (!open) {                                    /* a value that carried text and then children */
                o.n -= 3;
                buf_add(&o, " `", 2);
                open = 1;
            } else if (o.s[o.n - 1] != '`') {
                buf_add(&o, " ", 1);
            }
            for (; s < ve; s++) {
                if (*s == '|') buf_add(&o, "\\", 1);
                buf_add(&o, s, 1);
            }
            buf_add(&o, " ", 1);
        } else {
            free(o.s);
            return NULL;                                    /* not key: value lines, so not front matter */
        }
    }
    if (open) { if (o.s[o.n - 1] == ' ') o.n--; buf_add(&o, "` |\n", 4); }
    if (!rows) { free(o.s); return NULL; }
    buf_add(&o, "\n", 1);
    buf_add(&o, end, (int)strlen(end));
    buf_add(&o, "", 1);
    return o.s;
}

static void parse_doc(char *text)
{
    char *f = front_matter(text), *c = html_lists(f ? f : text);
    ndet = 0;
    parse_doc_in(c ? c : f ? f : text);
    free(c);
    free(f);
}

/* "mdview /dump in.md out.txt": write the parsed block structure (one token per block) and exit, to diff against another parser */
static void dump_structure(const char *in, const char *out)
{
    FILE *f = fopen(in, "rb"), *o;
    long size;
    char *text;
    int i;

    if (!f) return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char *)malloc(size + 1);
    size = (long)fread(text, 1, size, f);
    text[size] = 0;
    fclose(f);
    free_doc();
    parse_doc(text);
    o = fopen(out, "wb");
    if (o) {
        for (i = 0; i < nblocks; i++) {
            const Block *b = &blocks[i];
            switch (b->kind) {
            case BK_PARA:  fputs("P", o); break;
            case BK_HEAD:  fprintf(o, "H%d", b->level); break;
            case BK_LIST:  fprintf(o, "L%d", b->level); break;
            case BK_QUOTE: fputs("Q", o); break;
            case BK_CODE:  fputs("C", o); break;
            case BK_TABLE: fputs("T", o); break;
            case BK_RULE:  fputs("R", o); break;
            default:       fputs("D", o); break;
            }
            fputc(10, o);
        }
        fclose(o);
    }
    free(text);
}

/* ---- fonts and metrics ---- */

static HWND hwnd;
static HDC mdc;                    /* scratch DC for measuring text */
static int g_dpi = 96, g_zoom = 100;
static int g_nt, g_uni;               /* running on NT; Unicode text APIs usable */

static int PX(int pt) { return MulDiv(pt, g_dpi * g_zoom * 8 / 11, 7200); }
#define CODE_PX PX(12)                  /* the monospace size, for code blocks and inline code */
static int S(int n)   { return MulDiv(n, g_dpi * g_zoom * 8 / 11, 9600); }

typedef struct { int px, b, i, m, u; HFONT h; int asc, ht, spw; int cw[256]; struct { unsigned long cp; int w; } pwc[128]; } FontEnt;
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
    memset(f->pwc, 0, sizeof(f->pwc));
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = b ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = (BYTE)i;
    lf.lfUnderline = (BYTE)(u & 1);
    lf.lfStrikeOut = (BYTE)((u & 2) != 0);
    lf.lfCharSet = ANSI_CHARSET;
    lf.lfQuality = 5;                   /* CLEARTYPE_QUALITY: smooth at every size (older Windows ignores it) */
    lf.lfPitchAndFamily = m ? (FIXED_PITCH | FF_MODERN) : (VARIABLE_PITCH | FF_SWISS);
    strcpy(lf.lfFaceName, m && g_consolas ? "Consolas" : m ? "Courier New" : "Arial");
    f->h = CreateFontIndirect(&lf);
    if (m && g_consolas) {              /* the View option: Consolas where installed (Vista and up), else Courier New */
        char face[LF_FACESIZE];
        SelectObject(mdc, f->h);
        GetTextFace(mdc, LF_FACESIZE, face);
        if (lstrcmpi(face, "Consolas")) {
            DeleteObject(f->h);
            strcpy(lf.lfFaceName, "Courier New");
            f->h = CreateFontIndirect(&lf);
        }
    }
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
/* A symbol is 4 bytes in the text: PH_LEAD, then the code point in three 7-bit groups, each with its top bit set */
#define PH_LEAD 0x1F
static int nph;                         /* symbols seen in this document */

static int ph_unit(const char *s, int len, unsigned long *cp)      /* bytes taken by the char at s; *cp = its code point, or 0 */
{
    *cp = 0;
    if (s[0] == PH_LEAD && len >= 4 && ((unsigned char)s[1] & 0x80) && ((unsigned char)s[2] & 0x80) && ((unsigned char)s[3] & 0x80)) {
        *cp = ((unsigned long)((unsigned char)s[1] & 0x7F) << 14) | ((unsigned long)((unsigned char)s[2] & 0x7F) << 7) |
              ((unsigned char)s[3] & 0x7F);
        return 4;
    }
    return 1;
}

static int ph_enc(unsigned long cp, char *o)
{
    o[0] = PH_LEAD;
    o[1] = (char)(0x80 | ((cp >> 14) & 0x7F));
    o[2] = (char)(0x80 | ((cp >> 7) & 0x7F));
    o[3] = (char)(0x80 | (cp & 0x7F));
    return 4;
}

typedef struct { int px, emoji; HFONT h; } SymFont;
static SymFont symfonts[48];
static int nsym;

/* Win9x has no Unicode text APIs: common symbols are drawn with the Wingdings (face 0) and Symbol (face 1) fonts instead */
typedef struct { unsigned long cp; unsigned char face, ch; } SymMap;
static const SymMap symmap[] = {
    {0x2190,0,0xDF},{0x2192,0,0xE0},{0x2191,0,0xE1},{0x2193,0,0xE2},{0x2196,0,0xE3},{0x2197,0,0xE4},{0x2199,0,0xE5},{0x2198,0,0xE6},
    {0x2B05,0,0xE7},{0x27A1,0,0xE8},{0x2B06,0,0xE9},{0x2B07,0,0xEA},{0x2B09,0,0xEB},{0x2B08,0,0xEC},{0x2B0B,0,0xED},{0x2B0A,0,0xEE},
    {0x21E6,0,0xEF},{0x21E8,0,0xF0},{0x21E7,0,0xF1},{0x21E9,0,0xF2},{0x2194,0,0xF3},{0x21D4,0,0xF3},{0x2195,0,0xF4},
    {0x2713,0,0xFC},{0x2714,0,0xFC},{0x2705,0,0xFE},{0x2611,0,0xFE},{0x2717,0,0xFB},{0x2718,0,0xFB},{0x2716,0,0xFB},{0x274C,0,0xFB},
    {0x2612,0,0xFD},{0x2610,0,0xA8},{0x2B50,0,0xAB},{0x2605,0,0xAB},{0x2606,0,0xB6},{0x272A,0,0xB5},
    {0x263A,0,0x4A},{0x1F600,0,0x4A},{0x1F642,0,0x4A},{0x1F60A,0,0x4A},{0x1F603,0,0x4A},{0x1F604,0,0x4A},{0x1F601,0,0x4A},
    {0x1F610,0,0x4B},{0x2639,0,0x4C},{0x1F641,0,0x4C},{0x2620,0,0x4E},{0x2708,0,0x51},{0x2600,0,0x52},{0x2744,0,0x54},{0x271D,0,0x55},
    {0x260E,0,0x28},{0x2709,0,0x2A},{0x231B,0,0x36},{0x2328,0,0x37},{0x1F4BE,0,0x3C},{0x1F4BB,0,0x3A},{0x2702,0,0x22},{0x270F,0,0x21},
    {0x1F514,0,0x25},{0x1F4D6,0,0x26},{0x1F4C1,0,0x30},{0x1F4C2,0,0x31},{0x1F4C4,0,0x32},{0x270C,0,0x41},{0x1F44D,0,0x43},
    {0x1F44E,0,0x44},{0x1F448,0,0x45},{0x1F449,0,0x46},{0x1F446,0,0x47},{0x1F447,0,0x48},{0x2691,0,0x4F},{0x2318,0,0x7A},
    {0x25CF,0,0x6C},{0x25CB,0,0xA1},{0x25A0,0,0x6E},{0x25A1,0,0x6F},{0x25C6,0,0x75},{0x2756,0,0x76},
    {0x25B6,0,0xD8},{0x25B8,0,0xD8},{0x25BA,0,0xD8},{0x25BC,0,0xDA},{0x25BE,0,0xDA},
    {0x2665,1,0xA9},{0x2764,1,0xA9},{0x2663,1,0xA7},{0x2666,1,0xA8},{0x2660,1,0xAA},{0x221E,1,0xA5},{0x2260,1,0xB9},{0x2264,1,0xA3},
    {0x2265,1,0xB3},{0x2248,1,0xBB},{0x221A,1,0xD6},{0x2211,1,0xE5},{0x222B,1,0xF2},{0x03C0,1,0x70},{0x03B1,1,0x61},{0x03B2,1,0x62},
    {0x03B3,1,0x67},{0x03B4,1,0x64},{0x03BB,1,0x6C},{0x03A9,1,0x57},{0x2202,1,0xB6},{0x2206,1,0x44},{0x00D7,1,0xB4}
};

/* DOS box and block drawing (OEM code page 437, face 2: Courier New) */
static const struct { unsigned short cp; unsigned char ch; } oemmap[] = {
    {0x2500,0xC4},{0x2502,0xB3},{0x250C,0xDA},{0x2510,0xBF},{0x2514,0xC0},{0x2518,0xD9},{0x251C,0xC3},{0x2524,0xB4},
    {0x252C,0xC2},{0x2534,0xC1},{0x253C,0xC5},{0x2550,0xCD},{0x2551,0xBA},{0x2554,0xC9},{0x2557,0xBB},{0x255A,0xC8},
    {0x255D,0xBC},{0x2560,0xCC},{0x2563,0xB9},{0x2566,0xCB},{0x2569,0xCA},{0x256C,0xCE},{0x256D,0xDA},{0x256E,0xBF},
    {0x2570,0xC0},{0x256F,0xD9},{0x2588,0xDB},{0x2584,0xDC},{0x2580,0xDF},{0x258C,0xDD},{0x2590,0xDE},{0x2591,0xB0},
    {0x2592,0xB1},{0x2593,0xB2}
};

/* Ukrainian letters outside the main block, in code page 1251 (face 3: Arial, Cyrillic) */
static const struct { unsigned short cp; unsigned char ch; } cyrmap[] = {
    {0x401,0xA8},{0x451,0xB8},{0x404,0xAA},{0x454,0xBA},{0x406,0xB2},{0x456,0xB3},{0x407,0xAF},{0x457,0xBF},{0x490,0xA5},{0x491,0xB4}
};

static const SymMap *sym_lookup(unsigned long cp)
{
    static SymMap tmp;
    unsigned k;
    for (k = 0; k < sizeof(symmap) / sizeof(symmap[0]); k++)
        if (symmap[k].cp == cp) return &symmap[k];
    tmp.cp = cp;
    for (k = 0; k < sizeof(oemmap) / sizeof(oemmap[0]); k++)
        if (oemmap[k].cp == cp) { tmp.face = 2; tmp.ch = oemmap[k].ch; return &tmp; }
    if (cp >= 0x410 && cp <= 0x44F) { tmp.face = 3; tmp.ch = (unsigned char)(cp - 0x410 + 0xC0); return &tmp; }
    for (k = 0; k < sizeof(cyrmap) / sizeof(cyrmap[0]); k++)
        if (cyrmap[k].cp == cp) { tmp.face = 3; tmp.ch = cyrmap[k].ch; return &tmp; }
    return NULL;
}

static void ph_init(void)
{

    HMODULE g = GetModuleHandle("gdi32.dll");
    pExtTextOutW = (PEXTOUTW)GetProcAddress(g, "ExtTextOutW");
    pGetTextExtentPoint32W = (PTEXTENTW)GetProcAddress(g, "GetTextExtentPoint32W");
    g_uni = g_nt && pExtTextOutW && pGetTextExtentPoint32W;
}


static void ph_reset(void)
{
    nph = 0;
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

static int emoji_color(unsigned long cp);

static HFONT sym_font(int px, unsigned long cp)
{
    int k, emoji = emoji_color(cp);
    LOGFONT lf;
    const SymMap *sm = g_uni ? NULL : sym_lookup(cp);
    if (!emoji && cp >= 0x100 && cp < 0x590) emoji = 4;           /* Latin, Greek, Cyrillic: Arial has them, Segoe UI Symbol does not */
    if (sm) emoji = 10 + sm->face;
    for (k = 0; k < nsym; k++)
        if (symfonts[k].px == px && symfonts[k].emoji == emoji) return symfonts[k].h;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = !sm ? DEFAULT_CHARSET : sm->face == 2 ? OEM_CHARSET : sm->face == 3 ? RUSSIAN_CHARSET : SYMBOL_CHARSET;
    lf.lfQuality = 5;
    strcpy(lf.lfFaceName, sm ? (sm->face == 3 ? "Arial" : sm->face == 2 ? "Courier New" : sm->face ? "Symbol" : "Wingdings") : emoji == 4 ? "Arial" : emoji ? "Segoe UI Emoji" : "Segoe UI Symbol");
    if (nsym == 48) {
        DeleteObject(symfonts[0].h);
        memmove(symfonts, symfonts + 1, 47 * sizeof(SymFont));
        nsym--;
    }
    symfonts[nsym].px = px; symfonts[nsym].emoji = emoji;
    symfonts[nsym].h = CreateFontIndirect(&lf);
    return symfonts[nsym++].h;
}

static int ph_width(int fnt, unsigned long cp)
{
    FontEnt *f = &fonts[fnt];
    int slot = (int)((cp ^ (cp >> 7)) & 127);
    if (f->pwc[slot].cp != cp) {
        wchar_t w[2];
        SIZE sz;
        int n = wide_of(cp, w);
        HGDIOBJ old = SelectObject(mdc, sym_font(f->px, cp));
        if (!g_uni) {
            const SymMap *sm2 = sym_lookup(cp);
            char ch = sm2 ? (char)sm2->ch : '?';
            if (!GetTextExtentPoint32(mdc, &ch, 1, &sz)) sz.cx = f->px;
        } else if (!pGetTextExtentPoint32W(mdc, w, n, &sz)) sz.cx = f->px;
        SelectObject(mdc, old);
        f->pwc[slot].cp = cp;
        f->pwc[slot].w = sz.cx > 0 ? sz.cx : f->px;
    }
    return f->pwc[slot].w;
}

/* text width: the sum of the font's glyph advances (GDI does no kerning), no GDI call per word */
static int text_w(int fnt, const char *s, int len)
{
    const int *cw = fonts[fnt].cw;
    int w = 0;
    while (len > 0) {
        unsigned char c = (unsigned char)*s;
        if (nph && c == PH_LEAD) {
            unsigned long cp;
            int u = ph_unit(s, len, &cp);
            if (cp) {
                w += ph_width(fnt, cp);
                s += u;
                len -= u;
                continue;
            }
        }
        w += cw[c];
        s++;
        len--;
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
typedef struct { int x, y, w, h, bx, by, bw, bh, font, hs, hmax, it0, it1; const Block *blk; } CodeBox;   /* hs: horizontal scroll, hmax: how far it goes */
static CodeBox *cboxes;
static int ncboxes, capcboxes;
static int find_valid, find_visible;                    /* Find: text cache matches the items; panel shown */
static RECT find_rc;
static int as_on, as_x, as_y, as_hbox = -1, as_ax, as_ay;   /* middle-click autoscroll */
static DWORD as_t, as_t0;
static int sb_drag = -1, sb_grab;                        /* a code block's scrollbar thumb being dragged */
static int hot_box = -1, btn_hot, copied_box = -1;      /* box under the mouse, button hovered, box just copied */

/* top of a code block's Copy button in document coordinates: it rides down the block while its top is scrolled off */
static int btn_top(const CodeBox *cb)
{
    int off = cb->by - cb->y, top = g_scroll + off, last = cb->y + cb->h - cb->bh - off;
    if (top > last) top = last;
    return top > cb->by ? top : cb->by;
}

static CodeBox *add_cbox(int x, int y, int w, int h, const Block *blk)
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

typedef struct { const char *s; int len, w, fnt, span, link, sp_before, brk, dy; COLORREF col, bg; } Tok;

static Tok *tokenize(const Inline *in, int px, unsigned base, COLORREF basecol, int maxw, int *count)
{
    Tok *t = NULL;
    int n = 0, cap = 0, k, carry = 0, prev_spw = 0;

    for (k = 0; k < in->n; k++) {
        const Span *sp = &in->s[k];
        unsigned fl = base | sp->flags;
        int code = (fl & F_CODE) != 0;
        int shrink = (fl & (F_SUB | F_SUP)) != 0;
        int fnt = font_get(code ? CODE_PX * px / PX(11) : shrink ? px * 3 / 4 : px, (fl & F_BOLD) != 0, (fl & (F_ITAL | F_IMG)) != 0,
                           code, ((fl & (F_LINK | F_UL)) != 0) | (((fl & F_STRIKE) != 0) << 1));
        int dy = (fl & F_SUP) ? -px / 3 : (fl & F_SUB) ? px / 6 : 0;
        COLORREF col = (fl & F_LINK) ? C_LINK : (fl & F_MARK) ? RGB(0, 0, 0) : code ? C_CODE : (fl & F_IMG) ? C_GRAY : basecol;
        const char *p = sp->text;

        if (sp->flags & F_BREAK) {                /* ends the line */
            Tok *tk;
            if (n == cap) {
                cap = cap ? cap * 2 : 32;
                t = (Tok *)realloc(t, cap * sizeof(Tok));
            }
            tk = &t[n++];
            memset(tk, 0, sizeof(*tk));
            tk->s = ""; tk->fnt = font_get(px, 0, 0, 0, 0); tk->span = k; tk->brk = 1;
            carry = 0;
            continue;
        }
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
                    if (nph) {                                      /* never inside a 4-byte symbol */
                        int q = 0;
                        unsigned long cq;
                        while (q < fit) q += ph_unit(start + q, len - q, &cq);
                        if (q > fit) fit = q - 4 > 0 ? q - 4 : q;
                    }
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
                tk->brk = 0;
                tk->bg = (fl & F_MARK) ? RGB(255, 255, 0) : code ? C_INLINEBG : NOBACK;
                tk->dy = dy;
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
        int wl = w;
        while (j < n) {
            int add = t[j].w + (j > i ? t[j].sp_before : 0);
            if (j > i && lw + add > wl) break;
            lw += add;
            j++;
            if (t[j - 1].brk) break;
        }
        for (k = i; k < j; k++) {
            FontEnt *f = &fonts[t[k].fnt];
            if (f->asc > maxasc) maxasc = f->asc;
            if (f->ht - f->asc > maxdesc) maxdesc = f->ht - f->asc;
        }
        if (emit) {
            int cx = x + (align == 1 ? (wl - lw) / 2 : align == 2 ? (wl - lw) : 0);
            k = i;
            while (k < j) {
                int e = k, fw = t[k].w, flen = t[k].len, fx;
                char *txt;
                FontEnt *f = &fonts[t[k].fnt];
                Item *it;

                if (t[k].brk) { k++; continue; }
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
                    rect(fx - S(2), cy + (maxasc - f->asc) + t[k].dy - 1, fw + S(4), f->ht + 2, t[k].bg);
                it = add_item(IT_TEXT, fx, cy + (maxasc - f->asc) + t[k].dy, fw, f->ht, t[k].col);
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
    int best = 0;
    for (k = 0; k < n; k++) {
        if (t[k].brk) continue;
        if (k == 0 || t[k - 1].brk) total = t[k].w;
        else total += t[k].sp_before + t[k].w;
        if (total > best) best = total;
        if (t[k].w > lg) lg = t[k].w;
    }
    free(t);
    *nat = best;
    *longest = lg;
}

/* ---- block layout ---- */

/* ---- syntax highlighting for fenced code ---- */

enum { HC_PLAIN, HC_KW, HC_CTRL, HC_STR, HC_COM, HC_NUM, HC_TYPE, HC_PRE, HC_ATTR, HC_KEY };
static const int hl_pal[] = { P_BLOCK, P_KW, P_CTRL, P_STR, P_COM, P_NUM, P_TYPE, P_PRE, P_ATTR, P_KEY };
enum { FAM_NONE, FAM_DIFF, FAM_C, FAM_HASH, FAM_SQL, FAM_JSON, FAM_MARKUP };

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
    if (!strcmp(lang, "diff") || !strcmp(lang, "patch")) return FAM_DIFF;
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

    if (fam == FAM_DIFF) {                               /* the whole line by its first characters */
        int c = n && s[0] == '+' && strncmp(s, "+++", 3) ? HC_COM : n && s[0] == '-' && strncmp(s, "---", 3) ? HC_STR :
                (n && s[0] == '@') || !strncmp(s, "+++", 3) || !strncmp(s, "---", 3) ||
                !strncmp(s, "diff ", 5) || !strncmp(s, "index ", 6) ? HC_KW : HC_PLAIN;
        memset(cls, c, n);
        return;
    }
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
                        cls[i] = (isalpha((unsigned char)s[i]) || s[i] == '-' || s[i] == ':') ? HC_ATTR : HC_PLAIN;
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
            if (fam == FAM_JSON) {                          /* a string followed by a colon is an object key */
                for (j = i; j < n && s[j] == ' '; j++) ;
                if (j < n && s[j] == ':') {
                    for (j = i - 1; j >= 0 && cls[j] == HC_STR; j--) cls[j] = HC_KEY;
                }
            }
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
    static const int pts[] = {0, 14, 13, 12, 11, 11, 11};     /* Win95 help: titles are the body size plus a point, in bold */
    return pts[level];
}

static int block_gap(const Block *prev, const Block *b)
{
    if (!prev) return 0;
    if (b->kind == BK_LIST && prev->kind == BK_LIST) {
        if (b->level == 0 && prev->level == 0 && b->numbered != prev->numbered) return S(12);
        return b->loose && prev->loose ? S(10) : S(4);
    }
    if (b->inset || (prev->inset && b->kind == BK_LIST)) return S(8);
    if (prev->kind == BK_HEAD) return S(8);
    if (b->kind == BK_HEAD) return S(b->level <= 2 ? 14 : 10);
    return S(10);
}

static int lay_cell;                    /* laying out inside a table cell: tighter indents */
#define L_BASE (lay_cell ? 0 : S(8))
#define L_STEP (lay_cell ? S(18) : S(26))
#define L_TEXT (lay_cell ? S(18) : S(24))

static int layout_blocks(Block *bl, int nb, int x0, int cwid, int y, unsigned fl, int align);

/* how tall a cell is at width w: laid out for real, then the items are thrown away again */
static int cell_height(const Cell *c, int w, unsigned fl)
{
    int ni = nitems, nc = ncboxes, h;
    h = layout_blocks(c->b, c->n, 0, w, 0, fl, 0);
    nitems = ni;
    ncboxes = nc;
    return h;
}

/* the widest the cell wants to be, and the width of its longest unbreakable word */
static void cell_natural(const Cell *c, unsigned fl, int *nat, int *longest)
{
    int i, n0 = 0, l0 = 0;
    for (i = 0; i < c->n; i++) {
        const Block *b = &c->b[i];
        int n = 0, l = 0, ix = 0;
        switch (b->kind) {
        case BK_HEAD:
            natural_width(&b->inl, PX(head_pt(b->level)), fl | F_BOLD, &n, &l);
            break;
        case BK_LIST:
            natural_width(&b->inl, PX(11), fl, &n, &l);
            ix = L_BASE + b->level * L_STEP + L_TEXT;
            break;
        case BK_QUOTE:
            cell_natural(&b->cells[0], fl, &n, &l);
            ix = S(18);
            break;
        case BK_CODE: {
            int fnt = font_get(CODE_PX, 0, 0, 1, 0), k, cols, maxcols = 0;
            const char *q;
            for (k = 0; k < b->nlines; k++) {
                for (cols = 0, q = b->lines[k]; *q; q++) cols += *q == '\t' ? 4 : 1;
                if (cols > maxcols) maxcols = cols;
            }
            n = maxcols * text_w(fnt, "M", 1) + 2 * S(10);
            l = S(60);
            break;
        }
        case BK_PARA:
            natural_width(&b->inl, PX(11), fl, &n, &l);
            break;
        }
        if (b->inset) ix += L_BASE + (b->inset - 1) * L_STEP + L_TEXT;
        n += ix; l += ix;
        if (n > n0) n0 = n;
        if (l > l0) l0 = l;
    }
    *nat = n0;
    *longest = l0;
}

static void layout_table(Block *b, int x0, int avail, int *py)
{
    int cols = b->cols, rows = b->rows, r, c;
    int padx = S(8), pady = S(3), saved_cell = lay_cell;
    int nat[MAXCOLS] = {0}, mn[MAXCOLS] = {0}, cw[MAXCOLS] = {0};
    int *rh = (int *)malloc((rows + 1) * sizeof(int));
    int sumnat = 0, summin = 0, tw = 0, y = *py, x, it0 = nitems, vis_x = x0, vis_w = avail;

    lay_cell = 1;
    x0 -= S(8);                                     /* the first column's text lines up with the body text */
    avail += 2 * S(8);

    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            int n, l;
            cell_natural(&b->cells[r * cols + c], (b->header && r == 0) ? F_BOLD : 0, &n, &l);
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
            int ch = cell_height(&b->cells[r * cols + c], cw[c] - 2 * padx, (b->header && r == 0) ? F_BOLD : 0);
            if (ch > h) h = ch;
        }
        rh[r] = h + 2 * pady;
    }

    /* like a help file: no grid or shading, just a rule under the header */
    if (b->header) rect(x0 + padx, *py + rh[0] - 1, tw - 2 * padx, 1, C_TEXT);
    for (r = 0, y = *py; r < rows; y += rh[r], r++) {
        for (c = 0, x = x0; c < cols; x += cw[c], c++) {
            const Cell *cell = &b->cells[r * cols + c];
            int first = nitems;
            layout_blocks(cell->b, cell->n, x + padx, cw[c] - 2 * padx, y + pady,
                          (b->header && r == 0) ? F_BOLD : 0, b->align[c]);
            mark_first(first, c == 0 ? 1 : 2);
        }
    }
    if (tw > avail) {                               /* wider than the page: it scrolls sideways like a code block */
        int k, h = y - *py + S(12);
        CodeBox *cb;
        for (k = it0; k < nitems; k++)
            if (!items[k].box) items[k].box = ncboxes + 1;
        cb = add_cbox(vis_x, *py, vis_w, h, b);
        cb->bx = vis_x; cb->by = *py; cb->bw = cb->bh = 0;
        cb->hmax = tw - S(8) - vis_w;
        cb->it0 = it0;
        cb->it1 = nitems;
        y += S(12);
    }
    *py = y;
    free(rh);
    lay_cell = saved_cell;
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

static int layout_blocks(Block *bl, int nb, int x0, int cwid, int y, unsigned fl, int align)
{
    int i;
    const Block *prev = NULL;

    for (i = 0; i < nb; i++) {
        Block *b = &bl[i];
        int start = nitems, ix = 0;
        y += block_gap(prev, b);
        b->y = y;
        if (b->inset) {                                         /* inside a list item */
            ix = L_BASE + (b->inset - 1) * L_STEP + L_TEXT;
            x0 += ix;
            cwid -= ix;
        }

        switch (b->kind) {
        case BK_PARA:
            y += flow(&b->inl, x0, cwid, y, PX(11), fl, C_TEXT, b->halign ? b->halign : align, 1);
            break;
        case BK_HEAD: {
            int h = flow(&b->inl, x0, cwid, y, PX(head_pt(b->level)), fl | F_BOLD, C_HEAD, b->halign ? b->halign : align, 1);
            y += h;
            break;
        }
        case BK_LIST: {
            int px = PX(11);
            int base = L_BASE + b->level * L_STEP;
            int tx = x0 + base + L_TEXT;
            int first_ht = fonts[font_get(px, 0, 0, 0, 0)].ht;
            int h;
            if (b->numbered || b->task) {
                char label[24];
                int n, f = font_get(px, 0, 0, 0, 0), w;
                Item *it;
                if (b->task) {                                  /* a check box instead of the number */
                    n = ph_enc(b->task == 2 ? 0x2611 : 0x2610, label);
                    label[n] = 0;
                    nph = 1;
                } else {
                    n = sprintf(label, "%s.", b->num);
                }
                w = text_w(f, label, n);
                it = add_item(IT_TEXT, tx - S(6) - w, y, w, first_ht, C_GRAY);
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
            h = flow(&b->inl, tx, cwid - (tx - x0), y, px, fl, C_TEXT, 0, 1);
            y += h;
            break;
        }
        case BK_DETAILS: {                                      /* a summary line that opens and closes its content */
            int open = det_open[b->did], h;
            ph_enc(open ? 0x25BC : 0x25B6, b->inl.s[0].text);
            h = flow(&b->inl, x0, cwid, y, PX(11), fl, C_TEXT, 0, 1);
            y += h;
            if (open && b->cells[0].n) y = layout_blocks(b->cells[0].b, b->cells[0].n, x0 + S(18), cwid - S(18), y + S(8), fl, align);
            break;
        }
        case BK_QUOTE: {
            int ye = layout_blocks(b->cells[0].b, b->cells[0].n, x0 + S(18), cwid - S(18), y, fl, align);
            if (ye > y) rect(x0 + S(2), y, S(4), ye - y, C_QBAR);
            y = ye;
            break;
        }
        case BK_CODE: {
            int pad = S(10), fnt = font_get(CODE_PX, 0, 0, 1, 0);
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
                if (fam == FAM_DIFF && len && (exp[0] == '+' || exp[0] == '-') && strncmp(exp, "+++", 3) && strncmp(exp, "---", 3))
                    rect(x0 + 1, ly, cwid - 2, lh, exp[0] == '+' ? pal[P_ADDBG] : pal[P_DELBG]);   /* the whole line, as VS Code does */
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
                CodeBox *cb = add_cbox(x0, y, cwid, h, b);
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
            rect(x0, y + S(4), cwid, 1, C_TEXT);                  /* a thin dark line, as in a help file */
            y += S(8);
            break;
        }
        x0 -= ix;
        cwid += ix;
        mark_first(start, b->kind == BK_LIST ? 1 : 3);
        prev = b;
    }
    return y;
}

static void layout_all(int cw)
{
    int cwid, x0, y;
    DWORD t0 = GetTickCount();

    free_items();
    content_box(cw, &cwid, &x0);
    lay_cwid = cwid; lay_x0 = x0;
    y = layout_blocks(blocks, nblocks, x0, cwid, S(12), 0, 0);
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

static void sb_cell(const Cell *c)
{
    int i;
    for (i = 0; i < c->n; i++) {
        if (i) sb_add(" ");
        sb_inline(&c->b[i].inl);
    }
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
                    sb_cell(&b->cells[r * b->cols + c]);
                }
                sb_add("\r\n");
            }
            sb_add("\r\n");
            break;
        case BK_RULE:
            sb_add("---\r\n\r\n");
            break;
        case BK_DETAILS:
            sb_inline(&b->inl);
            sb_add("\r\n\r\n");
            break;
        case BK_QUOTE: {
            int q;
            for (q = 0; q < b->cells[0].n; q++) {
                sb_add("> ");
                sb_inline(&b->cells[0].b[q].inl);
                sb_add("\r\n");
            }
            sb_add("\r\n");
            break;
        }
        default:
            sb_inline(&b->inl);
            sb_add("\r\n\r\n");
        }
    }
    return sb;
}

/* ---- text decoding: UTF-8 -> Windows-1252 ---- */

/* a Unicode code point as Windows-1252, or as a symbol drawn with the Unicode / Wingdings fonts; returns the bytes written */
static int put_cp(unsigned long cp, char *o)
{
    char *p = o;
    if (cp < 0x80 || (cp >= 0xA0 && cp < 0x100)) { *p++ = (char)cp; return 1; }
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
    default:
        if (cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || cp == 0x200B || (cp >= 0x1F3FB && cp <= 0x1F3FF)) break;
        if ((g_uni || sym_lookup(cp)) && cp >= 0x100) {
            p += ph_enc(cp, p);
            nph = 1;
        } else {
            *p++ = '?';
        }
        break;
    }
    return (int)(p - o);
}

static char *utf8_to_ansi(const unsigned char *s, long n)
{
    char *o = (char *)malloc(n * 2 + 1), *p = o;          /* a 2-byte UTF-8 char can become a 4-byte symbol */
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
        p += put_cp(cp, p);
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

/* ---- colour emoji (Windows 10+): Direct2D / DirectWrite, loaded at run time ---- */

typedef struct { void **vt; } ComObj;
typedef struct { int type, fmt, alpha; float dpix, dpiy; int usage, level; } D2RTProps;
typedef HRESULT (WINAPI *PD2CF)(int, const GUID *, const void *, void **);
typedef HRESULT (WINAPI *PDWCF)(int, const GUID *, void **);
typedef HRESULT (WINAPI *PD2MKRT)(void *, const D2RTProps *, ComObj **);
typedef HRESULT (WINAPI *PD2BRUSH)(void *, const float *, const void *, ComObj **);
typedef HRESULT (WINAPI *PD2BIND)(void *, HDC, const RECT *);
typedef void (WINAPI *PD2BEGIN)(void *);
typedef HRESULT (WINAPI *PD2END)(void *, void *, void *);
typedef void (WINAPI *PD2TEXT)(void *, const wchar_t *, UINT, void *, const float *, void *, UINT, UINT);
typedef void (WINAPI *PD2SETCOL)(void *, const float *);
typedef HRESULT (WINAPI *PDWFMT)(void *, const wchar_t *, void *, int, int, int, float, const wchar_t *, ComObj **);

static ComObj *d2_rt, *d2_brush, *dw_fac;
static int d2_state;                    /* 0 not tried, 1 ready, -1 not available */
static struct { int px; ComObj *fmt; } d2_fmts[16];
static int nd2f;

static void d2_init(void)
{
    static const GUID iid_d2f = {0x06152247, 0x6f50, 0x465a, {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
    static const GUID iid_dwf = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
    HMODULE hd, hw;
    ComObj *fac = NULL;
    D2RTProps pr;
    float black[4] = {0, 0, 0, 1};
    PD2CF mkd2;
    PDWCF mkdw;

    d2_state = -1;
    if (!g_uni || !GetProcAddress(GetModuleHandle("user32.dll"), "GetDpiForSystem")) return;     /* colour glyphs need Windows 10 */
    hd = LoadLibrary("d2d1.dll");
    hw = LoadLibrary("dwrite.dll");
    if (!hd || !hw) return;
    mkd2 = (PD2CF)GetProcAddress(hd, "D2D1CreateFactory");
    mkdw = (PDWCF)GetProcAddress(hw, "DWriteCreateFactory");
    if (!mkd2 || !mkdw) return;
    if (mkd2(0, &iid_d2f, NULL, (void **)&fac) < 0 || !fac) return;
    if (mkdw(0, &iid_dwf, (void **)&dw_fac) < 0 || !dw_fac) return;
    memset(&pr, 0, sizeof(pr));
    pr.fmt = 87;                                    /* DXGI_FORMAT_B8G8R8A8_UNORM */
    pr.alpha = 1;                                   /* premultiplied */
    if (((PD2MKRT)fac->vt[16])(fac, &pr, &d2_rt) < 0 || !d2_rt) return;
    if (((PD2BRUSH)d2_rt->vt[8])(d2_rt, black, NULL, &d2_brush) < 0 || !d2_brush) return;
    d2_state = 1;
}

static ComObj *d2_format(int px)
{
    int k;
    ComObj *f = NULL;
    for (k = 0; k < nd2f; k++)
        if (d2_fmts[k].px == px) return d2_fmts[k].fmt;
    if (nd2f == 16) return NULL;
    if (((PDWFMT)dw_fac->vt[15])(dw_fac, L"Segoe UI Emoji", NULL, 400, 0, 5, (float)px, L"en-us", &f) < 0 || !f) return NULL;
    d2_fmts[nd2f].px = px;
    d2_fmts[nd2f].fmt = f;
    return d2_fmts[nd2f++].fmt;
}

/* the emoji that default to colour (the rest, like the arrows, stay plain text-style glyphs) */
static int emoji_color(unsigned long cp)
{
    return cp >= 0x1F000 || cp == 0x2764 || (cp >= 0x231A && cp <= 0x231B) || (cp >= 0x23E9 && cp <= 0x23F3) ||
           cp == 0x2614 || cp == 0x2615 || (cp >= 0x2648 && cp <= 0x2653) || cp == 0x267F || cp == 0x2693 || cp == 0x26A1 ||
           cp == 0x26AA || cp == 0x26AB || cp == 0x26BD || cp == 0x26BE || cp == 0x26C4 || cp == 0x26C5 || cp == 0x26D4 ||
           cp == 0x26EA || cp == 0x26F2 || cp == 0x26F3 || cp == 0x26F5 || cp == 0x26FA || cp == 0x26FD || cp == 0x2705 ||
           cp == 0x270A || cp == 0x270B || cp == 0x2728 || cp == 0x274C || cp == 0x274E || (cp >= 0x2753 && cp <= 0x2755) ||
           cp == 0x2757 || (cp >= 0x2795 && cp <= 0x2797) || cp == 0x27B0 || cp == 0x27BF || cp == 0x2B1B || cp == 0x2B1C ||
           cp == 0x2B50 || cp == 0x2B55;
}

/* draws one colour emoji with its baseline at y + asc; returns 0 if it could not */
static int d2_draw(HDC dc, int x, int y, int asc, unsigned long cp, int px, int w)
{
    wchar_t ws[2];
    int n = wide_of(cp, ws), top;
    float col[4], rc[4];
    RECT bind;
    COLORREF c = GetTextColor(dc);
    TEXTMETRIC tm;
    HGDIOBJ old;
    ComObj *fmt;

    if (d2_state == 0) d2_init();
    if (d2_state < 0 || !(fmt = d2_format(px))) return 0;
    old = SelectObject(mdc, sym_font(px, cp));
    GetTextMetrics(mdc, &tm);
    SelectObject(mdc, old);
    top = y + asc - tm.tmAscent;
    bind.left = x - 2; bind.top = top - 2; bind.right = x + w + px / 2 + 2; bind.bottom = top + tm.tmHeight + px / 2 + 2;
    if (((PD2BIND)d2_rt->vt[57])(d2_rt, dc, &bind) < 0) return 0;
    col[0] = GetRValue(c) / 255.0f; col[1] = GetGValue(c) / 255.0f; col[2] = GetBValue(c) / 255.0f; col[3] = 1.0f;
    ((PD2SETCOL)d2_brush->vt[8])(d2_brush, col);
    rc[0] = 2; rc[1] = 2; rc[2] = (float)(bind.right - bind.left); rc[3] = (float)(bind.bottom - bind.top);
    ((PD2BEGIN)d2_rt->vt[48])(d2_rt);
    ((PD2TEXT)d2_rt->vt[27])(d2_rt, ws, n, fmt, rc, d2_brush, 4 /* ENABLE_COLOR_FONT */, 0);
    return ((PD2END)d2_rt->vt[49])(d2_rt, NULL, NULL) >= 0;
}

/* text with embedded Unicode characters: plain runs with TextOut, the others with the Unicode symbol fonts */
static void draw_run(HDC dc, int x, int y, const char *s, int len, int fnt)
{
    const char *e = s + len;
    if (!nph) { TextOut(dc, x, y, s, len); return; }
    while (s < e) {
        const char *r = s;
        unsigned long cp = 0;
        int u = 1;
        while (r < e) {
            if (*r == PH_LEAD) {
                u = ph_unit(r, (int)(e - r), &cp);
                if (cp) break;
            }
            r++;
        }
        if (r > s) {
            TextOut(dc, x, y, s, (int)(r - s));
            x += text_w(fnt, s, (int)(r - s));
        }
        if (r < e) {
            int pw = ph_width(fnt, cp);
            if (!(g_uni && emoji_color(cp) && d2_draw(dc, x, y, fonts[fnt].asc, cp, fonts[fnt].px, pw))) {
                wchar_t w[2];
                int n = wide_of(cp, w);
                SelectObject(dc, sym_font(fonts[fnt].px, cp));
                SetTextAlign(dc, TA_BASELINE | TA_LEFT);
                if (g_uni) {
                    pExtTextOutW(dc, x, y + fonts[fnt].asc, 0, NULL, w, n, NULL);
                } else {
                    const SymMap *sm2 = sym_lookup(cp);
                    char ch = sm2 ? (char)sm2->ch : '?';
                    TextOut(dc, x, y + fonts[fnt].asc, &ch, 1);
                }
                SetTextAlign(dc, TA_TOP | TA_LEFT);
                SelectObject(dc, fonts[fnt].h);
            }
            x += pw;
            r += u;
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
        unsigned long fit_k;
        while (fit < it->len) {
            int u = 1;
            unsigned long k;
            if (nph) u = ph_unit(it->text + fit, it->len - fit, &k);
            w2 = w1 + text_w(it->font, it->text + fit, u);
            if (w2 > dx) break;
            w1 = w2;
            fit += u;
        }
        if (fit >= it->len) w2 = w1;
        p.off = (dx - w1 < w2 - dx) ? fit : (fit < it->len ? fit + (w2 > w1 && nph ? ph_unit(it->text + fit, it->len - fit, &fit_k) : 1) : fit);
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
                    int tx = it->x + prefix_w(backdc, it, s0), x0 = tx, x1 = it->x + prefix_w(backdc, it, s1);
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
                    draw_run(backdc, tx, top, it->text + s0, s1 - s0, it->font);
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
        int top = btn_top(cb) - g_scroll, hot = (i == hot_box && btn_hot);
        int done = (i == copied_box);
        RECT r;
        if ((i != hot_box && i != copied_box) || !cb->bw || top > view_h || top + cb->bh < 0) continue;   /* tables have no Copy button */
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
    if (!nph || !g_uni) return 0;
    while (*s) {
        unsigned long cp;
        int u = ph_unit(s, 4, &cp);
        if (cp) return 1;
        s += u;
    }
    return 0;
}

/* the text as UTF-16, with the Unicode characters put back */
static wchar_t *widen(const char *s)
{
    wchar_t *w = (wchar_t *)malloc((strlen(s) * 2 + 2) * sizeof(wchar_t));
    int n = 0;
    while (*s) {
        const char *r = s;
        unsigned long cp = 0;
        int u = 1;
        while (*r) {
            u = ph_unit(r, 4, &cp);
            if (cp) break;
            r += u;
        }
        if (r > s) {
            n += MultiByteToWideChar(CP_ACP, 0, s, (int)(r - s), w + n, (int)(r - s));
            s = r;
        }
        if (*s) {
            n += wide_of(cp, w + n);
            s += u;
        }
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
            char *d = (char *)GlobalLock(g), *o = d;
            memcpy(d, s, strlen(s) + 1);
            while (*d) {                                    /* symbols drawn from Wingdings: no text form */
                unsigned long k;
                int u = ph_unit(d, 4, &k);
                if (k) *o++ = '?';
                else *o++ = d[0];
                d += u;
            }
            *o = 0;
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

static void toggle_details(int id);

static void open_link(const char *url)
{
    char path[MAX_PATH], *slash, *hash;
    DWORD attr;

    if (!*url) return;
    if (!strncmp(url, "mdview:details:", 15)) { toggle_details(atoi(url + 15)); return; }
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
    CheckMenuItem(view_menu, ID_CONSOLAS, MF_BYCOMMAND | (g_consolas ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(view_menu, ID_AUTORELOAD, MF_BYCOMMAND | (g_autoreload ? MF_CHECKED : MF_UNCHECKED));
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

static void toggle_details(int id)
{
    int old = g_scroll;
    if (id < 0 || id >= MAXDET) return;
    det_open[id] = (char)!det_open[id];
    layout_all(view_w);
    set_scroll(old);
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
    if (link > 0 && link <= nlinks && strncmp(links[link - 1], "mdview:", 7)) {
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
    return x >= cboxes[i].bx && x < cboxes[i].bx + cboxes[i].bw && y >= btn_top(&cboxes[i]) && y < btn_top(&cboxes[i]) + cboxes[i].bh;
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
    const Block *b = cboxes[box].blk;
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

static void ui_font_update(void);

static void find_show(void)
{
    int fresh = !find_edit;
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
        find_edit_old = (WNDPROC)SetWindowLong(find_edit, GWL_WNDPROC, (LONG)find_edit_proc);
    }
    if (fresh) ui_font_update();
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

static void toggle_consolas(void)
{
    int old_doc = doc_h > 0 ? doc_h : 1, old_scroll = g_scroll;
    g_consolas = !g_consolas;
    save_setting("Consolas", g_consolas);
    update_view_menu();
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

/* modified time + size of the open file, polled to notice edits (FindFirstFile works on Win95, GetFileAttributesEx doesn't) */
typedef struct { DWORD lo, hi, size; } FileStamp;
static FileStamp loaded_stamp, seen_stamp;

static int file_stamp(const char *path, FileStamp *st)
{
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    st->lo = fd.ftLastWriteTime.dwLowDateTime;
    st->hi = fd.ftLastWriteTime.dwHighDateTime;
    st->size = fd.nFileSizeLow;
    return 1;
}

static int stamp_eq(const FileStamp *a, const FileStamp *b)
{
    return a->lo == b->lo && a->hi == b->hi && a->size == b->size;
}

/* reload = keep the scroll position and stay quiet if the file is momentarily unreadable (an editor mid-save) */
static int load_file_ex(const char *path, int reload)
{
    FILE *f;
    long size;
    unsigned char *raw;
    char *text;
    char title[MAX_PATH + 32];
    const char *base;
    FileStamp st;
    int old_scroll = g_scroll;

    if (!file_stamp(path, &st)) memset(&st, 0, sizeof(st));
    f = fopen(path, "rb");
    if (!f) {
        if (!reload) MessageBox(hwnd, "Can't open that file.", "mdview", MB_OK | MB_ICONEXCLAMATION);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 8 * 1024 * 1024) {
        fclose(f);
        if (!reload) MessageBox(hwnd, "File is too big (8 MB max).", "mdview", MB_OK | MB_ICONEXCLAMATION);
        return 0;
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
    loaded_stamp = seen_stamp = st;
    if (reload) set_scroll(old_scroll);
    return 1;
}

static void load_file(const char *path)
{
    load_file_ex(path, 0);
}

/* polled from a timer: reload once the file has changed and then held still for a poll (debounces editors that save in pieces) */
static void autoreload_poll(void)
{
    FileStamp st;
    if (!g_autoreload || !curfile[0] || !file_stamp(curfile, &st)) return;
    if (stamp_eq(&st, &loaded_stamp)) { seen_stamp = st; return; }
    if (!stamp_eq(&st, &seen_stamp)) { seen_stamp = st; return; }
    load_file_ex(curfile, 1);
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
    {   /* per-monitor aware processes get 96-dpi metrics from SystemParametersInfo, so ask for ours */
        BOOL (WINAPI *spifd)(UINT, UINT, PVOID, UINT, UINT) =
            (BOOL (WINAPI *)(UINT, UINT, PVOID, UINT, UINT))GetProcAddress(GetModuleHandle("user32.dll"), "SystemParametersInfoForDpi");
        if (!spifd || !spifd(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi))
            SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    }
    f = CreateFontIndirect(&ncm.lfMenuFont);
    of = SelectObject(d->um.hdc, f);
    SetBkMode(d->um.hdc, TRANSPARENT);
    SetTextColor(d->um.hdc, RGB(235, 235, 235));
    DrawText(d->um.hdc, buf, -1, &d->dis.rcItem, flags);
    SelectObject(d->um.hdc, of);
    DeleteObject(f);
}

/* UI font for the find bar: the stock GUI font scaled to the current DPI */
static HFONT ui_font;

static void ui_font_update(void)
{
    LOGFONT lf;
    HFONT nf;
    GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    lf.lfHeight = MulDiv(lf.lfHeight, g_dpi, 96);
    lf.lfWidth = 0;
    nf = CreateFontIndirect(&lf);
    if (!nf) return;
    if (find_edit) {
        SendMessage(find_edit, WM_SETFONT, (WPARAM)nf, 0);
        SendMessage(find_count, WM_SETFONT, (WPARAM)nf, 0);
        SendMessage(find_prev, WM_SETFONT, (WPARAM)nf, 0);
        SendMessage(find_next, WM_SETFONT, (WPARAM)nf, 0);
        SendMessage(find_close, WM_SETFONT, (WPARAM)nf, 0);
    }
    if (ui_font) DeleteObject(ui_font);
    ui_font = nf;
}

/* the window moved to a monitor with another DPI (or the scale changed): rescale everything DPI-derived */
static void dpi_changed(int dpi, const RECT *rc)
{
    int old_doc = doc_h > 0 ? doc_h : 1, old_scroll = g_scroll;
    if (dpi < 48) return;
    g_dpi = dpi;
    if (rc) SetWindowPos(hwnd, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    ui_font_update();
    if (tip) SendMessage(tip, MD_TTM_SETMAXTIPWIDTH, 0, MulDiv(520, g_dpi, 96));
    fonts_clear();
    layout_all(view_w);
    g_scroll = (int)((double)old_scroll * doc_h / old_doc);
    set_scroll(g_scroll);
    find_layout();
    InvalidateRect(hwnd, NULL, FALSE);
}

static UINT msh_wheel;      /* the wheel message that Win9x IntelliPoint drivers send ("MSWHEEL_ROLLMSG") */

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_diag) diag_note(h, msg, wp, lp);
    if (msh_wheel && msg == msh_wheel) {         /* wp is the wheel delta, 120 per notch */
        int delta = (short)wp;
        if (GetKeyState(VK_CONTROL) < 0) set_zoom(zoom_now() + (delta > 0 ? 10 : -10));
        else smooth_scroll(scroll_base() - delta * S(100) / 120);
        return 0;
    }
    switch (msg) {
    case 0x02E0:                                /* WM_DPICHANGED */
        dpi_changed(HIWORD(wp), (const RECT *)lp);
        return 0;
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
        if (wp == 7) { autoreload_poll(); return 0; }
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
        else smooth_scroll(scroll_base() - delta * S(100) / 120);
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
        case ID_RELOAD:  if (curfile[0]) load_file_ex(curfile, 1); break;
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
        case ID_CONSOLAS: toggle_consolas(); break;
        case ID_AUTORELOAD: g_autoreload = !g_autoreload; save_setting("AutoReload", g_autoreload); update_view_menu(); break;
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

/* A parent run under a "DPI unaware" compatibility override (Everything, say) passes it down in
   __COMPAT_LAYER, which beats our manifest and gets the window bitmap-stretched. Start once more
   without it; MDVIEW_RELAUNCHED stops a loop if the override is applied some other way. */
static int undpi_relaunch(void)
{
    char layer[256], *env, *e, *blk, *o;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    int n = 0, ok;

    if (GetEnvironmentVariable("MDVIEW_RELAUNCHED", layer, sizeof(layer))) return 0;
    if (!GetEnvironmentVariable("__COMPAT_LAYER", layer, sizeof(layer))) return 0;
    CharUpper(layer);
    if (!strstr(layer, "DPIUNAWARE")) return 0;
    env = GetEnvironmentStrings();
    if (!env) return 0;
    for (e = env; *e; e += strlen(e) + 1) n += (int)strlen(e) + 1;
    blk = (char *)malloc(n + 32);
    if (!blk) { FreeEnvironmentStrings(env); return 0; }
    for (e = env, o = blk; *e; e += strlen(e) + 1) {
        if (!_strnicmp(e, "__COMPAT_LAYER=", 15)) continue;
        strcpy(o, e); o += strlen(e) + 1;
    }
    strcpy(o, "MDVIEW_RELAUNCHED=1"); o += 20;
    *o = 0;
    FreeEnvironmentStrings(env);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    ok = CreateProcess(NULL, GetCommandLine(), NULL, NULL, FALSE, 0, blk, NULL, &si, &pi);
    free(blk);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
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

    if (undpi_relaunch()) return 0;
    dpiaware = GetProcAddress(GetModuleHandle("user32.dll"), "SetProcessDpiAwarenessContext");
    if (dpiaware) ((BOOL (WINAPI *)(HANDLE))dpiaware)((HANDLE)-4);     /* per-monitor v2 (Win10 1703+) */
    else {
        dpiaware = GetProcAddress(GetModuleHandle("user32.dll"), "SetProcessDPIAware");
        if (dpiaware) dpiaware();        /* crisp text on scaled displays (absent on Win9x) */
    }

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
    g_autoreload = load_setting("AutoReload", 1) != 0;
    g_consolas = load_setting("Consolas", 0) != 0;
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
    AppendMenu(view, MF_STRING, ID_CONSOLAS, "Use &Consolas for code");
    AppendMenu(view, MF_STRING, ID_AUTORELOAD, "&Reload when the file changes");
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
    SetTimer(hwnd, 7, 500, NULL);               /* watch the open file for edits */
    {   /* the window may open on a monitor whose DPI differs from the one the screen DC reports */
        UINT (WINAPI *wdpi)(HWND) = (UINT (WINAPI *)(HWND))GetProcAddress(GetModuleHandle("user32.dll"), "GetDpiForWindow");
        if (wdpi && wdpi(hwnd) >= 48) g_dpi = (int)wdpi(hwnd);
    }
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
        if (!strnicmp(w, "/dump ", 6)) {
            char *sp = strchr(w + 6, ' ');
            if (sp) { *sp = 0; dump_structure(w + 6, sp + 1); }
            ExitProcess(0);
        }
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


/* EUBOEA
 * Copyright (C) Krzysztof Palaiologos Szewczyk, 2019.
 * License: MIT
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
 * Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "codegen.h"

/* Initialize DynASM state. */
void dasm_init(Dst_DECL, int maxsection) {
    dasm_State * D;
    size_t psz = 0;
    int i;
    Dst_REF = NULL;
    DASM_M_GROW(Dst, struct dasm_State, Dst_REF, psz, DASM_PSZ(maxsection));
    D = Dst_REF;
    D->psize = psz;
    D->lglabels = NULL;
    D->lgsize = 0;
    D->pclabels = NULL;
    D->pcsize = 0;
    D->globals = NULL;
    D->maxsection = maxsection;
    for (i = 0; i < maxsection; i++) {
        D->sections[i].buf = NULL;  /* Need this for pass3. */
        D->sections[i].rbuf = D->sections[i].buf - DASM_SEC2POS(i);
        D->sections[i].bsize = 0;
        D->sections[i].epos = 0;  /* Wrong, but is recalculated after resize. */
    }
}

/* Free DynASM state. */
void dasm_free(Dst_DECL) {
    dasm_State * D = Dst_REF;
    int i;
    for (i = 0; i < D->maxsection; i++)
        if (D->sections[i].buf)
            DASM_M_FREE(Dst, D->sections[i].buf, D->sections[i].bsize);
    if (D->pclabels) DASM_M_FREE(Dst, D->pclabels, D->pcsize);
    if (D->lglabels) DASM_M_FREE(Dst, D->lglabels, D->lgsize);
    DASM_M_FREE(Dst, D, D->psize);
}

/* Setup global label array. Must be called before dasm_setup(). */
void dasm_setupglobal(Dst_DECL, void ** gl, unsigned int maxgl) {
    dasm_State * D = Dst_REF;
    D->globals = gl - 10;  /* Negative bias to compensate for locals. */
    DASM_M_GROW(Dst, int, D->lglabels, D->lgsize, (10+maxgl)*sizeof(int));
}

/* Grow PC label array. Can be called after dasm_setup(), too. */
void dasm_growpc(Dst_DECL, unsigned int maxpc) {
    dasm_State * D = Dst_REF;
    size_t osz = D->pcsize;
    DASM_M_GROW(Dst, int, D->pclabels, D->pcsize, maxpc*sizeof(int));
    memset((void *)(((unsigned char *)D->pclabels)+osz), 0, D->pcsize-osz);
}

/* Setup encoder. */
void dasm_setup(Dst_DECL, const void * actionlist) {
    dasm_State * D = Dst_REF;
    int i;
    D->actionlist = (dasm_ActList)actionlist;
    D->status = DASM_S_OK;
    D->section = &D->sections[0];
    memset((void *)D->lglabels, 0, D->lgsize);
    if (D->pclabels) memset((void *)D->pclabels, 0, D->pcsize);
    for (i = 0; i < D->maxsection; i++) {
        D->sections[i].pos = DASM_SEC2POS(i);
        D->sections[i].ofs = 0;
    }
}


#ifdef DASM_CHECKS
#define CK(x, st) \
  do { if (!(x)) { \
    D->status = DASM_S_##st|(int)(p-D->actionlist-1); va_end(ap); return; } } while (0)
#define CKPL(kind, st) \
  do { if ((size_t)((char *)pl-(char *)D->kind##labels) >= D->kind##size) { \
    D->status=DASM_S_RANGE_##st|(int)(p-D->actionlist-1); va_end(ap); return; } } while (0)
#else
#define CK(x, st)	((void)0)
#define CKPL(kind, st)	((void)0)
#endif

/* Pass 1: Store actions and args, link branches/labels, estimate offsets. */
void dasm_put(Dst_DECL, int start, ...) {
    va_list ap;
    dasm_State * D = Dst_REF;
    dasm_ActList p = D->actionlist + start;
    dasm_Section * sec = D->section;
    int pos = sec->pos, ofs = sec->ofs, mrm = 4;
    int * b;
    if (pos >= sec->epos) {
        DASM_M_GROW(Dst, int, sec->buf, sec->bsize,
                    sec->bsize + 2*DASM_MAXSECPOS*sizeof(int));
        sec->rbuf = sec->buf - DASM_POS2BIAS(pos);
        sec->epos = (int)sec->bsize/sizeof(int) - DASM_MAXSECPOS+DASM_POS2BIAS(pos);
    }
    b = sec->rbuf;
    b[pos++] = start;
    va_start(ap, start);
    while (1) {
        int action = *p++;
        if (action < DASM_DISP)
            ofs++;

        else if (action <= DASM_REL_A) {
            int n = va_arg(ap, int);
            b[pos++] = n;
            switch (action) {
                case DASM_DISP:
                    if (n == 0) { if ((mrm&7) == 4) mrm = p[-2]; if ((mrm&7) != 5) break; }
                case DASM_IMM_DB: if (((n+128)&-256) == 0) goto ob;
                case DASM_REL_A: /* Assumes ptrdiff_t is int. !x64 */
                case DASM_IMM_D: ofs += 4; break;
                case DASM_IMM_S: CK(((n+128)&-256) == 0, RANGE_I); goto ob;
            case DASM_IMM_B: CK((n&-256) == 0, RANGE_I); ob: ofs++; break;
                case DASM_VREG: CK((n&-8) == 0 && (n != 4 || (*p&1) == 0), RANGE_VREG);
            }
            mrm = 4;
        } else {
            int * pl, n;
            switch (action) {
                case DASM_REL_LG:
                case DASM_IMM_LG:
                    n = *p++; pl = D->lglabels + n;
                    /* Bkwd rel or global. */
                    if (n <= 246) { CK(n>=10||*pl<0, RANGE_LG); CKPL(lg, LG); goto putrel; }
                    pl -= 246; n = *pl;
                    if (n < 0) n = 0;  /* Start new chain for fwd rel if label exists. */
                    goto linkrel;
                case DASM_REL_PC:
                case DASM_IMM_PC: pl = D->pclabels + va_arg(ap, int); CKPL(pc, PC);
                putrel:
                    n = *pl;
                    if (n < 0)    /* Label exists. Get label pos and store it. */
                        b[pos] = -n;

                    else {
                    linkrel:
                        b[pos] = n;  /* Else link to rel chain, anchored at label. */
                        *pl = pos;
                    }
                    pos++;
                    ofs += 4;  /* Maximum offset needed. */
                    if (action == DASM_REL_LG || action == DASM_REL_PC)
                        b[pos++] = ofs;  /* Store pass1 offset estimate. */
                    break;
                case DASM_LABEL_LG: pl = D->lglabels + *p++; CKPL(lg, LG); goto putlabel;
                case DASM_LABEL_PC: pl = D->pclabels + va_arg(ap, int); CKPL(pc, PC);
                putlabel:
                    n = *pl;  /* n > 0: Collapse rel chain and replace with label pos. */
                    while (n > 0) { int * pb = DASM_POS2PTR(D, n); n = *pb; *pb = pos; }
                    *pl = -pos;  /* Label exists now. */
                    b[pos++] = ofs;  /* Store pass1 offset estimate. */
                    break;
                case DASM_ALIGN:
                case DASM_ESC: p++; ofs++; break;
                case DASM_MARK: mrm = p[-2]; break;
                case DASM_SECTION:
                case DASM_STOP: goto stop;
            }
        }
    }
stop:
    va_end(ap);
    sec->pos = pos;
    sec->ofs = ofs;
}
#undef CK

/* Pass 2: Link sections, shrink branches/aligns, fix label offsets. */
int dasm_link(Dst_DECL, size_t * szp) {
    dasm_State * D = Dst_REF;
    int secnum;
    int ofs = 0;
    #ifdef DASM_CHECKS
    *szp = 0;
    if (D->status != DASM_S_OK) return D->status;
    {
        int pc;
        for (pc = 0; pc*sizeof(int) < D->pcsize; pc++)
            if (D->pclabels[pc] > 0) return DASM_S_UNDEF_PC|pc;
    }
    #endif
    {
        /* Handle globals not defined in this translation unit. */
        int idx;
        for (idx = 10; idx*sizeof(int) < D->lgsize; idx++) {
            int n = D->lglabels[idx];
            /* Undefined label: Collapse rel chain and replace with marker (< 0). */
            while (n > 0) { int * pb = DASM_POS2PTR(D, n); n = *pb; *pb = -idx; }
        }
    }
    /* Combine all code sections. No support for data sections (yet). */
    for (secnum = 0; secnum < D->maxsection; secnum++) {
        dasm_Section * sec = D->sections + secnum;
        int * b = sec->rbuf;
        int pos = DASM_SEC2POS(secnum);
        int lastpos = sec->pos;
        while (pos != lastpos) {
            dasm_ActList p = D->actionlist + b[pos++];
            while (1) {
                int op, action = *p++;
                switch (action) {
                    case DASM_REL_LG: p++; op = p[-3]; goto rel_pc;
                case DASM_REL_PC: op = p[-2]; rel_pc: {
                            int shrink = op == 0xe9 ? 3 : ((op&0xf0) == 0x80 ? 4 : 0);
                            if (shrink) {  /* Shrinkable branch opcode? */
                                int lofs, lpos = b[pos];
                                if (lpos < 0) goto noshrink;  /* Ext global? */
                                lofs = *DASM_POS2PTR(D, lpos);
                                if (lpos > pos) {  /* Fwd label: add cumulative section offsets. */
                                    int i;
                                    for (i = secnum; i < DASM_POS2SEC(lpos); i++)
                                        lofs += D->sections[i].ofs;
                                } else {
                                    lofs -= ofs;  /* Bkwd label: unfix offset. */
                                }
                                lofs -= b[pos+1];  /* Short branch ok? */
                                if (lofs >= -128-shrink && lofs <= 127) ofs -= shrink;  /* Yes. */
                            else  noshrink: shrink = 0;    /* No, cannot shrink op. */
                            }
                            b[pos+1] = shrink;
                            pos += 2;
                            break;
                        }
                    case DASM_SPACE: case DASM_IMM_LG: case DASM_VREG: p++;
                    case DASM_DISP: case DASM_IMM_S: case DASM_IMM_B: case DASM_IMM_W:
                    case DASM_IMM_D: case DASM_IMM_WB: case DASM_IMM_DB:
                    case DASM_SETLABEL: case DASM_REL_A: case DASM_IMM_PC: pos++; break;
                    case DASM_LABEL_LG: p++;
                    case DASM_LABEL_PC: b[pos++] += ofs; break; /* Fix label offset. */
                    case DASM_ALIGN: ofs -= (b[pos++]+ofs)&*p++; break; /* Adjust ofs. */
                    case DASM_EXTERN: p += 2; break;
                    case DASM_ESC: p++; break;
                    case DASM_MARK: break;
                    case DASM_SECTION: case DASM_STOP: goto stop;
                }
            }
        stop: (void)0;
        }
        ofs += sec->ofs;  /* Next section starts right after current section. */
    }
    D->codesize = ofs;  /* Total size of all code sections */
    *szp = ofs;
    return DASM_S_OK;
}

#define dasmb(x)	*cp++ = (unsigned char)(x)
#ifndef DASM_ALIGNED_WRITES
#define dasmw(x) \
  do { *((unsigned short *)cp) = (unsigned short)(x); cp+=2; } while (0)
#define dasmd(x) \
  do { *((unsigned int *)cp) = (unsigned int)(x); cp+=4; } while (0)
#else
#define dasmw(x)	do { dasmb(x); dasmb((x)>>8); } while (0)
#define dasmd(x)	do { dasmw(x); dasmw((x)>>16); } while (0)
#endif

/* Pass 3: Encode sections. */
int dasm_encode(Dst_DECL, void * buffer) {
    dasm_State * D = Dst_REF;
    unsigned char * base = (unsigned char *)buffer;
    unsigned char * cp = base;
    int secnum;
    /* Encode all code sections. No support for data sections (yet). */
    for (secnum = 0; secnum < D->maxsection; secnum++) {
        dasm_Section * sec = D->sections + secnum;
        int * b = sec->buf;
        int * endb = sec->rbuf + sec->pos;
        while (b != endb) {
            dasm_ActList p = D->actionlist + *b++;
            unsigned char * mark = NULL;
            while (1) {
                int action = *p++;
                int n = (action >= DASM_DISP && action <= DASM_ALIGN) ? *b++ : 0;
                switch (action) {
                    case DASM_DISP: if (!mark) mark = cp; {
                            unsigned char * mm = mark;
                            if (*p != DASM_IMM_DB && *p != DASM_IMM_WB) mark = NULL;
                            if (n == 0) {
                                int mrm = mm[-1]&7;
                                if (mrm == 4) mrm = mm[0]&7;
                                if (mrm != 5) { mm[-1] -= 0x80; break; }
                            }
                            if (((n+128) & -256) != 0) goto wd;
                            else mm[-1] -= 0x40;
                        }
                case DASM_IMM_S: case DASM_IMM_B: wb: dasmb(n); break;
                    case DASM_IMM_DB: if (((n+128)&-256) == 0) {
                        db:
                            if (!mark) mark = cp; mark[-2] += 2; mark = NULL; goto wb;
                        } else mark = NULL;
                case DASM_IMM_D: wd: dasmd(n); break;
                    case DASM_IMM_WB: if (((n+128)&-256) == 0) goto db;
                        else mark = NULL;
                    case DASM_IMM_W: dasmw(n); break;
                    case DASM_VREG: { int t = *p++; if (t >= 2) n<<=3; cp[-1] |= n; break; }
                    case DASM_REL_LG: p++;
                        if (n >= 0) goto rel_pc;
                        b++; n = (int)(ptrdiff_t)D->globals[-n];
                case DASM_REL_A: rel_a: n -= (int)(ptrdiff_t)(cp+4); goto wd; /* !x64 */
                case DASM_REL_PC: rel_pc: {
                            int shrink = *b++;
                            int * pb = DASM_POS2PTR(D, n);
                            if (*pb < 0) { n = pb[1]; goto rel_a; }
                            n = *pb - ((int)(cp-base) + 4-shrink);
                            if (shrink == 0) goto wd;
                            if (shrink == 4) { cp--; cp[-1] = *cp-0x10; } else cp[-1] = 0xeb;
                            goto wb;
                        }
                    case DASM_IMM_LG:
                        p++;
                        if (n < 0) { n = (int)(ptrdiff_t)D->globals[-n]; goto wd; }
                    case DASM_IMM_PC: {
                            int * pb = DASM_POS2PTR(D, n);
                            n = *pb < 0 ? pb[1] : (*pb + (int)(ptrdiff_t)base);
                            goto wd;
                        }
                    case DASM_LABEL_LG: {
                            int idx = *p++;
                            if (idx >= 10)
                                D->globals[idx] = (void *)(base + (*p == DASM_SETLABEL ? *b : n));
                            break;
                        }
                    case DASM_LABEL_PC: case DASM_SETLABEL: break;
                    case DASM_SPACE: { int fill = *p++; while (n--) *cp++ = fill; break; }
                    case DASM_ALIGN:
                        n = *p++;
                        while (((cp-base) & n)) *cp++ = 0x90; /* nop */
                        break;
                    case DASM_EXTERN: n = DASM_EXTERN(Dst, cp, p[1], *p); p += 2; goto wd;
                    case DASM_MARK: mark = cp; break;
                    case DASM_ESC: action = *p++;
                    default: *cp++ = action; break;
                    case DASM_SECTION: case DASM_STOP: goto stop;
                }
            }
        stop: (void)0;
        }
    }
    if (base + D->codesize != cp)  /* Check for phase errors. */
        return DASM_S_PHASE;
    return DASM_S_OK;
}

int dasm_getpclabel(Dst_DECL, unsigned int pc) {
    dasm_State * D = Dst_REF;
    if (pc*sizeof(int) < D->pcsize) {
        int pos = D->pclabels[pc];
        if (pos < 0)
            return *DASM_POS2PTR(D, -pos);
    }
    return -2;  /* Unused or out of range. */
}

#ifdef DASM_CHECKS
/* Optional sanity checker to call between isolated encoding steps. */
int dasm_checkstep(Dst_DECL, int secmatch) {
    dasm_State * D = Dst_REF;
    if (D->status == DASM_S_OK) {
        int i;
        for (i = 1; i <= 9; i++) {
            if (D->lglabels[i] > 0) { D->status = DASM_S_UNDEF_L|i; break; }
            D->lglabels[i] = 0;
        }
    }
    if (D->status == DASM_S_OK && secmatch >= 0 &&
        D->section != &D->sections[secmatch])
        D->status = DASM_S_MATCH_SEC|(int)(D->section-D->sections);
    return D->status;
}
#endif

/*
 * TSLANG INTERMEDIATE CODE VIRTUAL MACHINE
 *
 * Copyright (C) 2017 Ali Gholami Rudi
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCHRS		(1 << 5)	/* token length */
#define NARGS		(1 << 5)	/* number of arguments */

#define MAX(a, b)	((a) < (b) ? (b) : (a))

/* TSLANG instructions */
struct tsop {
	char op[NCHRS];			/* operation name */
	char args[NARGS][NCHRS];	/* operands */
	int lnum;			/* line number */
};

/* TSLANG labels and procedures */
struct tsloc {
	char name[NCHRS];		/* name */
	long pos;			/* code position */
};

static FILE *fp;		/* program input */
static int fp_lnum = 1;		/* the current line number */
static char *fp_path;		/* the current file */
static char fp_back = -1;	/* the character pushed backed via back() */

static struct tsop *code;	/* program instructions */
static int code_n;		/* the number of instructions */
static int code_sz;		/* the size of code[] */

static struct tsloc *locs;	/* program labels and procedures */
static int locs_n;		/* the number of items in locs[] */
static int locs_sz;		/* the size of locs[] */

static long *regs;		/* machine registers */
static int regs_n;		/* the last machine register accessed */
static int regs_sz;		/* the size of regs[] */
static int regs_off;		/* the offset of accessing regs[] */

/* read the next character from the source program */
static int next(void)
{
	int c = fp_back;
	fp_back = -1;
	if (c >= 0)
		return c;
	c = fgetc(fp);
	if (c == '#')
		while (c != EOF && c != '\n')
			c = fgetc(fp);
	if (c == '\n')
		fp_lnum++;
	return c;
}

/* unget the last character read via next() */
static void back(int c)
{
	fp_back = c;
}

/* skip the characters specified */
static int nextskip(char *skip)
{
	int c = next();
	if (c == EOF)
		return 1;
	while (c != EOF && strchr(skip, c))
		c = next();
	if (c != EOF)
		back(c);
	return 0;
}

/* read the next word */
static int nextword(char *tok, char *skip)
{
	int c = next();
	int i = 0;
	if (c == EOF)
		return -1;
	while (c != EOF && !strchr(skip, c)) {
		if (i < NCHRS - 1)
			tok[i++] = c;
		c = next();
	}
	if (c != EOF)
		back(c);
	tok[i] = '\0';
	return i == 0;
}

/* read the next command */
static int nextcommand(FILE *fp, char *cmd)
{
	if (nextskip(" \t\r\n;"))
		return 1;
	if (nextword(cmd, " \t\r\n;"))
		return 1;
	return 0;
}

/* read the arguments of a command */
static int nextarguments(FILE *fp, char (*args)[NCHRS])
{
	int i;
	if (nextskip(" \t"))
		return 1;
	for (i = 0; i < NARGS; i++) {
		if (nextword(args[i], ", \t\r\n;"))
			break;
		if (nextskip(", \t"))
			break;
	}
	return 0;
}

/* exit because of an unrecoverable error */
static void die(char *fmt, ...)
{
	va_list ap;
	char msg[512];
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	fprintf(stderr, "tsvm:%s:%d: %s\n", fp_path, fp_lnum, msg);
	exit(1);
}

/* extend the given memory allocation */
static void *mextend(void *old, long oldsz, long newsz, int memsz)
{
	void *new = malloc(newsz * memsz);
	if (!new)
		die("failed to allocate %ld bytes", newsz);
	memcpy(new, old, oldsz * memsz);
	memset(new + oldsz * memsz, 0, (newsz - oldsz) * memsz);
	free(old);
	return new;
}

/* read the input source program */
static int readprogram(void)
{
	char cmd[NCHRS];
	while (!nextcommand(fp, cmd)) {
		if (code_n == code_sz) {
			code_sz = MAX(code_sz, 512) * 2;
			code = mextend(code, code_n, code_sz, sizeof(code[0]));
		}
		if (locs_n == locs_sz) {
			locs_sz = MAX(locs_sz, 512) * 2;
			locs = mextend(locs, locs_n, locs_sz, sizeof(locs[0]));
		}
		if (cmd[strlen(cmd) - 1] == ':') {
			locs[locs_n].pos = code_n;
			cmd[strlen(cmd) - 1] = '\0';
			strcpy(locs[locs_n].name, cmd);
			locs_n++;
		} else if (!strcmp("proc", cmd)) {
			locs[locs_n].pos = code_n;
			nextskip(" \t");
			nextword(locs[locs_n].name, " \t\r\n;");
			locs_n++;
		} else {
			code[code_n].lnum = fp_lnum;
			strcpy(code[code_n].op, cmd);
			if (!nextarguments(fp, code[code_n].args))
				code_n++;
		}
	}
	return 0;
}

/* find the position of a label or procedure */
static int locs_find(char *name)
{
	int i;
	for (i = 0; i < locs_n; i++)
		if (!strcmp(locs[i].name, name))
			return locs[i].pos;
	return -1;
}

/* read the identifier of a register; for instance 3 for "r3" */
static int regnum(char *r)
{
	if (r[0] == 'r')
		return atoi(r + 1);
	return -1;
}

/* directly access a register (ignores regs_off) */
static long regget_direct(int r)
{
	while (r >= regs_sz) {
		int sz = MAX(regs_sz, 512) * 2;
		regs = mextend(regs, regs_sz, sz, sizeof(regs[0]));
		regs_sz = sz;
	}
	return regs[r];
}

/* write to a register directly (ignores regs_off) */
static void regset_direct(int r, long val)
{
	regget_direct(r);
	regs[r] = val;
}

/* read the value of the given register in the current frame */
static long regget(int r)
{
	if (r < 0)
		die("bad register identifier");
	r += regs_off;
	regget_direct(r);
	if (r >= regs_n)
		regs_n = r + 1;
	return regs[r];
}

/* set the value of the given register in the current frame */
static void regset(int r, long val)
{
	regget(r);
	regs[r + regs_off] = val;
}

/* tsvm builtin functions */
static int execproc_builtin(char *name)
{
	long val;
	if (!strcmp("iget", name)) {		/* read a word */
		scanf("%ld", &val);
		regset_direct(regs_n, val);
		return 0;
	}
	if (!strcmp("iput", name)) {		/* write a word */
		printf("%ld\n", regget_direct(regs_n));
		return 0;
	}
	if (!strcmp("mem", name)) {		/* allocate memory */
		regset_direct(regs_n, (long) malloc(regget_direct(regs_n)));
		return 0;
	}
	if (!strcmp("rel", name)) {		/* release memory */
		free(malloc(regget_direct(regs_n)));
		return 0;
	}
	return 1;
}

/* execute a procedure */
static int execproc(char *name)
{
	int ip = locs_find(name);
	int r0, r1, r2;
	int dst = 0;
	int regs_off1 = regs_off;
	int i;
	if (ip < 0)
		die("procedure %s not found", name);
	regs_off = regs_n;
	while (ip < code_n) {
		char *op = code[ip].op;
		fp_lnum = code[ip].lnum;
		r0 = regnum(code[ip].args[0]);
		r1 = regnum(code[ip].args[1]);
		r2 = regnum(code[ip].args[2]);
		if (!strcmp("mov", op))
			regset(r0, r1 < 0 ? atoi(code[ip].args[1]) : regget(r1));
		if (!strcmp("add", op))
			regset(r0, regget(r1) + regget(r2));
		if (!strcmp("sub", op))
			regset(r0, regget(r1) - regget(r2));
		if (!strcmp("mul", op))
			regset(r0, regget(r1) * regget(r2));
		if (!strcmp("div", op))
			regset(r0, regget(r1) / regget(r2));
		if (!strcmp("mod", op))
			regset(r0, regget(r1) % regget(r2));
		if (op[0] == 'c' && op[1] == 'm' && op[2] == 'p') {
			char *cmp = op + 3;
			if (!strcmp("<", cmp))
				regset(r0, regget(r1) < regget(r2));
			if (!strcmp(">", cmp))
				regset(r0, regget(r1) > regget(r2));
			if (!strcmp("<=", cmp))
				regset(r0, regget(r1) <= regget(r2));
			if (!strcmp(">=", cmp))
				regset(r0, regget(r1) >= regget(r2));
			if (!strcmp("=", cmp) || !strcmp("==", cmp))
				regset(r0, regget(r1) == regget(r2));
		}
		if (!strcmp("get", op))			/* read from memory */
			regset(r0, *(long *) regget(r1));
		if (!strcmp("put", op))			/* write to memory */
			*(long *) regget(r0) = regget(r1);
		if (!strcmp("ret", op))
			break;
		if (!strcmp("call", op)) {
			for (i = 1; i < NARGS; i++) {
				int r = regnum(code[ip].args[i]);
				if (r >= 0)
					regset_direct(regs_n + i - 1, regget(r));
			}
			if (execproc_builtin(code[ip].args[0]))
				execproc(code[ip].args[0]);
			if (r1 >= 0)
				regset(r1, regget_direct(regs_n));
		}
		if (!strcmp("jz", op) || !strcmp("jnz", op) || !strcmp("jmp", op)) {
			dst = locs_find(code[ip].args[!strcmp("jmp", op) ? 0 : 1]);
			if (dst < 0)
				die("label %s not found", code[ip].args[0]);
		}
		if (!strcmp("jz", op) && regget(r0) == 0) {
			ip = dst;
			continue;
		}
		if (!strcmp("jnz", op) && regget(r0) != 0) {
			ip = dst;
			continue;
		}
		if (!strcmp("jmp", op)) {
			ip = dst;
			continue;
		}
		ip++;
	}
	regs_n = regs_off;
	regs_off = regs_off1;
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;
	if (argc < 2) {
		fprintf(stderr, "usage: %s file.t\n", argv[0]);
		return 1;
	}
	fp_path = argv[1];
	fp = fopen(fp_path, "r");
	if (!fp) {
		fprintf(stderr, "tsvm: cannot open %s\n", fp_path);
		return 1;
	}
	readprogram();
	fclose(fp);
	ret = execproc("main");
	free(code);
	free(locs);
	free(regs);
	return ret;
}

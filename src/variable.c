/*
 * variable.c  変数管理
 *
 * Copyright (C) 1997-1998 Masaki Chikama (Wren) <chikama@kasumi.ipl.mech.nagoya-u.ac.jp>
 *               1998-                           <masaki-c@is.aist-nara.ac.jp>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
*/
/* $Id: variable.c,v 1.15 2002/09/18 13:16:22 chikama Exp $ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "portab.h"
#include "utfsjis.h"
#include "variable.h"
#include "xsystem35.h"

#define SYSVARLONG_MAX 128
#define PAGE_MAX 256

typedef struct {
	int *pointvar;
	int page;
	int offset;
} VariableAttributes;

/* システム変数 */
int sysVar[SYSVAR_MAX];
/* 配列変数の情報 */
static VariableAttributes attributes[SYSVAR_MAX];
/* 配列本体 */
struct VarPage varPage[PAGE_MAX];
/* 64bit変数 */
double longVar[SYSVARLONG_MAX];
/* 文字列変数 */
static char **strVar;
/* 文字列変数の属性(最大,1つあたりの大きさ) */
int strvar_cnt;
int strvar_len;

const char *v_name(int var) {
	if (var < nact->ain.varnum)
		return nact->ain.var[var];
	static char buf[10];
	sprintf(buf, "VAR%d", var);
	return buf;
}

static char *advance(const char *s, int n) {
	while (*s && n > 0) {
		s = advance_char(s, nact->encoding);
		n--;
	}
	return (char *)s;
}

int *v_ref_indexed(int var, int index, struct VarRef *ref) {
	VariableAttributes *attr = &attributes[var];
	int page = attr->page;

	if (page == 0) {
		if (index < 0)
			index = 0;
		// If VAR_n is not an array variable, VAR_n[i] points to VAR_(n+i).
		index += var;
		if (ref) {
			ref->var = var;
			ref->page = page;
			ref->index = index;
		}
		if (index >= SYSVAR_MAX)
			return NULL;
		return sysVar + index;
	}

	if (index < 0)  // Implicit array access
		index = *attr->pointvar;

	index += attr->offset;
	if (ref) {
		ref->var = var;
		ref->page = page;
		ref->index = index;
	}
	if (index >= varPage[page].size)
		return NULL;
	return varPage[page].value + index;
}

// DC command
bool v_allocatePage(int page, int size, bool saveflag) {
	if (page <= 0 || page >= PAGE_MAX) { return false; }
	if (size <= 0 || size > 65536)     { return false; }
	
	void *buf = varPage[page].value;
	if (buf != NULL)
		buf = realloc(buf, size * sizeof(int));
	else
		buf = calloc(size, sizeof(int));
	if (!buf)
		NOMEMERR();

	varPage[page].value    = buf;
	varPage[page].size     = size;
	varPage[page].saveflag = saveflag;
	
	return true;
}

// DS command
bool v_bindArray(int datavar, int *pointvar, int offset, int page) {
	if (datavar <  0 || datavar >= SYSVAR_MAX)         { return false; }
	if (page    <= 0 || page    >= PAGE_MAX)           { return false; }
	if (offset  <  0 || offset  >= varPage[page].size) { return false; }
	
	attributes[datavar].pointvar = pointvar;
	attributes[datavar].page     = page;
	attributes[datavar].offset   = offset;
	return true;
}

// DR command
bool v_unbindArray(int datavar) {
	attributes[datavar].page = 0;
	return true;
}

// DI command
void v_getPageStatus(int page, int *in_use, int *size) {
	if (page == 0) {
		*in_use = false;
		*size = 1;  // why...
	} else if (page >= PAGE_MAX) {
		*in_use = false;
		*size = 0;
	} else {
		*in_use = varPage[page].value != NULL;
		*size = varPage[page].size & 0xffff;
	}
}

/* 文字列変数の再初期化 */
void svar_init(int max_index, int len) {
	for (int i = max_index + 1; i < strvar_cnt; i++) {
		if (strVar[i])
			free(strVar[i]);
	}
	strVar = realloc(strVar, (max_index + 1) * sizeof(char *));
	if (strVar == NULL) {
		NOMEMERR();
	}
	for (int i = strvar_cnt; i <= max_index; i++)
		strVar[i] = NULL;
	strvar_cnt = max_index + 1;
	strvar_len = len;
}

int svar_maxindex(void) {
	return strvar_cnt - 1;
}

/* 変数の初期化 */
void v_init(void) {
	varPage[0].value = sysVar;
	varPage[0].size = SYSVAR_MAX;
	varPage[0].saveflag = true;
	svar_init(STRVAR_MAX - 1, STRVAR_LEN);
}

void v_reset(void) {
	memset(sysVar, 0, sizeof(sysVar));
	memset(attributes, 0, sizeof(attributes));
	memset(longVar, 0, sizeof(longVar));

	for (int i = 1; i < PAGE_MAX; i++) {
		if (varPage[i].value)
			free(varPage[i].value);
	}
	memset(varPage, 0, sizeof(varPage));

	for (int i = 0; i < strvar_cnt; i++) {
		if (strVar[i]) {
			free(strVar[i]);
			strVar[i] = NULL;
		}
	}
	v_init();
}

/* 文字変数への代入 */
void svar_set(int no, const char *str) {
	if ((unsigned)no >= strvar_cnt) {
		WARNING("string index out of range: %d", no);
		return;
	}
	if (strVar[no])
		free(strVar[no]);
	strVar[no] = strdup(str);
}

void svar_copy(int dstno, int dstpos, int srcno, int srcpos, int len) {
	if ((unsigned)dstno >= strvar_cnt) {
		WARNING("string index out of range: %d", dstno);
		return;
	}
	if (!strVar[dstno])
		strVar[dstno] = strdup("");

	char *buf = NULL;
	const char *src;
	if (srcno == dstno)
		src = buf = strdup(strVar[srcno]);
	else
		src = svar_get(srcno);

	dstpos = advance(strVar[dstno], dstpos) - strVar[dstno];  // #chars -> #bytes
	src = advance(src, srcpos);
	len = advance(src, len) - src;  // #chars -> #bytes

	strVar[dstno] = realloc(strVar[dstno], dstpos + len + 1);
	strncpy(strVar[dstno] + dstpos, src, len);
	strVar[dstno][dstpos + len] = '\0';

	if (buf)
		free(buf);
}

/* 文字変数への接続 */
void svar_append(int no, const char *str) {
	if ((unsigned)no >= strvar_cnt) {
		WARNING("string index out of range: %d", no);
		return;
	}
	if (!strVar[no]) {
		strVar[no] = strdup(str);
		return;
	}
	int len1 = strlen(strVar[no]);
	int len2 = strlen(str);
	strVar[no] = realloc(strVar[no], len1 + len2 + 1);
	strcpy(strVar[no] + len1, str);
}

/* 文字変数の長さ */
size_t svar_length(int no) {
	const char *s = svar_get(no);

	int c = 0;
	while (*s) {
		s = advance_char(s, nact->encoding);
		c++;
	}
	return c;
}

/* Width of a string (2 for full-width characters, 1 for half-width) */
int svar_width(int no) {
	return strlen(svar_get(no));
}

/* 文字変数そのもの */
const char *svar_get(int no) {
	if ((unsigned)no >= strvar_cnt) {
		WARNING("string index out of range: %d", no);
		return "";
	}
	return strVar[no] ? strVar[no] : "";
}

int svar_find(int no, int start, const char *str) {
	if ((unsigned)no >= strvar_cnt) {
		WARNING("string index out of range: %d", no);
		return -1;
	}
	if (!*str)
		return 0;
	if (!strVar[no])
		return -1;
	const char *p = advance(strVar[no], start);
	const char *found = strstr(p, str);
	if (!found)
		return -1;
	int n = 0;
	while (p < found) {
		p = advance_char(p, nact->encoding);
		n++;
	}
	return n;
}

void svar_fromVars(int no, const int *vars) {
	if ((unsigned)no >= strvar_cnt) {
		WARNING("string index out of range: %d", no);
		return;
	}
	int len = 0;
	for (const int *c = vars; *c; c++)
		len += (*c < 256) ? 1 : 2;

	if (strVar[no])
		free(strVar[no]);
	char *p = strVar[no] = malloc(len + 1);

	for (const int *v = vars; *v; v++) {
		if (*v < 256) {
			*p++ = *v;
		} else {
			*p++ = *v & 0xff;
			*p++ = *v >> 8;
		}
	}
	*p = '\0';
}

int svar_toVars(int no, int *vars) {
	if ((unsigned)no >= strvar_cnt) {
		WARNING("string index out of range: %d", no);
		return 0;
	}
	if (!strVar[no]) {
		*vars = 0;
		return 1;
	}

	int count = 0;
	for (const char *p = strVar[no]; *p; p++, count++) {
		vars[count] = *p & 0xff;
		if (CHECKSJIS1BYTE(*p) && *(p + 1))
			vars[count] |= (*++p & 0xff) << 8;
	}
	vars[count++] = 0;
	return count;
}

int svar_getCharType(int no, int pos) {
	const char *p = svar_get(no);
	for (; *p && pos > 0; pos--)
		p += CHECKSJIS1BYTE(*p) ? 2 : 1;
	if (!*p)
		return 0;
	return CHECKSJIS1BYTE(*p) ? 2 : 1;
}

void svar_replaceAll(int no, int pattern, int replacement) {
	const char *pat = svar_get(pattern);
	if (!*pat)
		return;
	const char *repl = svar_get(replacement);

	if ((unsigned)no >= strvar_cnt) {
		WARNING("string index out of range: %d", no);
		return;
	}
	if (!strVar[no])
		return;
	char *src = strVar[no];
	strVar[no] = NULL;

	char *start = src, *found;;
	while ((found = strstr(start, pat))) {
		char bak = *found;
		*found = '\0';
		svar_append(no, start);
		*found = bak;
		svar_append(no, repl);
		start = found + strlen(pat);
	}
	svar_append(no, start);
	free(src);
}

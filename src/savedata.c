/*
 * savedate.c  セーブデータの管理
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
/* $Id: savedata.c,v 1.36 2003/07/21 23:06:47 chikama Exp $ */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "portab.h"
#include "savedata.h"
#include "scenario.h"
#include "xsystem35.h"
#include "LittleEndian.h"
#include "filecheck.h"
#include "windowframe.h"
#include "selection.h"
#include "message.h"

/* セーブデータ */
static int savefile_sysvar_cnt = SYSVAR_MAX;

static void* saveStackInfo(Ald_stackHdr *head);
static void  loadStackInfo(char *buf);
static void* saveStrVar(Ald_strVarHdr *head);
static void  loadStrVar(char *buf);
static void* saveSysVar(Ald_sysVarHdr *head, int page);
static int   loadSysVar(char *buf);
static void* loadGameData(int no, int *status, int *size);
static int   saveGameData(int no, char *buf, int size);

#ifdef __EMSCRIPTEN__
EM_JS(void, scheduleSync, (), {
	xsystem35.shell.syncfs();
});
#else
#define scheduleSync()
#endif

/* savefile を参照 */
const char *save_get_file(int index) {
	return nact->files.save_fname[index];
}

/* savefile を削除 */
int save_delete_file(int index) {
	int ret = unlink(nact->files.save_fname[index]);
	
	if (ret == 0) {
		return 1;
	}
	return 1; /* とりあえず */
}

// QE command
int save_vars_to_file(char *fname_utf8, struct VarRef *src, int cnt) {
	// FIXME: System39.exe does not truncate existing file.
	FILE *fp = fc_open(fname_utf8, 'w');
	if (!fp)
		return SAVE_SAVEERR;

	if (cnt > v_sliceSize(src)) {
		WARNING("QE: array size too small (size = %d, data count = %d)", v_sliceSize(src), cnt);
		cnt = v_sliceSize(src);
	}

	int *p = v_resolveRef(src);
	while (cnt--) {
		fputc(*p & 0xff, fp);
		fputc(*p >> 8, fp);
		p++;
	}
	fclose(fp);
	scheduleSync();
	return SAVE_SAVEOK0;
}

// LE command
int load_vars_from_file(char *fname_utf8, struct VarRef *dest, int cnt) {
	FILE *fp = fc_open(fname_utf8, 'r');
	if (!fp)
		return SAVE_LOADERR;

	WORD *tmp = malloc(cnt * sizeof(WORD));
	if (!tmp) {
		WARNING("Out of memory");
		fclose(fp);
		return SAVE_LOADERR;
	}

	size_t size = fread(tmp, sizeof(WORD), cnt, fp);
	fclose(fp);

	if (size != cnt) {
		WARNING("LE: data file too small (requested = %d, loaded = %d)", cnt, size);
		cnt = size;
		// NOTE: System39.exe never returns SAVE_LOADSHORTAGE (254).
	}

	if (cnt > v_sliceSize(dest)) {
		WARNING("LE: array size too small (size = %d, data count = %d)", v_sliceSize(dest), cnt);
		cnt = v_sliceSize(dest);
	}

	int *start = v_resolveRef(dest);
	for (int i = 0; i < cnt; i++)
		start[i] = SDL_SwapLE16(tmp[i]);

	free(tmp);
	return SAVE_LOADOK;
}


/* 指定ファイルへの文字列の書き込み, start = 1~ */
int save_save_str_with_file(char *fname_utf8, int start, int cnt) {
	int status = 0, size, _size,i;
	FILE *fp;
	char *tmp, *_tmp;
	
	_tmp = tmp = malloc(svar_maxindex() * strvar_len);
	if (tmp == NULL) {
		WARNING("Out of memory");
		return SAVE_LOADSHORTAGE;
	}
	
	*tmp = 0;
	for (i = 0; i < cnt; i++) {
		strncpy(tmp, svar_get(start + i), strvar_len - 1);
		tmp[strvar_len - 1] = '\0';
		tmp += strlen(tmp) + 1;
	}
	
	if (NULL == (fp = fc_open(fname_utf8, 'w'))) {
		status = SAVE_SAVEERR; goto errexit;
	}
	size = tmp - _tmp;
	_size = fwrite(_tmp, sizeof(char), size , fp);
	
	if (size != _size) {
		status = SAVE_OTHERERR;
	} else {
		status = SAVE_SAVEOK0;
	}
	
	fclose(fp);
	scheduleSync();
 errexit:      
	free(_tmp);
	
	return status;
}

/* 指定ファイルからの文字列の読み込み */
int save_load_str_with_file(char *fname_utf8, int start, int cnt) {
	int status = 0, size, i;
	FILE *fp;
	char *tmp, *_tmp=NULL;
	long filesize;
	
	if (NULL == (fp = fc_open(fname_utf8, 'r'))) {
		return SAVE_LOADERR;
	}
	
	fseek(fp, 0L, SEEK_END);
	filesize = ftell(fp);
	if (filesize == 0) {
		return SAVE_LOADERR;
	}
	
	tmp = _tmp = (char *)malloc(filesize);
	if (tmp == NULL) {
		WARNING("Out of memory");
		return SAVE_LOADERR;
	}
	
	fseek(fp, 0L, SEEK_SET);
	size = fread(tmp, 1, filesize,fp);
	
	if (size != filesize) {
		status = SAVE_LOADSHORTAGE;
	} else {
		status = SAVE_LOADOK;
	}
	for (i = 0; i < cnt; i++) {
		svar_set(start + i, tmp);
		tmp += strlen(tmp) + 1;
	}
	fclose(fp);
	free(_tmp);
	
	return status;
}

/* セーブファイルのコピー */
int save_copyAll(int dstno, int srcno) {
	char *saveTop;
	int status, filesize;
	
	if (dstno >= SAVE_MAXNUMBER || srcno >= SAVE_MAXNUMBER) {
		return SAVE_SAVEERR;
	}
	saveTop = loadGameData(srcno, &status, &filesize);
	if (saveTop == NULL)
		return SAVE_SAVEERR;
	if (((Ald_baseHdr *)saveTop)->version != SAVE_DATAVERSION) {
		WARNING("endian mismatch");
		free(saveTop);
		return SAVE_SAVEERR;
	}
	status = saveGameData(dstno, saveTop, filesize);
	
	free(saveTop);
	
	return status;
}

/* データの一部ロード */
int save_loadPartial(int no, struct VarRef *vref, int cnt) {
	Ald_baseHdr *save_base;
	char *vtop;
	WORD *tmp;
	int *var;
	char *saveTop = NULL;
	int i, status, filesize;

	if (no >= SAVE_MAXNUMBER) {
		return SAVE_SAVEERR;
	}
	cnt = min(cnt, v_sliceSize(vref));
	var = v_resolveRef(vref);
	
	saveTop = loadGameData(no, &status, &filesize);
	if (saveTop == NULL) {
		return status;
	}
	
	if (filesize <= sizeof(Ald_baseHdr)) {
		goto errexit;
	}

	save_base = (Ald_baseHdr *)saveTop;
	if (save_base->version != SAVE_DATAVERSION) {
		WARNING("endian mismatch");
		goto errexit;
	}

	if (save_base->varSys[vref->page] == 0) {
		goto errexit;
	}
	vtop = saveTop + save_base->varSys[vref->page] + sizeof(Ald_sysVarHdr);
	tmp = (WORD *)vtop + vref->index;
	for (i = 0; i < cnt; i++) {
		*var = *tmp; tmp++; var++;
	}
	free(saveTop);
	return SAVE_LOADOK;
	
 errexit:
	if (saveTop != NULL)
		free(saveTop);
	
	return SAVE_LOADERR;
}

/* データの一部セーブ */
int save_savePartial(int no, struct VarRef *vref, int cnt) {
	Ald_baseHdr *save_base;
	WORD *tmp;
	char *vtop;
	int *var;
	char *saveTop = NULL;
	int i, status, filesize;

	if (no >= SAVE_MAXNUMBER) {
		return SAVE_SAVEERR;
	}
	if (!varPage[vref->page].saveflag)
		goto errexit;
	cnt = min(cnt, v_sliceSize(vref));
	var = v_resolveRef(vref);
	
	saveTop = loadGameData(no, &status, &filesize);
	if (saveTop == NULL)
		return status;
	if (filesize <= sizeof(Ald_baseHdr))
		goto errexit;
	save_base = (Ald_baseHdr *)saveTop;
	if (save_base->version != SAVE_DATAVERSION) {
		WARNING("endian mismatch");
		goto errexit;
	}
	vtop = saveTop + save_base->varSys[vref->page] + sizeof(Ald_sysVarHdr);
	tmp = (WORD *)vtop + vref->index;
	for (i = 0; i < cnt; i++) {
		*tmp = (WORD)*var; tmp++; var++;
	}
	status = saveGameData(no, saveTop, filesize);
	free(saveTop);
	
	return status;
	
 errexit:
	if (saveTop != NULL)
		free(saveTop);
	
	return SAVE_SAVEERR;
}


/* データのロード */
int save_loadAll(int no) {
	Ald_baseHdr *save_base;
	char *saveTop = NULL;
	int i, status, filesize;
	
	if (no >= SAVE_MAXNUMBER) {
		return SAVE_SAVEERR;
	}
	saveTop = loadGameData(no, &status, &filesize);
	if (saveTop == NULL)
		return status;
	if (filesize <= sizeof(Ald_baseHdr))
		goto errexit;
	/* 各種データの反映 */
	save_base = (Ald_baseHdr *)saveTop;
	if (save_base->version != SAVE_DATAVERSION) {
		WARNING("endian mismatch");
		goto errexit;
	}
	if (strcmp(SAVE_DATAID, save_base->ID) != 0)
		goto errexit;
	nact->sel.MsgFontSize        = save_base->selMsgSize;
	nact->sel.MsgFontColor       = save_base->selMsgColor;
	nact->sel.WinBackgroundColor = save_base->selBackColor;
	nact->sel.WinFrameColor      = save_base->selFrameColor;
	nact->msg.MsgFontSize        = save_base->msgMsgSize;
	nact->msg.MsgFontColor       = save_base->msgMsgColor;
	nact->msg.WinBackgroundColor = save_base->msgBackColor;
	nact->msg.WinFrameColor      = save_base->msgFrameColor;
	sl_jmpFar2(save_base->scoPage, save_base->scoIndex);

	for (i = 0; i < SELWINMAX; i++) {
		nact->sel.wininfo[i].x      = save_base->selWinInfo[i].x;
		nact->sel.wininfo[i].y      = save_base->selWinInfo[i].y;
		nact->sel.wininfo[i].width  = save_base->selWinInfo[i].width;
		nact->sel.wininfo[i].height = save_base->selWinInfo[i].height;
		// nact->sel.wininfo[i].save   = TRUE;
	}
	for (i = 0; i < MSGWINMAX; i++) {
		nact->msg.wininfo[i].x      = save_base->msgWinInfo[i].x;
		nact->msg.wininfo[i].y      = save_base->msgWinInfo[i].y;
		nact->msg.wininfo[i].width  = save_base->msgWinInfo[i].width;
		nact->msg.wininfo[i].height = save_base->msgWinInfo[i].height;
		// nact->msg.wininfo[i].savedImage = NULL;
		// nact->msg.wininfo[i].save   = FALSE;
	}
	/* スタックのロード */
	loadStackInfo(saveTop + save_base->stackinfo);
	/* 文字列変数のロード */
	loadStrVar(saveTop + save_base->varStr);
	/* 数値・配列変数のロード */
	for (i = 0; i < 256; i++) {
		if (save_base->varSys[i] != 0) {
			if (SAVE_LOADOK != loadSysVar(saveTop + save_base->varSys[i]))
				goto errexit;
			
		}
	}
	free(saveTop);
	return SAVE_LOADOK;
 errexit:
	free(saveTop);
	return SAVE_LOADERR;
}

/* データのセーブ */
int save_saveAll(int no) {
	Ald_baseHdr   *save_base = calloc(1, sizeof(Ald_baseHdr));
	Ald_strVarHdr save_strHdr;
	Ald_stackHdr  save_stackHdr;
	Ald_sysVarHdr save_sysHdr;
	char *sd_varStr = NULL;
	char *sd_stack  = NULL;
	char *sd_varSys = NULL;
	int i, totalsize = sizeof(Ald_baseHdr);
	FILE *fp = NULL;
	
	if (no >= SAVE_MAXNUMBER)
		goto errexit;
	
	if (save_base == NULL)
		goto errexit;
	
	fc_backup_oldfile(nact->files.save_fname[no]);
	fp = fopen(nact->files.save_fname[no], "wb");
	
	if (fp == NULL)
		goto errexit;
	
	memset(&save_stackHdr, 0, sizeof(Ald_stackHdr));
	memset(&save_strHdr, 0, sizeof(Ald_strVarHdr));
	memset(&save_sysHdr, 0, sizeof(Ald_sysVarHdr));
	
	/* 各種データのセーブ */
	strncpy(save_base->ID, SAVE_DATAID, 32);
	save_base->version       = SAVE_DATAVERSION;
	save_base->selMsgSize    = (BYTE)nact->sel.MsgFontSize;
	save_base->selMsgColor   = (BYTE)nact->sel.MsgFontColor;
	save_base->selBackColor  = (BYTE)nact->sel.WinBackgroundColor;
	save_base->selFrameColor = (BYTE)nact->sel.WinFrameColor;
	save_base->msgMsgSize    = (BYTE)nact->msg.MsgFontSize;
	save_base->msgMsgColor   = (BYTE)nact->msg.MsgFontColor;
	save_base->msgBackColor  = (BYTE)nact->msg.WinBackgroundColor;
	save_base->msgFrameColor = (BYTE)nact->msg.WinFrameColor;
	save_base->scoPage       = sl_getPage();
	save_base->scoIndex      = sl_getIndex();
	
	for (i = 0; i < SELWINMAX; i++) {
		save_base->selWinInfo[i].x      = (WORD)nact->sel.wininfo[i].x;
		save_base->selWinInfo[i].y      = (WORD)nact->sel.wininfo[i].y;
		save_base->selWinInfo[i].width  = (WORD)nact->sel.wininfo[i].width;
		save_base->selWinInfo[i].height = (WORD)nact->sel.wininfo[i].height;
	}
	
	for (i = 0; i < MSGWINMAX; i++) {
		save_base->msgWinInfo[i].x      = (WORD)nact->msg.wininfo[i].x;
		save_base->msgWinInfo[i].y      = (WORD)nact->msg.wininfo[i].y;
		save_base->msgWinInfo[i].width  = (WORD)nact->msg.wininfo[i].width;
		save_base->msgWinInfo[i].height = (WORD)nact->msg.wininfo[i].height;
	}

	fseek(fp, sizeof(Ald_baseHdr), SEEK_SET);
	
	/* スタック情報 */
	if (NULL == (sd_stack = saveStackInfo(&save_stackHdr)))
		goto errexit;

	if (1 != fwrite(&save_stackHdr, sizeof(save_stackHdr), 1, fp))
		goto errexit;
	
	if (save_stackHdr.size != 0 && 1 != fwrite(sd_stack, save_stackHdr.size, 1, fp))
		goto errexit;
	
	save_base->stackinfo  = totalsize;
	totalsize            += save_stackHdr.size + sizeof(Ald_stackHdr);
	
	/* 文字列変数 */
	if (NULL == (sd_varStr = saveStrVar(&save_strHdr)))
		goto errexit;
	
	if (1 != fwrite(&save_strHdr, sizeof(Ald_strVarHdr), 1, fp))
		goto errexit;
	
	if (save_strHdr.size != 0 && 1 != fwrite(sd_varStr, save_strHdr.size, 1, fp))
		goto errexit;
	
	save_base->varStr  = totalsize;
	totalsize         += save_strHdr.size + sizeof(Ald_strVarHdr);
	free(sd_varStr);
	
	/* 数値変数・配列変数 */
	for (i = 0; i < 256; i++) {
		sd_varSys = saveSysVar(&save_sysHdr, i);
		if (sd_varSys == NULL) {
			save_base->varSys[i] = 0;
		} else {
			if (1 != fwrite(&save_sysHdr, sizeof(Ald_sysVarHdr), 1, fp))
				goto errexit;
			if (1 != fwrite(sd_varSys, save_sysHdr.size, 1, fp))
				goto errexit;
			save_base->varSys[i] = totalsize;
			totalsize           += save_sysHdr.size + sizeof(Ald_sysVarHdr);
			free(sd_varSys);
		}
	}
	
	fseek(fp, 0, SEEK_SET);
	
	if (1 != fwrite(save_base, sizeof(Ald_baseHdr), 1, fp))
		goto errexit;
	
	fclose(fp);
	scheduleSync();
	free(save_base);

	return SAVE_SAVEOK1;
	
 errexit:
	if (fp != NULL)
		fclose(fp);
	if (save_base != NULL)
		free(save_base);
	if (sd_varStr != NULL)
		free(sd_varStr);
	if (sd_varSys != NULL)
		free(sd_varSys);
	
	return SAVE_SAVEERR;
}

/* スタック情報のセーブ */
static void *saveStackInfo(Ald_stackHdr *head) {
	int count;
	int *info = sl_getStackInfo(&count);
	
	head->size = count * sizeof(int);
	return (void *)info;
}

/* スタック情報のロード */
static void loadStackInfo(char *buf) {
	Ald_stackHdr *head = (Ald_stackHdr *)buf;
	char         *data = buf + sizeof(Ald_stackHdr);
	sl_putStackInfo((int *)data, head->size / sizeof(int));
}

/* 文字列変数のセーブ */
static void *saveStrVar(Ald_strVarHdr *head) {
	int i;
	char *tmp, *_tmp;
	_tmp = tmp = malloc(svar_maxindex() * strvar_len);
	if (tmp == NULL) {
		WARNING("Out of memory");
		return NULL;
	}
	*tmp = 0;
	// Do not save svar[0], for backward compatibility.
	for (i = 1; i <= svar_maxindex(); i++) {
		strncpy(tmp, svar_get(i), strvar_len - 1);
		tmp[strvar_len - 1] = '\0';
		tmp += strlen(tmp) + 1;
	}
	head->size   = tmp - _tmp;
	head->count  = svar_maxindex();
	head->maxlen = strvar_len;
	return _tmp;
}

/* 文字列変数のロード */
static void loadStrVar(char *buf) {
	Ald_strVarHdr *head = (Ald_strVarHdr *)buf;
	int cnt, max, i;
	
	cnt = head->count;
	max = head->maxlen;
	if (svar_maxindex() != cnt || strvar_len != max) {
		WARNING("Unexpected number of strings in savedata (%d, expected %d)", cnt, svar_maxindex());
		svar_init(cnt, max);
	}
	buf += sizeof(Ald_strVarHdr);
	for (i = 1; i <= cnt; i++) {
		svar_set(i, buf);
		buf += strlen(buf) + 1;
	}
}

/* 数値・配列変数のセーブ */
static void *saveSysVar(Ald_sysVarHdr *head, int page) {
	int *var;
	int cnt, i;
	WORD *tmp, *_tmp;
	if (!varPage[page].saveflag)
		return NULL;
	cnt = varPage[page].size;
	if (page == 0 && savefile_sysvar_cnt < cnt)
		cnt = savefile_sysvar_cnt;
	var = varPage[page].value;
	if (var == NULL)
		return NULL;
	head->size   = cnt * sizeof(WORD);
	head->pageNo = page;
	tmp = _tmp = (WORD *)malloc(cnt * sizeof(WORD));
	if (tmp == NULL) {
		WARNING("Out of memory");
		return NULL;
	}
	for (i = 0; i < cnt; i++) {
		*tmp = (WORD)*var; var++; tmp++;
	}
	return _tmp;
}

/* 数値・配列変数のロード */
static int loadSysVar(char *buf) {
	int i, cnt;
	int  *var;
	WORD *data;
	Ald_sysVarHdr *head = (Ald_sysVarHdr *)buf;
	int page = head->pageNo;

	cnt = head->size / sizeof(WORD);
	if (page == 0)
		savefile_sysvar_cnt = cnt;
	if (varPage[page].size < cnt || varPage[page].value == NULL) {
		if (!v_allocatePage(page, cnt, TRUE)) {
			WARNING("Array allocation failed: page=%d size=%d", page, cnt);
			return SAVE_LOADERR;
		}
	}
	var = varPage[page].value;

	buf += sizeof(Ald_sysVarHdr);
	data = (WORD *)buf;
	for (i = 0; i < cnt; i++) {
		*var++ = *data++;
	}
	return SAVE_LOADOK;
}


/* ゲームデータのロード

 no:    セーブファイル番号 0~ 
 *status:  ステータス
 *size: データの大きさを返すポインタ

あとで free(*buf)するのを忘れないように
*/
static void* loadGameData(int no, int *status, int *size) {
	FILE *fp;
	long filesize;
	char *buf;
	
	fp = fopen(nact->files.save_fname[no], "rb");
	if (fp == NULL)
		goto errexit;
	fseek(fp, 0L, SEEK_END);
	filesize = ftell(fp);
	if (filesize == 0)
		goto errexit;

	buf = (char *)malloc(filesize);
	if (buf == NULL)
		goto errexit;

	fseek(fp, 0L, SEEK_SET);
	fread(buf, filesize, 1, fp);
	fclose(fp);

	*size = (int)filesize;
	*status = SAVE_LOADOK;
	return buf;

 errexit:
	if (fp != NULL)
		fclose(fp);
	*status = SAVE_LOADERR;
	return NULL;
}

static int saveGameData(int no, char *buf, int size) {
	FILE *fp;
	int status = SAVE_SAVEOK1;
	
	fc_backup_oldfile(nact->files.save_fname[no]);
	fp = fopen(nact->files.save_fname[no], "wb");
	if (fp == NULL) {
		return SAVE_SAVEERR;
	}
	if (1 != fwrite(buf, size, 1, fp)) {
		status = SAVE_SAVEERR;
	}
	fclose(fp);
	scheduleSync();
	return status;
}

/* 指定ファイルからの画像の読み込み thanx tajiru@wizard */
BYTE* load_cg_with_file(char *fname_utf8, int *status, long *filesize){
	int size;
	FILE *fp;
	static BYTE *tmp;
	
	*status = 0;
	
	if (NULL == (fp = fc_open(fname_utf8, 'r'))) {
		*status = SAVE_LOADERR; return NULL;
	}
	
	fseek(fp, 0L, SEEK_END);
	*filesize = ftell(fp);
	if (*filesize == 0) {
		*status = SAVE_LOADERR; return NULL;
	}
	
	tmp = (char *)malloc(*filesize);
	if (tmp == NULL) {
		WARNING("Out of memory");
		*status = SAVE_LOADERR; return NULL;
	}
	fseek(fp, 0L, SEEK_SET);
	size = fread(tmp, 1, *filesize,fp);
	
	if (size != *filesize) {
		*status = SAVE_LOADSHORTAGE;
	} else {
		*status = SAVE_LOADOK;
	}
	
	fclose(fp);
	return tmp;
}

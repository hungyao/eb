/*                                                            -*- C -*-
 * Copyright (c) 2003
 *    Motoyuki Kasahara
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * 使用方法:
 *     font <appendix-path> <subbook-index>
 * 例:
 *     font /cdrom 0
 * 説明:
 *     <appendix-path> で指定した appendix から特定の副本を選び、そ
 *     の副本が定義している半角外字の代替文字列をすべて表示します。
 *
 *     その appendix が、半角外字の代替文字列を定義していないと、エ
 *     ラーになります。
 *
 *     <subbook-index> には、操作対象の副本のインデックスを指定しま
 *     す。インデックスは、書籍の最初の副本から順に 0、1、2 ... に
 *     なります。
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <eb/eb.h>
#include <eb/error.h>
#include <eb/appendix.h>

int
main(argc, argv)
    int argc;
    char *argv[];
{
    EB_Error_Code error_code;
    EB_Appendix app;
    EB_Subbook_Code subbook_list[EB_MAX_SUBBOOKS];
    int subbook_count;
    int subbook_index;
    int alt_start;
    char text[EB_MAX_ALTERNATION_TEXT_LENGTH + 1];
    int i;

    /* コマンド行引数をチェック。*/
    if (argc != 3) {
        fprintf(stderr, "Usage: %s appendix-path subbook-index\n",
            argv[0]);
        exit(1);
    }

    /* EB ライブラリと `app' を初期化。*/
    eb_initialize_library();
    eb_initialize_appendix(&app);

    /* appendix を `app' に結び付ける。*/
    error_code = eb_bind_appendix(&app, argv[1]);
    if (error_code != EB_SUCCESS) {
        fprintf(stderr, "%s: failed to bind the app, %s: %s\n",
            argv[0], eb_error_message(error_code), argv[1]);
        goto die;
    }

    /* 副本の一覧を取得。*/
    error_code = eb_appendix_subbook_list(&app, subbook_list,
	&subbook_count);
    if (error_code != EB_SUCCESS) {
        fprintf(stderr, "%s: failed to get the subbook list, %s\n",
            argv[0], eb_error_message(error_code));
        goto die;
    }

    /* 副本のインデックスを取得。*/
    subbook_index = atoi(argv[2]);

    /*「現在の副本 (current subbook)」を設定。*/
    if (eb_set_appendix_subbook(&app, subbook_list[subbook_index])
	< 0) {
        fprintf(stderr, "%s: failed to set the current subbook, %s\n",
            argv[0], eb_error_message(error_code));
        goto die;
    }

    /* 外字の開始位置を取得。*/
    error_code = eb_narrow_alt_start(&app, &alt_start);
    if (error_code != EB_SUCCESS) {
        fprintf(stderr, "%s: failed to get font information, %s\n",
            argv[0], eb_error_message(error_code));
        goto die;
    }

    i = alt_start;
    for (;;) {
        /* 外字の代替文字列を取得。*/
	error_code = eb_narrow_alt_character_text(&app, i, text);
	if (error_code != EB_SUCCESS) {
            fprintf(stderr, "%s: failed to get font data, %s\n",
                argv[0], eb_error_message(error_code));
            goto die;
        }

	/* 取得した代替文字列を出力。*/
	printf("%04x: %s\n", i, text);

        /* 外字の文字番号を一つ進める。*/
	error_code = eb_forward_narrow_alt_character(&app, 1, &i);
	if (error_code != EB_SUCCESS)
	    break;
    }
        
    /* appendix と EB ライブラリの利用を終了。*/
    eb_finalize_appendix(&app);
    eb_finalize_library();
    exit(0);

    /* エラー発生で終了するときの処理。*/
  die:
    eb_finalize_appendix(&app);
    eb_finalize_library();
    exit(1);
}

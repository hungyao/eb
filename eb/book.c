/*
 * Copyright (c) 1997, 98, 99, 2000  Motoyuki Kasahara
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

#include "ebconfig.h"

#include "eb.h"
#include "error.h"
#include "internal.h"
#include "font.h"
#include "language.h"

/*
 * Unexported functions.
 */
static EB_Error_Code eb_initialize_catalog EB_P((EB_Book *));

/*
 * Book ID counter.
 */
static EB_Book_Code counter = 0;

/*
 * Mutex for `counter'.
 */
#ifdef ENABLE_PTHREAD
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/*
 * Initialize `book'.
 */
void
eb_initialize_book(book)
    EB_Book *book;
{
    eb_initialize_lock(&book->lock);
    pthread_mutex_lock(&counter_mutex);
    book->code = counter++;
    pthread_mutex_unlock(&counter_mutex);
    book->path = NULL;
    book->subbook_current = NULL;
    book->subbooks = NULL;
    book->languages = NULL;
    book->language_current = NULL;
    book->messages = NULL;
    book->text_context.unprocessed = NULL;
    book->text_context.unprocessed_size = 0;
}


/*
 * Bind `book' to `path'.
 */
EB_Error_Code
eb_bind(book, path)
    EB_Book *book;
    const char *path;
{
    EB_Error_Code error_code;
    char temporary_path[PATH_MAX + 1];

    /*
     * Lock the book.
     */
    eb_lock(&book->lock);

    /*
     * Clear the book.
     */
    eb_finalize_book(book);
    eb_initialize_book(book);

    /*
     * Set the path of the book.
     * The length of the file name "<path>/subdir/subsubdir/file.ebz;1" must
     * be PATH_MAX maximum.
     */
    if (PATH_MAX < strlen(path)) {
	error_code = EB_ERR_TOO_LONG_FILE_NAME;
	goto failed;
    }
    strcpy(temporary_path, path);
    error_code = eb_canonicalize_path_name(temporary_path);
    if (error_code != EB_SUCCESS)
	goto failed;

    book->path_length = strlen(temporary_path);
    if (PATH_MAX < book->path_length + 1 + EB_MAX_DIRECTORY_NAME_LENGTH + 1
	+ EB_MAX_DIRECTORY_NAME_LENGTH + 1 + EB_MAX_FILE_NAME_LENGTH) {
	error_code = EB_ERR_TOO_LONG_FILE_NAME;
	goto failed;
    }

    book->path = (char *)malloc(book->path_length + 1);
    if (book->path == NULL) {
	error_code = EB_ERR_MEMORY_EXHAUSTED;
	goto failed;
    }
    strcpy(book->path, temporary_path);

    /*
     * Read information from the `LANGUAGE' file.
     * If failed to initialize, JIS X 0208 is assumed.
     */
    if (eb_initialize_languages(book) != EB_SUCCESS)
	book->character_code = EB_CHARCODE_JISX0208;

    /*
     * Read information from the `CATALOG(S)' file.
     */
    error_code = eb_initialize_catalog(book);
    if (error_code != EB_SUCCESS)
	goto failed;
    
    /*
     * Unlock the book.
     */
    eb_unlock(&book->lock);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    eb_finalize_book(book);
    eb_unlock(&book->lock);
    return error_code;
}


/*
 * Suspend using `book'.
 */
void
eb_suspend(book)
    EB_Book *book;
{
    eb_lock(&book->lock);
    eb_unset_subbook(book);
    eb_unset_language(book);
    eb_unlock(&book->lock);
}


/*
 * Finish using `book'.
 */
void
eb_finalize_book(book)
    EB_Book *book;
{
    /*
     * Dispose memories and unset struct members.
     */
    eb_unset_subbook(book);
    eb_unset_language(book);

    if (book->languages != NULL)
	free(book->languages);

    if (book->subbooks != NULL)
	free(book->subbooks);

    if (book->messages != NULL)
	free(book->messages);

    if (book->path != NULL)
	free(book->path);

    if (book->text_context.unprocessed != NULL)
	free(book->text_context.unprocessed);

    book->path = NULL;
    book->subbook_current = NULL;
    book->subbooks = NULL;
    book->languages = NULL;
    book->language_current = NULL;
    book->messages = NULL;
    book->text_context.unprocessed = NULL;
    book->text_context.unprocessed_size = 0;

    eb_finalize_lock(&book->lock);
}


/*
 * There are some books that EB Library sets wrong character code of
 * the book.  They are written in JIS X 0208, but the library sets
 * ISO 8859-1.
 *
 * We fix the character of the books.  The following table lists
 * titles of the first subbook in those books.
 */
static const char * const misleaded_book_table[] = {
    /* SONY DataDiskMan (DD-DR1) accessories. */
    "%;%s%A%e%j!\\%S%8%M%9!\\%/%i%&%s",

    /* Shin Eiwa Waei Chujiten (earliest edition) */
    "8&5f<R!!?71QOBCf<-E5",

    /* EB Kagakugijutsu Yougo Daijiten (YRRS-048) */
    "#E#B2J3X5;=QMQ8lBg<-E5",
    NULL
};

/*
 * Read information from the `CATALOG(S)' file in 'book'.
 * Return EB_SUCCESS if it succeeds, error-code otherwise.
 */
static EB_Error_Code
eb_initialize_catalog(book)
    EB_Book *book;
{
    EB_Error_Code error_code;
    char buffer[EB_SIZE_PAGE];
    char catalog_path_name[PATH_MAX + 1];
    char *space;
    EB_Subbook *subbook;
    size_t catalog_size;
    size_t title_size;
    const char * const *misleaded;
    int file = -1;
    int i;

    /*
     * Find a catalog file.
     */
    if (eb_compose_path_name(book->path, EB_FILE_NAME_CATALOG, EB_SUFFIX_NONE,
	catalog_path_name) == 0) {
	book->disc_code = EB_DISC_EB;
	catalog_size = EB_SIZE_EB_CATALOG;
	title_size = EB_MAX_EB_TITLE_LENGTH;
    } else if (eb_compose_path_name(book->path, EB_FILE_NAME_CATALOGS,
	EB_SUFFIX_NONE, catalog_path_name) == 0) {
	book->disc_code = EB_DISC_EPWING;
	catalog_size = EB_SIZE_EPWING_CATALOG;
	title_size = EB_MAX_EPWING_TITLE_LENGTH;
    } else {
	error_code = EB_ERR_FAIL_OPEN_CAT;
	goto failed;
    }

    /*
     * Open a catalog file.
     */
    file = open(catalog_path_name, O_RDONLY | O_BINARY);
    if (file < 0) {
	error_code = EB_ERR_FAIL_OPEN_CAT;
	goto failed;
    }

    /*
     * Get the number of subbooks in this book.
     */
    if (eb_read_all(file, buffer, 16) != 16) {
	error_code = EB_ERR_FAIL_READ_CAT;
	goto failed;
    }
    book->subbook_count = eb_uint2(buffer);
    if (EB_MAX_SUBBOOKS < book->subbook_count)
	book->subbook_count = EB_MAX_SUBBOOKS;
    if (book->subbook_count == 0) {
	error_code = EB_ERR_UNEXP_CAT;
	goto failed;
    }

    /*
     * Get EPWING format version.
     */
    if (book->disc_code == EB_DISC_EPWING)
	book->version = eb_uint1(buffer + 3);

    /*
     * Allocate memories for subbook entries.
     */
    book->subbooks = (EB_Subbook *) malloc(sizeof(EB_Subbook)
	* book->subbook_count);
    if (book->subbooks == NULL) {
	error_code = EB_ERR_MEMORY_EXHAUSTED;
	goto failed;
    }

    /*
     * Read information about subbook.
     */
    for (i = 0, subbook = book->subbooks; i < book->subbook_count;
	 i++, subbook++) {
	/*
	 * Read data from the catalog file.
	 */
	if (eb_read_all(file, buffer, catalog_size) != catalog_size) {
	    error_code = EB_ERR_FAIL_READ_CAT;
	    goto failed;
	}

	/*
	 * Set a directory name.
	 */
	strncpy(subbook->directory_name, buffer + 2 + title_size,
	    EB_MAX_DIRECTORY_NAME_LENGTH);
	subbook->directory_name[EB_MAX_DIRECTORY_NAME_LENGTH] = '\0';
	space = strchr(subbook->directory_name, ' ');
	if (space != NULL)
	    *space = '\0';
	eb_fix_directory_name(book->path, subbook->directory_name);

	/*
	 * Set an index page.
	 */
	if (book->disc_code == EB_DISC_EB)
	    subbook->index_page = 1;
	else {
	    subbook->index_page = eb_uint2(buffer + 2
		+ EB_MAX_EPWING_TITLE_LENGTH + EB_MAX_DIRECTORY_NAME_LENGTH
		+ 4);
	}

	/*
	 * Set a title.  (Convert from JISX0208 to EUC JP)
	 */
	strncpy(subbook->title, buffer + 2, title_size);
	subbook->title[title_size] = '\0';
	if (book->character_code != EB_CHARCODE_ISO8859_1)
	    eb_jisx0208_to_euc(subbook->title, subbook->title);

	/*
	 * Initialize font availability information.
	 */
	subbook->narrow_fonts[EB_FONT_16].font_code = EB_FONT_INVALID;
	subbook->narrow_fonts[EB_FONT_24].font_code = EB_FONT_INVALID;
	subbook->narrow_fonts[EB_FONT_30].font_code = EB_FONT_INVALID;
	subbook->narrow_fonts[EB_FONT_48].font_code = EB_FONT_INVALID;
	subbook->wide_fonts[EB_FONT_16].font_code = EB_FONT_INVALID;
	subbook->wide_fonts[EB_FONT_24].font_code = EB_FONT_INVALID;
	subbook->wide_fonts[EB_FONT_30].font_code = EB_FONT_INVALID;
	subbook->wide_fonts[EB_FONT_48].font_code = EB_FONT_INVALID;

	/*
	 * If the book is EPWING, get font file names.
	 */
	if (book->disc_code == EB_DISC_EPWING) {
	    EB_Font *font;
	    char *buffer_p;
	    int j;

	    /*
	     * Narrow font file names.
	     */
	    buffer_p = buffer + 2 + title_size + 50;
	    for (font = subbook->narrow_fonts, j = 0; j < EB_MAX_FONTS;
		 j++, font++) {
		/*
		 * Skip this entry if the first character of the file name
		 * is not valid.
		 */
		if (*buffer_p == '\0' || 0x80 <= *((unsigned char *)buffer_p))
		    continue;
		strncpy(font->file_name, buffer_p,
		    EB_MAX_DIRECTORY_NAME_LENGTH);
		font->file_name[EB_MAX_DIRECTORY_NAME_LENGTH] = '\0';
		font->font_code = j;
		font->page = 1;
		space = strchr(font->file_name, ' ');
		if (space != NULL)
		    *space = '\0';
		buffer_p += EB_MAX_DIRECTORY_NAME_LENGTH;
	    }

	    /*
	     * Wide font file names.
	     */
	    buffer_p = buffer + 2 + title_size + 18;
	    for (font = subbook->wide_fonts, j = 0; j < EB_MAX_FONTS;
		 j++, font++) {
		/*
		 * Skip this entry if the first character of the file name
		 * is not valid.
		 */
		if (*buffer_p == '\0' || 0x80 <= *((unsigned char *)buffer_p))
		    continue;
		strncpy(font->file_name, buffer_p,
		    EB_MAX_DIRECTORY_NAME_LENGTH);
		font->file_name[EB_MAX_DIRECTORY_NAME_LENGTH] = '\0';
		font->font_code = j;
		font->page = 1;
		space = strchr(font->file_name, ' ');
		if (space != NULL)
		    *space = '\0';
		buffer_p += EB_MAX_DIRECTORY_NAME_LENGTH;
	    }
	}

	subbook->initialized = 0;
	subbook->code = i;
    }

    /*
     * Close the catalog file.
     */
    close(file);

    /*
     * Fix chachacter-code of the book.
     */
    for (misleaded = misleaded_book_table; *misleaded != NULL; misleaded++) {
	if (strcmp(book->subbooks->title, *misleaded) == 0) {
	    book->character_code = EB_CHARCODE_JISX0208;
	    for (i = 0, subbook = book->subbooks; i < book->subbook_count;
		 i++, subbook++) {
		eb_jisx0208_to_euc(subbook->title, subbook->title);
	    }
	    break;
	}
    }

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    if (0 <= file)
	close(file);
    if (book->subbooks != NULL) {
	free(book->subbooks);
	book->subbooks = NULL;
    }
    return error_code;
}


/*
 * Test whether `book' is bound.
 */
int
eb_is_bound(book)
    EB_Book *book;
{
    /*
     * Lock the book.
     */
    eb_lock(&book->lock);

    /*
     * Check for the current status.
     */
    if (book->path == NULL)
	goto failed;

    /*
     * Unlock the book.
     */
    eb_unlock(&book->lock);

    return 1;

    /*
     * An error occurs...
     */
  failed:
    eb_unlock(&book->lock);
    return 0;
}


/*
 * Return the bound path of `book'.
 */
EB_Error_Code
eb_path(book, path)
    EB_Book *book;
    char *path;
{
    EB_Error_Code error_code;

    /*
     * Lock the book.
     */
    eb_lock(&book->lock);

    /*
     * Check for the current status.
     */
    if (book->path == NULL) {
	error_code = EB_ERR_UNBOUND_BOOK;
	goto failed;
    }

    /*
     * Copy the path to `path'.
     */
    strcpy(path, book->path);

    /*
     * Unlock the book.
     */
    eb_unlock(&book->lock);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *path = '\0';
    eb_unlock(&book->lock);
    return error_code;
}


/*
 * Inspect a disc type.
 */
EB_Error_Code
eb_disc_type(book, disc_code)
    EB_Book *book;
    EB_Disc_Code *disc_code;
{
    EB_Error_Code error_code;

    /*
     * Lock the book.
     */
    eb_lock(&book->lock);

    /*
     * Check for the current status.
     */
    if (book->path == NULL) {
	error_code = EB_ERR_UNBOUND_BOOK;
	goto failed;
    }

    /*
     * Copy the disc code to `disc_code'.
     */
    *disc_code = book->disc_code;

    /*
     * Unlock the book.
     */
    eb_unlock(&book->lock);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *disc_code = EB_DISC_INVALID;
    eb_unlock(&book->lock);
    return error_code;
}


/*
 * Inspect a character code used in the book.
 */
EB_Error_Code
eb_character_code(book, character_code)
    EB_Book *book;
    EB_Character_Code *character_code;
{
    EB_Error_Code error_code;

    /*
     * Lock the book.
     */
    eb_lock(&book->lock);

    /*
     * Check for the current status.
     */
    if (book->path == NULL) {
	error_code = EB_ERR_UNBOUND_BOOK;
	goto failed;
    }

    /*
     * Copy the character code to `character_code'.
     */
    *character_code = book->character_code;

    /*
     * Unlock the book.
     */
    eb_unlock(&book->lock);

    return EB_SUCCESS;

    /*
     * An error occurs...
     */
  failed:
    *character_code = EB_CHARCODE_INVALID;
    eb_unlock(&book->lock);
    return error_code;
}


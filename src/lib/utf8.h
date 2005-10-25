/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup lib
 * @file
 *
 * Unicode Transformation Format 8 bits.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#ifndef _utf8_h_
#define _utf8_h_

#include <glib.h>


#if 0  /* xxxUSE_ICU */
#include "unicode/uchar.h"
#include "unicode/ustring.h"
#include "unicode/utypes.h"
#include "unicode/ustdio.h"
#include "unicode/unorm.h"
#endif

typedef enum {
	UNI_NORM_NFC = 0,
	UNI_NORM_NFKC,
	UNI_NORM_NFD,
	UNI_NORM_NFKD,

	NUM_UNI_NORM
} uni_norm_t;

/*
 * Gtk+ renderers want UTF-8 NFC
 */
#define UNI_NORM_GUI UNI_NORM_NFC

/*
 * NFC is more dense than NFD, thus it's the normalization of choice when
 * passing text over the wire.
 */
#define UNI_NORM_NETWORK UNI_NORM_NFC

#if defined(__APPLE__) && defined(__MACH__) /* Darwin */
/* Mac OS X (Darwin) wants filenames always in UTF-8 NFD */
#define UNI_NORM_FILESYSTEM UNI_NORM_NFD
#else /* !Darwin */
/* Unix systems usually use NFC for UTF-8 filenames */
#define UNI_NORM_FILESYSTEM UNI_NORM_NFC
#endif /* Darwin */

void locale_init(void);
void locale_close(void);
const gchar *locale_get_charset(void);
gint utf8_is_valid_char(const gchar *s);
gboolean is_ascii_string(const gchar *str);
size_t utf8_is_valid_string(const gchar *s, size_t len);
size_t utf8_strlcpy(gchar *dst, const gchar *src, size_t dst_size);
guint32 utf8_decode_char(const gchar *s, gint len, gint *retlen, gboolean warn);
gint utf8_to_iso8859(gchar *s, gint len, gboolean space);
size_t utf8_strlower(gchar *dst, const gchar *src, size_t size);
gchar *utf8_strlower_copy(const gchar *src);
size_t utf8_strupper(gchar *dst, const gchar *src, size_t size);
gchar *utf8_strupper_copy(const gchar *src);
gchar *utf8_canonize(const gchar *src);
gchar *utf8_normalize(const gchar *src, uni_norm_t norm);

size_t utf32_to_utf8(const guint32 *in, gchar *out, size_t size);
guint32 utf32_lowercase(guint32 uc);

/**
 * This is a highly specialized function (read: don't use it if you don't
 * understand what it does and how it's used) to be used with
 * utf8_decode_char().
 * It's purpose is to determine the maximum possible length in bytes of
 * current UTF-8 character that ``s'' points to.
 *
 * @param s a UTF-8 encoded string.
 * @param len number of bytes pending to be decoded.
 *
 * @returns the maximum length in bytes of the current UTF-8 character.
 */
static inline size_t
utf8_decode_lookahead(const gchar *s, size_t len)
{
	while (len < 6 && s[len] != '\0')
		len++;
	return len;
}

/**
 * Encodes a single UTF-32 character as UTF-16 and return the result
 * compacted into a 32-bit integer.
 * See also RFC 2781.
 *
 * @param uc the unicode character to encode.
 * @returns 0 if the unicode character is invalid. Otherwise the
 *         	UTF-16 encoded character is returned in a compact form:
 *			The lower 16 bits are the first UTF-16 character, the
 *			upper 16 bits are the second one. If the upper bits are
 *			all zero, the unicode character fit into 16 bits.
 */
static inline guint32
utf16_encode_char_compact(guint32 uc)
{
	if (uc <= 0xFFFF) {
		return uc;
	} else if (uc <= 0x10FFFF) {
		guint16 w1, w2;

		uc -= 0x10000;
		w1 = (uc >> 10) | 0xd800;
		w2 = (uc & 0x3ff) | 0xdc00;
		return (w2 << 16) | w1;
	}
	return 0;
}

/**
 * Lazy converters either return a pointer to a static buffer or manage
 * the allocated memory themselves. They may also return the original
 * pointer. Copy the result before calling them again unless you don't
 * need the previous result anymore.
 */
const gchar *lazy_iso8859_1_to_utf8(const gchar *src);
const gchar *lazy_locale_to_utf8(const gchar *src);
const gchar *lazy_ui_string_to_utf8(const gchar *src);
const gchar *lazy_utf8_to_locale(const gchar *src);
const gchar *lazy_locale_to_utf8_normalized(const gchar *src, uni_norm_t norm);

static inline const gchar *
lazy_vendor_to_utf8(const gchar *s)
{
	return !s || is_ascii_string(s) ? s : lazy_iso8859_1_to_utf8(s);
}

static inline const gchar *
lazy_vendor_to_locale(const gchar *s)
{
	return s ? lazy_utf8_to_locale(lazy_vendor_to_utf8(s)) : NULL;
}

gchar *locale_to_utf8(const gchar *str);
gchar *unknown_to_utf8(const gchar *str);
gchar *locale_to_utf8_normalized(const gchar *str, uni_norm_t norm);
gchar *filename_to_utf8_normalized(const gchar *str, uni_norm_t norm);
gchar *iso8859_1_to_utf8_normalized(const gchar *str, uni_norm_t norm);
gchar *unknown_to_utf8_normalized(const gchar *src, uni_norm_t norm);

gchar *utf8_to_filename(const gchar *s);
gchar *utf8_to_locale(const gchar *s);

static inline gchar *
utf8_or_locale_normalize(const gchar *s, uni_norm_t norm)
{
	g_assert(s);

	return '\0' == s[0] || utf8_is_valid_string(s, 0)
		? utf8_normalize(s, norm)
		: locale_to_utf8_normalized(s, norm);
}


gboolean icu_enabled(void);
gboolean locale_is_latin(void);
gboolean locale_is_utf8(void);

#if 0  /* xxxUSE_ICU */

#define UNICODE_CANONIZE(x) \
	(icu_enabled() ? unicode_canonize(x) : utf8_canonize(x))

int locale_to_icu_conv(const gchar *in, int lenin, UChar *out, int lenout);
int utf8_to_icu_conv(const gchar *in, int lenin, UChar *out, int lenout);
int icu_to_utf8_conv(const UChar *in, int lenin, gchar *out, int lenout);

int unicode_NFC(const UChar *source, gint32 len, UChar *result, gint32 rlen);
int unicode_NFKD(const UChar *source, gint32 len, UChar *result, gint32 rlen);
int unicode_lower(const UChar *source, gint32 len, UChar *result, gint32 rlen);
int unicode_upper(const UChar *source, gint32 len, UChar *result, gint32 rlen);
int unicode_filters(const UChar *source, gint32 len, UChar *result);
gchar* unicode_canonize(const gchar *in);

#else /* !xxxUSE_ICU */

#define UNICODE_CANONIZE(x) utf8_canonize(x)

#endif	/* xxxUSE_ICU */

#endif	/* _utf8_h_ */

/* vi: set sw=4 ts=4 cindent: */

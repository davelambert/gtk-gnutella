/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
 *
 * Unicode Transformation Format 8 bits.
 *
 * This code has been heavily inspired by utf8.c/utf8.h from Perl 5.6.1,
 * written by Larry Wall et al.
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

#include "common.h"

RCSID("$Id$");

#ifdef ENABLE_NLS
#include <libintl.h>
#endif /* ENABLE_NLS */

#include <locale.h>

#if defined(I_LIBCHARSET)
#include <libcharset.h>
#else
#include <langinfo.h>
#endif /* I_LIBCHARSET */

#ifndef USE_GLIB2
#include <iconv.h>
#define GIConv iconv_t
#define g_iconv_open(t, f) iconv_open(t, f)
#define g_iconv(c, i, n, o, m) iconv(c, i, n, o, m)
#endif /* !USE_GLIB2 */

#include "utf8_tables.h"

#include "utf8.h"
#include "misc.h"
#include "glib-missing.h"
#include "override.h"		/* Must be the last header included */

static guint32 common_dbg = 0;	/* XXX -- need to init lib's props --RAM */

static void unicode_compose_init(void);
static void unicode_decompose_init(void);
size_t utf8_decompose_nfd(const gchar *in, gchar *out, size_t size);
size_t utf8_decompose_nfkd(const gchar *in, gchar *out, size_t size);
size_t utf32_strmaxlen(const guint32 *s, size_t maxlen);
size_t utf32_to_utf8(const guint32 *in, gchar *out, size_t size);
size_t utf32_strlen(const guint32 *s);

/* use_icu is set to TRUE if the initialization of ICU succeeded. If it
 * fails, we'll fall back to the non-ICU behaviour. */
static gboolean use_icu = FALSE;

/* Used by is_latin_locale(). It is initialized by locale_init(). */
static gboolean latin_locale = FALSE;

#ifdef USE_ICU
static UConverter *conv_icu_locale = NULL;
static UConverter *conv_icu_utf8 = NULL;
#endif /* USE_ICU */

static const gchar *charset = NULL;

static GIConv cd_locale_to_utf8	= (GIConv) -1;
static GIConv cd_utf8_to_locale	= (GIConv) -1;
static GIConv cd_latin_to_utf8	= (GIConv) -1;

static inline G_GNUC_CONST guint
utf8_skip(guchar c)
{
	/*
	 * How wide is an UTF-8 encoded char, depending on its first byte?
	 */
	static const guint8 utf8len[64] = {
		2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,		/* 192-207 */
		2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,		/* 208-223 */
		3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,		/* 224-239 */
		4,4,4,4,4,4,4,4,5,5,5,5,6,6,			/* 240-253 */
		7,7										/* 254-255: special */
	};

	return 0xC0 ? utf8len[c & 63] : 1;
}

static const guint8 utf8len_mark[] = {
	0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC
};

#define UTF8_LENGTH_MARK(len)	utf8len_mark[len]

/*
 * The following table is from Unicode 3.1.
 *
 * Code Points           1st Byte    2nd Byte    3rd Byte    4th Byte
 *
 *    U+0000..U+007F      00..7F���
 *    U+0080..U+07FF      C2..DF      80..BF���
 *    U+0800..U+0FFF      E0          A0..BF      80..BF��
 *    U+1000..U+FFFF      E1..EF      80..BF      80..BF��
 *   U+10000..U+3FFFF     F0          90..BF      80..BF      80..BF
 *   U+40000..U+FFFFF     F1..F3      80..BF      80..BF      80..BF
 *  U+100000..U+10FFFF    F4          80..8F      80..BF      80..BF

 */

#define CHAR(x)					((guchar) (x))
#define UTF8_BYTE_MARK			0x80
#define UTF8_BYTE_MASK			0xbf
#define UTF8_IS_ASCII(x)		(CHAR(x) < UTF8_BYTE_MARK)
#define UTF8_IS_START(x)		(CHAR(x) >= 0xc0 && CHAR(x) <= 0xfd)
#define UTF8_IS_CONTINUATION(x)	\
	(CHAR(x) >= UTF8_BYTE_MARK && CHAR(x) <= UTF8_BYTE_MASK)
#define UTF8_IS_CONTINUED(x)	(CHAR(x) & UTF8_BYTE_MARK)

#define UTF8_CONT_MASK			(CHAR(0x3f))
#define UTF8_ACCU_SHIFT			6
#define UTF8_ACCUMULATE(o,n)	\
	(((o) << UTF8_ACCU_SHIFT) | (CHAR(n) & UTF8_CONT_MASK))

#define UNISKIP(v) (			\
	(v) <  0x80U 		? 1 :	\
	(v) <  0x800U 		? 2 :	\
	(v) <  0x10000U 	? 3 :	\
	(v) <  0x200000U	? 4 :	\
	(v) <  0x4000000U	? 5 :	\
	(v) <  0x80000000U	? 6 : 7)

#define UNI_SURROGATE_FIRST		0xd800
#define UNI_SURROGATE_SECOND	0xdc00
#define UNI_SURROGATE_LAST		0xdfff
#define UNI_HANGUL_FIRST		0xac00
#define UNI_HANGUL_LAST			0xd7a3
#define UNI_REPLACEMENT			0xfffd
#define UNI_BYTE_ORDER_MARK		0xfffe
#define UNI_ILLEGAL				0xffff

#define UNICODE_IS_SURROGATE(x)	\
	((x) >= UNI_SURROGATE_FIRST && (x) <= UNI_SURROGATE_LAST)

#define UNICODE_IS_HANGUL(x)	\
	((x) >= UNI_HANGUL_FIRST && (x) <= UNI_HANGUL_LAST)

#define UNICODE_IS_ASCII(x)				((x) < 0x0080U)
#define UNICODE_IS_REPLACEMENT(x)		((x) == UNI_REPLACEMENT)
#define UNICODE_IS_BYTE_ORDER_MARK(x)	((x) == UNI_BYTE_ORDER_MARK)
#define UNICODE_IS_ILLEGAL(x) \
	(((x) & UNI_ILLEGAL) == UNI_ILLEGAL || (x) > 0x10FFFFU)

static inline guint
utf32_combining_class(guint32 uc)
{
#define GET_ITEM(i) (utf32_comb_class_lut[(i)].uc)
#define FOUND(i) G_STMT_START { \
	return utf32_comb_class_lut[(i)].cc; \
	/* NOTREACHED */ \
} G_STMT_END
	
	/* Perform a binary search to find ``uc'' */
	BINARY_SEARCH(guint32, uc, G_N_ELEMENTS(utf32_comb_class_lut), CMP,
		GET_ITEM, FOUND);
	
#undef FOUND
#undef GET_ITEM
	return 0;
}

static inline gboolean
utf32_composition_exclude(guint32 uc)
{
#define GET_ITEM(i) (utf32_composition_exclusions[(i)])
#define FOUND(i) G_STMT_START { \
	return TRUE; \
	/* NOTREACHED */ \
} G_STMT_END
	
	/* Perform a binary search to find ``uc'' */
	BINARY_SEARCH(guint32, uc, G_N_ELEMENTS(utf32_composition_exclusions), CMP,
		GET_ITEM, FOUND);
	
#undef FOUND
#undef GET_ITEM
	return FALSE;
}

static inline gint
general_category_cmp(size_t i, guint32 uc)
{
	guint32 uc2, uc3;
	
	uc2 = utf32_general_category_lut[i].uc;
	if (uc == uc2)
		return 0;
	if (uc < uc2)
		return 1;
	
	uc3 = uc2 + utf32_general_category_lut[i].len;
	return uc < uc3 ? 0 : -1;
}

static inline uni_gc_t
utf32_general_category(guint32 uc)
{
#define GET_ITEM(i) (i)
#define FOUND(i) G_STMT_START { \
	return utf32_general_category_lut[(i)].gc; \
	/* NOTREACHED */ \
} G_STMT_END

	/* Perform a binary search to find ``uc'' */
	BINARY_SEARCH(size_t, uc, G_N_ELEMENTS(utf32_general_category_lut),
		general_category_cmp, GET_ITEM, FOUND);

#undef FOUND
#undef GET_ITEM
	return UNI_GC_OTHER_NOT_ASSIGNED;
}

/**
 * Are the first bytes of string `s' forming a valid UTF-8 character?
 *
 * @returns amount of bytes used to encode that character, or 0 if invalid.
 */
gint
utf8_is_valid_char(const gchar *s)
{
	const guchar u = (guchar) *s;
	guint len;
	guint slen;
	guint32 v;
	guint32 ov;

	if (UTF8_IS_ASCII(u))
		return 1;

	if (!UTF8_IS_START(u))
		return 0;

	len = utf8_skip(u);

	if (len < 2 || !UTF8_IS_CONTINUATION(s[1]))
		return 0;

	for (slen = len - 1, s++, ov = v = u; slen; slen--, s++, ov = v) {
		if (!UTF8_IS_CONTINUATION(*s))
			return 0;
		v = UTF8_ACCUMULATE(v, *s);
		if (v < ov)
			return 0;
	}

	if ((guint) UNISKIP(v) < len)
		return 0;

	return len;
}

/**
 * Returns amount of UTF-8 chars when first `len' bytes of the given string
 * `s' form valid a UTF-8 string, 0 meaning the string is not valid UTF-8.
 *
 * If `len' is 0, the string must be NUL-terminated.
 */
size_t
utf8_is_valid_string(const gchar *s, size_t len)
{
	const gchar *x;
	gint clen, n = 0;

	g_assert(len <= INT_MAX);

	if (len != 0) {
		const gchar *s_end;
		
		s_end = s + len;
		for (x = s; x < s_end; n++, x += clen) {
			if (0 == (clen = utf8_is_valid_char(x)))
				return 0;
		}
		if (x != s_end)
			return 0;
	} else {
		for (x = s; *x != '\0'; n++, x += clen) {
			if (0 == (clen = utf8_is_valid_char(x)))
				return 0;
		}
	}

	return n;
}

/**
 * Works exactly like strlcpy() but preserves a valid UTF-8 encoding, if
 * the string has to be truncated.
 *
 * @param dst the target buffer to copy the string to.
 * @param src the source buffer to copy the string from.
 * @param dst_size the number of bytes ``dst'' can hold.
 */
size_t
utf8_strlcpy(gchar *dst, const gchar *src, size_t dst_size)
{
	gchar *d = dst;
	const gchar *s = src;

	g_assert(NULL != dst);
	g_assert(NULL != src);

	if (dst_size-- > 0) {
		while ('\0' != *s) {
			size_t clen;

			clen = utf8_is_valid_char(s);
			clen = MAX(1, clen);
			if (clen > dst_size)
				break;

			if (clen == 1) {
				*d++ = *s++;
				dst_size--;
			} else {
				memmove(d, s, clen);
				d += clen;
				s += clen;
				dst_size -= clen;
			}
		}
		*d = '\0';
	}
 	while (*s)
		s++;
	return s - src;
}

/**
 * @param uc the unicode character to encode.
 * @returns 0 if the unicode character is invalid. Otherwise the
 *          length of the UTF-8 character is returned.
 */
static inline gint
utf8_encoded_char_len(guint32 uc)
{
	guint len;

	if (UNICODE_IS_SURROGATE(uc)) {
		return 0;
	}

	len = UNISKIP(uc);
	if (len > 4) {
		len = 2;
		uc = UNI_REPLACEMENT;
	}
	g_assert(len > 0 && len <= 6);

	return len;
}


/**
 * @param uc the unicode character to encode.
 * @param buf the destination buffer. MUST BE at least 6 bytes long.
 * @returns 0 if the unicode character is invalid. Otherwise the
 *          length of the UTF-8 character is returned.
 */
gint
utf8_encode_char(guint32 uc, gchar *buf)
{
	guint len;
	gchar *p;

	g_assert(buf);

	len = utf8_encoded_char_len(uc);
	if (len > 0) {
		g_assert(len > 0 && len <= 6);

		p = &buf[len];
		while (--p > buf) {
			*p = (uc | UTF8_BYTE_MARK) & UTF8_BYTE_MASK;
			uc >>= UTF8_ACCU_SHIFT;
		}
		*p = uc | UTF8_LENGTH_MARK(len);
	}
	return len;
}

/**
 * Encodes a single UTF-32 character as UTF-16 into a buffer.
 * See also RFC 2781.
 *
 * @param uc the unicode character to encode.
 * @param dst the destination buffer. MUST BE at least 4 bytes long.
 * @returns 0 if the unicode character is invalid. Otherwise, the
 *          amount of UTF-16 characters is returned i.e., 1 or 2.
 */
gint
utf16_encode_char(guint32 uc, guint16 *dst)
{
	g_assert(dst != NULL);	
	
	if (uc <= 0xFFFF) {
		*dst = uc;
		return 1;
	} else if (uc <= 0x10FFFF) {
		uc -= 0x10000;
		dst[0] = (uc >> 10) | UNI_SURROGATE_FIRST;
		dst[1] = (uc & 0x3ff) | UNI_SURROGATE_SECOND;
		return 2;
	}
	return 0;
}

/**
 * Returns the character value of the first character in the string `s',
 * which is assumed to be in UTF-8 encoding and no longer than `len'.
 * `retlen' will be set to the length, in bytes, of that character.
 *
 * If `s' does not point to a well-formed UTF-8 character, the behaviour
 * is dependent on the value of `warn'.  When FALSE, it is assumed that
 * the caller will raise a warning, and this function will silently just
 * set `retlen' to -1 and return zero.
 */
guint32
utf8_decode_char(const gchar *s, gint len, gint *retlen, gboolean warn)
{
	guint32 v = *s;
	guint32 ov = 0;
	gint clen = 1;
	gint expectlen = 0;
	gint warning = -1;
	char msg[128];

	g_assert(s);

#define UTF8_WARN_EMPTY				0
#define UTF8_WARN_CONTINUATION		1
#define UTF8_WARN_NON_CONTINUATION	2
#define UTF8_WARN_FE_FF				3
#define UTF8_WARN_SHORT				4
#define UTF8_WARN_OVERFLOW			5
#define UTF8_WARN_SURROGATE			6
#define UTF8_WARN_BOM				7
#define UTF8_WARN_LONG				8
#define UTF8_WARN_ILLEGAL			9

	if (len == 0) {
		warning = UTF8_WARN_EMPTY;
		goto malformed;
	}

	if (UTF8_IS_ASCII(v)) {
		if (retlen)
			*retlen = 1;
		return *s;
	}

	if (UTF8_IS_CONTINUATION(v)) {
		warning = UTF8_WARN_CONTINUATION;
		goto malformed;
	}

	if (UTF8_IS_START(v) && len > 1 && !UTF8_IS_CONTINUATION(s[1])) {
		warning = UTF8_WARN_NON_CONTINUATION;
		goto malformed;
	}

	if (v == 0xfe || v == 0xff) {
		warning = UTF8_WARN_FE_FF;
		goto malformed;
	}

	if      (!(v & 0x20)) { clen = 2; v &= 0x1f; }
	else if (!(v & 0x10)) { clen = 3; v &= 0x0f; }
	else if (!(v & 0x08)) { clen = 4; v &= 0x07; }
	else if (!(v & 0x04)) { clen = 5; v &= 0x03; }
	else if (!(v & 0x02)) { clen = 6; v &= 0x01; }
	else if (!(v & 0x01)) { clen = 7; v = 0; }

	if (retlen)
		*retlen = clen;

	expectlen = clen;

	if (len < expectlen) {
		warning = UTF8_WARN_SHORT;
		goto malformed;
	}

	for (clen--, s++, ov = v; clen; clen--, s++, ov = v) {
		if (!UTF8_IS_CONTINUATION(*s)) {
			s--;
			warning = UTF8_WARN_NON_CONTINUATION;
			goto malformed;
		} else
			v = UTF8_ACCUMULATE(v, *s);

		if (v < ov) {
			warning = UTF8_WARN_OVERFLOW;
			goto malformed;
		} else if (v == ov) {
			warning = UTF8_WARN_LONG;
			goto malformed;
		}
	}

	if (UNICODE_IS_SURROGATE(v)) {
		warning = UTF8_WARN_SURROGATE;
		goto malformed;
	} else if (UNICODE_IS_BYTE_ORDER_MARK(v)) {
		warning = UTF8_WARN_BOM;
		goto malformed;
	} else if (expectlen > UNISKIP(v)) {
		warning = UTF8_WARN_LONG;
		goto malformed;
	} else if (UNICODE_IS_ILLEGAL(v)) {
		warning = UTF8_WARN_ILLEGAL;
		goto malformed;
	}

	return v;

malformed:

	if (!warn) {
		if (retlen)
			*retlen = -1;
		return 0;
	}

	switch (warning) {
	case UTF8_WARN_EMPTY:
		gm_snprintf(msg, sizeof(msg), "empty string");
		break;
	case UTF8_WARN_CONTINUATION:
		gm_snprintf(msg, sizeof(msg),
			"unexpected continuation byte 0x%02lx", (gulong) v);
		break;
	case UTF8_WARN_NON_CONTINUATION:
		gm_snprintf(msg, sizeof(msg),
			"unexpected non-continuation byte 0x%02lx "
			"after start byte 0x%02lx", (gulong) s[1], (gulong) v);
		break;
	case UTF8_WARN_FE_FF:
		gm_snprintf(msg, sizeof(msg), "byte 0x%02lx", (gulong) v);
		break;
	case UTF8_WARN_SHORT:
		gm_snprintf(msg, sizeof(msg), "%d byte%s, need %d",
			len, len == 1 ? "" : "s", expectlen);
		break;
	case UTF8_WARN_OVERFLOW:
		gm_snprintf(msg, sizeof(msg), "overflow at 0x%02lx, byte 0x%02lx",
			(gulong) ov, (gulong) *s);
		break;
	case UTF8_WARN_SURROGATE:
		gm_snprintf(msg, sizeof(msg), "UTF-16 surrogate 0x04%lx", (gulong) v);
		break;
	case UTF8_WARN_BOM:
		gm_snprintf(msg, sizeof(msg), "byte order mark 0x%04lx", (gulong) v);
		break;
	case UTF8_WARN_LONG:
		gm_snprintf(msg, sizeof(msg), "%d byte%s, need %d",
			expectlen, expectlen == 1 ? "" : "s", UNISKIP(v));
		break;
	case UTF8_WARN_ILLEGAL:
		gm_snprintf(msg, sizeof(msg), "character 0x%04lx", (gulong) v);
		break;
	default:
		gm_snprintf(msg, sizeof(msg), "unknown reason");
		break;
	}

	g_warning("malformed UTF-8 character: %s", msg);

	if (retlen)
		*retlen = expectlen ? expectlen : len;

	return 0;
}

/*
 * utf8_to_iso8859
 *
 * Convert UTF-8 string to ISO-8859-1 inplace.  If `space' is TRUE, all
 * characters outside the U+0000 .. U+00FF range are turned to space U+0020.
 * Otherwise, we stop at the first out-of-range character.
 *
 * If `len' is 0, the length of the string is computed with strlen().
 *
 * Returns length of decoded string.
 */
gint
utf8_to_iso8859(gchar *s, gint len, gboolean space)
{
	gchar *x = s;
	gchar *xw = s;			/* Where we write back ISO-8859 chars */
	gchar *s_end;

	if (!len)
		len = strlen(s);
	s_end = s + len;

	while (x < s_end) {
		gint clen;
		guint32 v = utf8_decode_char(x, len, &clen, FALSE);

		if (clen == -1)
			break;

		g_assert(clen >= 1);

		if (v & 0xffffff00) {	/* Not an ISO-8859-1 character */
			if (!space)
				break;
			v = 0x20;
		}

		*xw++ = (guchar) v;
		x += clen;
		len -= clen;
	}

	*xw = '\0';

	return xw - s;
}


#if defined(USE_GLIB2)

static const char *
get_locale_charset(void)
{
	const char *cs = NULL;

	g_get_charset(&cs);
	return cs;
}

#else /* !USE_GLIB2 */

#if defined(I_LIBCHARSET)
#define get_locale_charset() locale_charset()
#else /* !I_LIBCHARSET */

/* List of known codesets. The first word of each string is the alias to be
 * returned. The words are seperated by whitespaces.
 */
static const char *codesets[] = {
 "ASCII ISO_646.IRV:1983 646 C US-ASCII la_LN.ASCII lt_LN.ASCII",
 "BIG5 big5 big5 zh_TW.BIG5 zh_TW.Big5",
 "CP1046 IBM-1046",
 "CP1124 IBM-1124",
 "CP1129 IBM-1129",
 "CP1252 IBM-1252",
 "CP437 en_NZ en_US",
 "CP775 lt lt_LT lv lv_LV",
 "CP850 IBM-850 cp850 ca ca_ES de de_AT de_CH de_DE en en_AU en_CA en_GB "
	"en_ZA es es_AR es_BO es_CL es_CO es_CR es_CU es_DO es_EC es_ES es_GT "
	"es_HN es_MX es_NI es_PA es_PY es_PE es_SV es_UY es_VE et et_EE eu eu_ES "
	"fi fi_FI fr fr_BE fr_CA fr_CH fr_FR ga ga_IE gd gd_GB gl gl_ES id id_ID "
	"it it_CH it_IT nl nl_BE nl_NL pt pt_BR pt_PT sv sv_SE mt mt_MT eo eo_EO",
 "CP852 cs cs_CZ hr hr_HR hu hu_HU pl pl_PL ro ro_RO sk sk_SK sl sl_SI "
	"sq sq_AL sr sr_YU",
 "CP856 IBM-856",
 "CP857 tr tr_TR",
 "CP861 is is_IS",
 "CP862 he he_IL",
 "CP864 ar ar_AE ar_DZ ar_EG ar_IQ ar_IR ar_JO ar_KW ar_MA ar_OM ar_QA "
	"ar_SA ar_SY",
 "CP865 da da_DK nb nb_NO nn nn_NO no no_NO",
 "CP866 ru_RU.CP866 ru_SU.CP866 be be_BE bg bg_BG mk mk_MK ru ru_RU ",
 "CP869 el el_GR",
 "CP874 th th_TH",
 "CP922 IBM-922",
 "CP932 IBM-932 ja ja_JP",
 "CP943 IBM-943",
 "CP949 KSC5601 kr kr_KR",
 "CP950 zh_TW",
 "DEC-HANYU dechanyu",
 "DEC-KANJI deckanji",
 "EUC-JP IBM-eucJP eucJP eucJP sdeckanji ja_JP.EUC",
 "EUC-KR IBM-eucKR eucKR eucKR deckorean 5601 ko_KR.EUC",
 "EUC-TW IBM-eucTW eucTW eucTW cns11643",
 "GB2312 IBM-eucCN hp15CN eucCN dechanzi gb2312 zh_CN.EUC",
 "GBK zh_CN",
 "HP-ARABIC8 arabic8",
 "HP-GREEK8 greek8",
 "HP-HEBREW8 hebrew8",
 "HP-KANA8 kana8",
 "HP-ROMAN8 roman8 ",
 "HP-TURKISH8 turkish8 ",
 "ISO-8859-1 ISO8859-1 iso88591 da_DK.ISO_8859-1 de_AT.ISO_8859-1 "
	"de_CH.ISO_8859-1 de_DE.ISO_8859-1 en_AU.ISO_8859-1 en_CA.ISO_8859-1 "
	"en_GB.ISO_8859-1 en_US.ISO_8859-1 es_ES.ISO_8859-1 fi_FI.ISO_8859-1 "
	"fr_BE.ISO_8859-1 fr_CA.ISO_8859-1 fr_CH.ISO_8859-1 fr_FR.ISO_8859-1 "
	"is_IS.ISO_8859-1 it_CH.ISO_8859-1 it_IT.ISO_8859-1 la_LN.ISO_8859-1 "
	"lt_LN.ISO_8859-1 nl_BE.ISO_8859-1 nl_NL.ISO_8859-1 no_NO.ISO_8859-1 "
	"pt_PT.ISO_8859-1 sv_SE.ISO_8859-1",
 "ISO-8859-13 IBM-921",
 "ISO-8859-14 ISO_8859-14 ISO_8859-14:1998 iso-ir-199 latin8 iso-celtic l8",
 "ISO-8859-15 ISO8859-15 iso885915 da_DK.DIS_8859-15 de_AT.DIS_8859-15 "
	"de_CH.DIS_8859-15 de_DE.DIS_8859-15 en_AU.DIS_8859-15 en_CA.DIS_8859-15 "
	"en_GB.DIS_8859-15 en_US.DIS_8859-15 es_ES.DIS_8859-15 fi_FI.DIS_8859-15 "
	"fr_BE.DIS_8859-15 fr_CA.DIS_8859-15 fr_CH.DIS_8859-15 fr_FR.DIS_8859-15 "
	"is_IS.DIS_8859-15 it_CH.DIS_8859-15 it_IT.DIS_8859-15 la_LN.DIS_8859-15 "
	"lt_LN.DIS_8859-15 nl_BE.DIS_8859-15 nl_NL.DIS_8859-15 no_NO.DIS_8859-15 "
	"pt_PT.DIS_8859-15 sv_SE.DIS_8859-15",
 "ISO-8859-2 ISO8859-2 iso88592 cs_CZ.ISO_8859-2 hr_HR.ISO_8859-2 "
	"hu_HU.ISO_8859-2 la_LN.ISO_8859-2 lt_LN.ISO_8859-2 pl_PL.ISO_8859-2 "
	"sl_SI.ISO_8859-2",
 "ISO-8859-4 ISO8859-4 la_LN.ISO_8859-4 lt_LT.ISO_8859-4",
 "ISO-8859-5 ISO8859-5 iso88595 ru_RU.ISO_8859-5 ru_SU.ISO_8859-5",
 "ISO-8859-6 ISO8859-6 iso88596",
 "ISO-8859-7 ISO8859-7 iso88597",
 "ISO-8859-8 ISO8859-8 iso88598",
 "ISO-8859-9 ISO8859-9 iso88599",
 "KOI8-R koi8-r ru_RU.KOI8-R ru_SU.KOI8-R",
 "KOI8-U uk_UA.KOI8-U",
 "SHIFT_JIS SJIS PCK ja_JP.SJIS ja_JP.Shift_JIS",
 "TIS-620 tis620 TACTIS TIS620.2533",
 "UTF-8 utf8 *",
 NULL
};

/*
 * locale_charset:
 *
 * Returns a string representing the current locale as an alias which is
 * understood by GNU iconv. The returned pointer points to a static buffer.
 */
const char *
get_locale_charset(void)
{
	int i = 0;
	const char *cs;
	const char *start = codesets[0];
	const char *first_end = NULL;

	cs = nl_langinfo(CODESET);
	if (NULL == cs || '\0' == *cs)
		return NULL;

	while (NULL != codesets[i]) {
		static char buf[64];
		const char *end;
		size_t len;

		end = strchr(start, ' ');
		if (NULL == end)
			end = strchr(start, '\0');
		if (NULL == first_end)
			first_end = end;

 		len = end - start;
		if (len > 0 && is_strcaseprefix(start, cs)) {
			len = first_end - codesets[i] + 1;
			g_strlcpy(buf, codesets[i], MIN(len, sizeof(buf)));
			return buf;
		}
		if ('\0' == *end) {
			first_end = NULL;
			start = codesets[++i];
		} else
			start = end + 1;

	}
	return NULL;
}
#endif /* I_LIBCHARSET */

#endif /* USE_GLIB2 */

const gchar *
locale_get_charset(void)
{
	return charset;
}

static void
textdomain_init(const char *codeset)
{
#ifdef ENABLE_NLS
	bindtextdomain(PACKAGE, LOCALE_EXP);

#ifdef HAS_BIND_TEXTDOMAIN_CODESET

#ifdef USE_GLIB2
	codeset = "UTF-8";
#endif /* USE_GLIB2*/

	bind_textdomain_codeset(PACKAGE, codeset);

#endif /* HAS_BIND_TEXTDOMAIN_CODESET */

	textdomain(PACKAGE);

#else /* !NLS */
	(void) codeset;
#endif /* NLS */
}

void
locale_init(void)
{
	static const gchar * const latin_sets[] = {
		"ASCII",
		"ISO-8859-1",
		"ISO-8859-15",
		"CP1252",
		"MacRoman",
		"CP437",
		"CP775",
		"CP850",
		"CP852",
		"CP865",
		"HP-ROMAN8",
		"ISO-8859-2",
		"ISO-8859-4",
		"ISO-8859-14",
	};
	guint i;

	setlocale(LC_ALL, "");
	charset = get_locale_charset();

	if (charset == NULL) {
		/* Default locale codeset */
		charset = "ISO-8859-1";
		g_warning("locale_init: Using default codeset %s as fallback.",
			charset);
	}

	g_message("using locale charset \"%s\"", charset);
	textdomain_init(charset);

	for (i = 0; i < G_N_ELEMENTS(latin_sets); i++)
		if (0 == ascii_strcasecmp(charset, latin_sets[i])) {
			latin_locale = TRUE;
			break;
		}

	if ((GIConv)-1 == (cd_latin_to_utf8 = g_iconv_open("UTF-8", "ISO-8859-1")))
		g_warning("g_iconv_open(\"UTF-8\", \"ISO-8859-1\") failed.");
	if (0 != strcmp("ISO-8859-1", charset)) {
		if ((GIConv)-1 == (cd_locale_to_utf8 = g_iconv_open("UTF-8", charset)))
			g_warning("g_iconv_open(\"UTF-8\", \"%s\") failed.", charset);
	} else {
		cd_locale_to_utf8 = cd_latin_to_utf8;
	}
	if ((GIConv)-1 == (cd_utf8_to_locale = g_iconv_open(charset, "UTF-8")))
		g_warning("g_iconv_open(\"%s\", \"UTF-8\") failed.", charset);

#ifdef USE_ICU
	{
		UErrorCode errorCode = U_ZERO_ERROR;

		/* set up the locale converter */
		conv_icu_locale = ucnv_open(charset, &errorCode);
		if (U_FAILURE(errorCode)) {
			g_warning("ucnv_open for locale failed with %d", errorCode);
		} else {

			/* set up the UTF-8 converter */
			conv_icu_utf8 = ucnv_open("utf8", &errorCode);
			if (U_FAILURE(errorCode)) {
				g_warning("ucnv_open for utf-8 failed with %d", errorCode);
			} else {
				/* Initialization succeeded, thus enable using of ICU */
				use_icu = TRUE;
			}
		}
	}
#endif
	
	unicode_compose_init();
	unicode_decompose_init();
}

/**
 * Called at shutdown time.
 */
void
locale_close(void)
{
#ifdef USE_ICU
	if (conv_icu_locale) {
	  ucnv_close(conv_icu_locale);
	  conv_icu_locale = NULL;
	}
	if (conv_icu_utf8) {
	  ucnv_close(conv_icu_utf8);
	  conv_icu_utf8 = NULL;
	}
#endif
}

static inline char *
g_iconv_complete(GIConv cd, const char *inbuf, size_t inbytes_left,
	char *outbuf, size_t outbytes_left)
{
#if 0
	/* This is the appropriate replacement unicode character 0xFFFD but
	 * it looks awkward (a modified question mark) in a non-unicode
	 * context. So rather use a underscore for filenames.
	 */
	static const gchar replacement[] = { 0xEF, 0xBF, 0xBD };
#else
	static const gchar replacement[] = { '_' };
#endif
	gchar *result = outbuf;

	if ((GIConv) -1 == cd)
		return NULL;

	if (outbytes_left > 0)
		outbuf[0] = '\0';

	while (inbytes_left > 0 && outbytes_left > 1) {
		size_t ret;

		ret = g_iconv(cd, cast_to_gpointer(&inbuf), &inbytes_left,
		    &outbuf, &outbytes_left);

		if ((size_t) -1 == ret) {
			switch (errno) {
			case EILSEQ:
			case EINVAL:
				if (common_dbg > 1)
					g_warning("g_iconv_complete: g_iconv() failed soft: %s",
						g_strerror(errno));

				if (outbytes_left > sizeof replacement) {
					inbuf++;
					inbytes_left--;
					memcpy(outbuf, replacement, sizeof replacement);
					outbuf += sizeof replacement;
					outbytes_left -= sizeof replacement;
				} else {
					outbytes_left = 1;
				}
				break;
			default:
				if (common_dbg > 1)
					g_warning("g_iconv_complete(): g_iconv() failed hard: %s",
						g_strerror(errno));
				return NULL;
			}
		}
	}
	*outbuf = '\0';
	return result;
}

/**
 * If ``len'' is 0 the length will be calculated using strlen(), otherwise
 * only ``len'' characters will be converted.
 * If the string is already valid UTF-8 it will be returned "as-is".
 * The function might return a pointer to a STATIC buffer! If the output
 * string is longer than 4095 characters it will be truncated.
 * Non-convertible characters will be replaced by '_'. The returned string
 * WILL be NUL-terminated in any case.
 *
 * In case of an unrecoverable error, NULL is returned.
 *
 * ATTENTION:	Don't use this function for anything but *uncritical*
 *				strings	e.g., to view strings in the GUI. The conversion
 *				MAY be inappropriate!
 */
gchar *
locale_to_utf8(const gchar *str, size_t len)
{
	static gchar outbuf[4096 + 6]; /* an UTF-8 char is max. 6 bytes large */

	g_assert(NULL != str);

	if (0 == len)
		len = strlen(str);
	if (utf8_is_valid_string(str, len))
		return deconstify_gchar(str);
	else
		return g_iconv_complete(cd_locale_to_utf8,
				str, len, outbuf, sizeof(outbuf) - 7);
}

/**
 * If the string is already valid UTF-8 it will be returned "as-is", otherwise
 * a newly allocated buffer containing the converted string.
 * Non-convertible characters will be replaced by '_'. The returned string
 * WILL be NUL-terminated in any case.
 *
 * In case of an unrecoverable error, NULL is returned.
 *
 * @param str a NUL-terminated string.
 * @return a pointer to the UTF-8 encoded string.
 */
gchar *
locale_to_utf8_full(const gchar *str)
{
	gchar *s;
	size_t utf8_len, len;
	
	g_assert(NULL != str);

	len = strlen(str);
   	utf8_len = len * 6 + 1;
	if (utf8_is_valid_string(str, len))
		return deconstify_gchar(str);
	
	g_assert((utf8_len - 1) / 6 == len);
	s = g_malloc(utf8_len);

	return g_iconv_complete(cd_locale_to_utf8, str, len, s, utf8_len);
}

/**
 * Converts a string from the current locale encoding to UTF-8 encoding
 * with all characters decomposed.
 *
 * @param str the string to convert.
 * @param len the length of ``str''. May be set to zero if ``str'' is
 *		  NUL-terminated.
 *
 * @returns a newly allocated string.
 */
gchar *
locale_to_utf8_nfd(const gchar *str, size_t len)
{
	gchar sbuf[4096];
	const gchar *s;
	gchar *ret;
	size_t utf8_size;

	g_assert(NULL != str);

	if (0 == len)
		len = strlen(str);
	if (0 == len)
		return g_strdup("");

   	utf8_size = len * 6 + 1;
	if (utf8_is_valid_string(str, len)) {
		s = str;
	} else {
		gchar *p;

		g_assert((utf8_size - 1) / 6 == len);
		p = len < sizeof sbuf ? sbuf : g_malloc(utf8_size);
		s = g_iconv_complete(cd_locale_to_utf8, str, len, p, utf8_size);
	}

	/*
	 * Do a dry run first to determine the length. The output buffer won't
	 * be touched, but it must be valid. So just pass ``s'' not NULL.
	 */
	len = utf8_decompose_nfd(s, NULL, 0);
	utf8_size = len + 1;
	ret = g_malloc(utf8_size);
	len = utf8_decompose_nfd(s, ret, utf8_size);
	g_assert(len == utf8_size - 1);

	if (s != str && s != sbuf) {
		g_free(deconstify_gchar(s));
		s = NULL;
	}

	return ret;
}


gchar *
utf8_to_locale(const gchar *str, size_t len)
{
	static gchar outbuf[4096 + 6]; /* a multibyte char is max. 6 bytes large */

	g_assert(NULL != str);

	return g_iconv_complete(cd_utf8_to_locale,
				str, len != 0 ? len : strlen(str), outbuf, sizeof(outbuf) - 7);
}

gboolean
is_ascii_string(const gchar *str)
{
	gint c;

	while ((c = (guchar) *str++))
		if (c & ~0x7f)
	        return FALSE;

    return TRUE;
}

gchar *
iso_8859_1_to_utf8(const gchar *fromstr)
{
	static gchar outbuf[4096 + 6]; /* a multibyte char is max. 6 bytes large */

	g_assert(NULL != fromstr);

	return g_iconv_complete(cd_latin_to_utf8,
				fromstr, strlen(fromstr), outbuf, sizeof(outbuf) - 7);
}

gchar *
lazy_utf8_to_locale(const gchar *str, size_t len)
{
	gchar *t = utf8_to_locale(str, len);
	return NULL != t ? t : "<Cannot convert to locale>";
}

/**
 * Converts the supplied string ``str'' from the current locale encoding
 * to a UTF-8 string.  If compiled for GTK+ 2.x this function does also
 * enforce NFC.
 *
 * @param str the string to convert.
 * @param len the length of ``str''. May be set to zero if ``str'' is
 *		  NUL-terminated.
 *
 * @returns the converted string or ``str'' if no conversion was necessary.
 */
gchar *
lazy_locale_to_utf8(const gchar *str, size_t len)
{
	const gchar *s;

	/* Let's assume that most of the supplied strings are pure ASCII. */
	if (is_ascii_string(str))
		return deconstify_gchar(str);

	s = locale_to_utf8(str, len);
	if (!s)
		return "<Cannot convert to UTF-8>";

#ifdef USE_GLIB2
	{
		static gchar buf[4096 + 6];
		gchar *s_nfc;

		/* Enforce UTF-8 NFC because GTK+ would render a decomposed string
		 * otherwise (if the string is not in NFC). This is a known GTK+ bug.
		 */

		s_nfc = g_utf8_normalize(s, (gssize) -1, G_NORMALIZE_NFC);
		if (0 != strcmp(s, s_nfc)) {
			g_strlcpy(buf, s_nfc, sizeof buf);
			s = buf;
		}
		G_FREE_NULL(s_nfc);
	}
#endif	/* USE_GLIB2 */

	return deconstify_gchar(s);
}

/**
 * Converts a UTF-8 encoded string to a UTF-32 encoded string. The
 * target string ``out'' is always be zero-terminated unless ``size''
 * is zero.
 *
 * @param in the UTF-8 input string.
 * @param out the target buffer for converted UTF-32 string.
 * @param size the length of the outbuf buffer - characters not bytes!
 *        Whether the buffer was too small can be checked by comparing
 *        ``size'' with the return value. The value of ``size'' MUST NOT
 *        exceed INT_MAX.
 *
 * @returns the length in characters of completely converted string.
 */
size_t
utf8_to_utf32(const gchar *in, guint32 *out, size_t size)
{
	const gchar *s = in;
	guint32 *p = out;
	gint retlen;
	size_t len = 0;

	g_assert(in != NULL);
	g_assert(size == 0 || out != NULL);
	g_assert(size <= INT_MAX);

	if (size > 0) {
		while (*s != '\0' && --size > 0) {
			len = utf8_decode_lookahead(s, len);
			*p++ = utf8_decode_char(s, len, &retlen, TRUE);
			s += retlen;
			len -= retlen;
		}
		*p = 0x0000;
	}

	while (*s != '\0') {
		len = utf8_decode_lookahead(s, len);
		utf8_decode_char(s, len, &retlen, TRUE);
		s += retlen;
		len -= retlen;
		p++;
	}

	return p - out;
}

/**
 * Converts a UTF-32 encoded string to a UTF-8 encoded string. The
 * target string ``out'' is always be zero-terminated unless ``size''
 * is zero.
 *
 * @param in the UTF-32 input string.
 * @param out the target buffer for converted UTF-8 string.
 * @param size the length of the outbuf buffer in bytes.
 *        Whether the buffer was too small can be checked by comparing
 *        ``size'' with the return value. The value of ``size'' MUST NOT
 *        exceed INT_MAX.
 *
 * @returns the length in bytes of completely converted string.
 */
size_t
utf32_to_utf8(const guint32 *in, gchar *out, size_t size)
{
	const guint32 *s = in;
	gchar *p = out;
	guint retlen;
	guint32 uc;
	gchar utf8_buf[7];

	g_assert(in != NULL);
	g_assert(size == 0 || out != NULL);
	g_assert(size <= INT_MAX);

	if (size-- > 0) {
		do {
			uc = *s;
			retlen = utf8_encode_char(uc, utf8_buf);
			if (uc == 0x0000 || retlen > size)
				break;
			memcpy(p, utf8_buf, retlen);
			p += retlen;
			size -= retlen;
			s++;
		} while (size > 0);
		*p = '\0';
	}

	while (0x0000 != (uc = *s++))
		p += utf8_encoded_char_len(uc);

	return p - out;
}

/**
 * The equivalent of g_strdup() for UTF-32 strings.
 */
guint32 *
utf32_strdup(const guint32 *s)
{
	guint32 *p;
	size_t n;

	if (!s)
		return NULL; /* Just because g_strdup() does it like this */
	
	n = (1 + utf32_strlen(s)) * sizeof *p;
	p = g_malloc(n);
	memcpy(p, s, n);
	return p;
}

/**
 * Looks up the decomposed string for an UTF-32 character.
 *
 * @param uc the unicode character to look up.
 * @param nfkd if TRUE, compatibility composition is used, otherwise
 *			canonical composition.
 *
 * @returns NULL if the character is not in decomposition table. Otherwise,
 *          the returned pointer points to a possibly unterminated UTF-32
 *			string of maximum UTF32_NFD_REPLACE_MAXLEN characters. The result
 *			is constant.
 */
static const guint32 *
utf32_decompose_lookup(guint32 uc, gboolean nfkd)
{
	/* utf32_nfkd_lut contains UTF-32 strings, so we return a pointer
	 * to the respective entry instead of copying the string */
#define GET_ITEM(i) (utf32_nfkd_lut[(i)].c & ~UTF32_F_MASK)
#define FOUND(i) G_STMT_START { \
	return utf32_nfkd_lut[(i)].c & (nfkd ? 0 : UTF32_F_NFKD) \
		? NULL \
		: utf32_nfkd_lut[(i)].d; \
	/* NOTREACHED */ \
} G_STMT_END
	
	/* Perform a binary search to find ``uc'' */
	BINARY_SEARCH(guint32, uc, G_N_ELEMENTS(utf32_nfkd_lut), CMP,
		GET_ITEM, FOUND);
	
#undef FOUND
#undef GET_ITEM
	return NULL;
}

/**
 * Looks up the simple uppercase variant of an UTF-32 character.
 *
 * @return the uppercase variant of ``uc'' or ``uc'' itself.
 */

static guint32
utf32_uppercase(guint32 uc)
{
	if (uc < 0x80)
		return is_ascii_lower(uc) ? (guint32) ascii_toupper(uc) : uc;

#define GET_ITEM(i) (utf32_uppercase_lut[(i)].lower)
#define FOUND(i) G_STMT_START { \
	return utf32_uppercase_lut[(i)].upper; \
	/* NOTREACHED */ \
} G_STMT_END
	
	/* Perform a binary search to find ``uc'' */
	BINARY_SEARCH(guint32, uc, G_N_ELEMENTS(utf32_uppercase_lut), CMP,
		GET_ITEM, FOUND);
	
#undef FOUND
#undef GET_ITEM

	/* Deseret block */
	if (uc >= 0x10428 && uc <= 0x1044F)
		return uc - 0x28;

	return uc; /* not found */
}

/**
 * Looks up the simple lowercase variant of an UTF-32 character.
 *
 * @return the lowercase variant of ``uc'' or ``uc'' itself.
 */
guint32
utf32_lowercase(guint32 uc)
{
	if (uc < 0x80)
		return is_ascii_upper(uc) ? (guint32) ascii_tolower(uc) : uc;

#define GET_ITEM(i) (utf32_lowercase_lut[(i)].upper)
#define FOUND(i) G_STMT_START { \
	return utf32_lowercase_lut[(i)].lower; \
	/* NOTREACHED */ \
} G_STMT_END
	
	/* Perform a binary search to find ``uc'' */
	BINARY_SEARCH(guint32, uc, G_N_ELEMENTS(utf32_lowercase_lut), CMP,
		GET_ITEM, FOUND);
	
#undef FOUND
#undef GET_ITEM

	/* Deseret block */
	if (uc >= 0x10400 && uc <= 0x10427)
		return uc + 0x28;

	return uc; /* not found */
}

/**
 * Checks whether an UTF-32 string is in canonical order.
 */
gboolean
utf32_canonical_sorted(const guint32 *src)
{
	guint32 uc;
	guint prev, cc;

	for (prev = 0; 0 != (uc = *src++); prev = cc) {
		cc = utf32_combining_class(uc);
		if (cc != 0 && prev > cc)
			return FALSE;
	}

	return TRUE;
}

/**
 * Puts an UTF-32 string into canonical order.
 */
guint32 *
utf32_sort_canonical(guint32 *src)
{
	guint32 *s = src, *stable = src, uc;
	guint prev, cc;

	for (prev = 0; 0 != (uc = *s); prev = cc) {
		cc = utf32_combining_class(uc);
		if (cc == 0) {
			stable = s++;
		} else if (prev <= cc) {
			s++;
		} else {
			guint32 *p;
			
			while (0 != utf32_combining_class(*++s))
				;

			/* Use insertion sort because we need a stable sort algorithm */
			for (p = &stable[1]; p != s; p++) {
				guint32 *q;

				uc = *p;
				cc = utf32_combining_class(uc);

				for (q = p; q != stable; q--) {
					guint32 uc2;

					uc2 = *(q - 1);
					if (cc >= utf32_combining_class(uc2))
						break;

					g_assert(q != s);
					*q = uc2;
				}

				g_assert(q != s);
				*q = uc;
			}
			
			stable = s;
			cc = 0;
		}
	}

	return src;
}

/**
 * Checks whether an UTF-32 encoded string is in canonical order.
 */
gboolean
utf8_canonical_sorted(const gchar *src)
{
	guint prev, cc;
	size_t len = 0;

	for (prev = 0; '\0' != *src; prev = cc) {
		guint32 uc;
		gint retlen;

		len = utf8_decode_lookahead(src, len);
		uc = utf8_decode_char(src, len, &retlen, FALSE);
		if (uc == 0x0000)
			break;
		
		cc = utf32_combining_class(uc);
		if (cc != 0 && prev > cc)
			return FALSE;
		
		src += retlen;
		len -= retlen;
	}

	return TRUE;
}

/**
 * Puts an UTF-32 encoded string into canonical order.
 */
gchar *
utf8_sort_canonical(gchar *src)
{
	guint32 *buf32, *d, a[1024];
	size_t size8, size32, n;

	/* XXX: Sorting combine characters is rather heavy with UTF-8 encoding
	 *		because the characters have variable byte lengths. Therefore
	 *		and for simplicity, the whole string is temporarily converted
	 *		to UTF-32 and then put into canonical order. An optimization
	 *		could be converting only between stable code points. However,
	 *		in the worst case, that's still the whole string.
	 */
	
	size8 = 1 + strlen(src);
	size32 = 1 + utf8_to_utf32(src, NULL, 0);

	/* Use an auto buffer for reasonably small strings */
	if (size32 > G_N_ELEMENTS(a)) {
		d = g_malloc(size32 * sizeof *buf32);
		buf32 = d;
	} else {
		d = NULL;
		buf32 = a;
	}
	
	n = utf8_to_utf32(src, buf32, size32);
	g_assert(n == size32 - 1);
	utf32_sort_canonical(buf32);
	n = utf32_to_utf8(buf32, src, size8);
	g_assert(n == size8 - 1);
	
	G_FREE_NULL(d);

	return src;
}

/**
 * Decomposes a Hangul character.
 *
 * @param uc must be a Hangul character
 * @param buf must be at least three elements large
 * @return a pointer to the last written character in buf
 */
static inline guint32 *
utf32_decompose_hangul_char(guint32 uc, guint32 *buf)
{
	/*
	 * Take advantage of algorithmic Hangul decomposition to reduce
	 * the size of the lookup table drastically. See also:
	 *
	 * 		http://www.unicode.org/reports/tr15/#Hangul
	 */
#define T_COUNT 28
#define V_COUNT 21
#define N_COUNT (T_COUNT * V_COUNT)
	static const guint32 l_base = 0x1100;
	static const guint32 v_base = 0x1161;
	static const guint32 t_base = 0x11A7;
	const guint32 i = uc - UNI_HANGUL_FIRST;
	guint32 l = l_base + i / N_COUNT;
	guint32 v = v_base + (i % N_COUNT) / T_COUNT;
	guint32 t = t_base + i % T_COUNT;

	*buf++ = l;
	*buf++ = v;

	if (t != t_base) {
		*buf++ = t;
	}
#undef N_COUNT
#undef V_COUNT
#undef T_COUNT
	return buf;
}

/**
 * Composes all Hangul characters in a string.
 */
static inline size_t
utf32_compose_hangul(guint32 *src)
{
#define L_COUNT 19
#define T_COUNT 28
#define V_COUNT 21
#define N_COUNT (T_COUNT * V_COUNT)
#define S_COUNT (L_COUNT * N_COUNT)
	static const guint32 l_base = 0x1100;
	static const guint32 v_base = 0x1161;
	static const guint32 t_base = 0x11A7;
	static const guint32 s_base = 0xAC00;
	guint32 uc, prev, *p, *s = src;

	if (0 == (prev = *s))
		return 0;

	for (p = ++s; 0 != (uc = *s); s++) {
		gint l_index, s_index;
	
		l_index	= prev - l_base;
		if (0 <= l_index && l_index < L_COUNT) {
			gint v_index = uc - v_base;

			if (0 <= v_index && v_index < V_COUNT) {
				prev = s_base + (l_index * V_COUNT + v_index) * T_COUNT;
				*(p - 1) = prev;
				continue;
			}
		}

		s_index = prev - s_base;
		if (0 <= s_index && s_index < S_COUNT && 0 == (s_index % T_COUNT)) {
			gint t_index = uc - t_base;

			if (0 < t_index && t_index < T_COUNT) {
				prev += t_index;
				*(p - 1) = prev;
				continue;
			}
		}

		prev = uc;
		*p++ = uc;
	}
	
#undef N_COUNT
#undef V_COUNT
#undef T_COUNT
#undef L_COUNT
	
	*p = 0x0000;
	return p - src;
}

/**
 * Decomposes a single UTF-32 character. This must be used iteratively
 * to gain the complete decomposition.
 *
 * @param uc the UTF-32 to decompose.
 * @param len the variable ``len'' points to will be set to
 *        length in characters (not bytes!) of decomposed string. This is
 *        important because the decomposed string is not zero-terminated.
 * @param nfkd if TRUE, compatibility composition is used, otherwise
 *			canonical composition.
 *
 * @returns a pointer to a buffer holding the decomposed string.
 *			The buffer is unterminated. The maximum length is
 *			UTF32_NFKD_REPLACE_MAXLEN characters. The returned pointer points
 *			to a static buffer which might get overwritten by subsequent
 *			calls to this function.
 */
static inline const guint32 *
utf32_decompose_single_char(guint32 uc, size_t *len, gboolean nfkd)
{
	static guint32 buf[3];
	guint32 *p = buf;
	const guint32 *q;

	if (UNICODE_IS_ASCII(uc)) {
		*p++ = uc;
	} else if (UNICODE_IS_HANGUL(uc)) {
		p = utf32_decompose_hangul_char(uc, p);
	} else if (NULL != (q = utf32_decompose_lookup(uc, nfkd))) {
		*len = utf32_strmaxlen(q, UTF32_NFKD_REPLACE_MAXLEN);
		return q;
	} else {
		*p++ = uc;
	}

	g_assert(p > buf && p < &buf[sizeof buf]);
	*len = p - buf;
	return buf;
}

/**
 * Decomposes an UTF-32 character completely.
 *
 * @param uc the UTF-32 to decompose.
 * @param len the variable ``len'' points to will be set to
 *        length in characters (not bytes!) of decomposed string. This is
 *        important because the decomposed string is not zero-terminated.
 * @param nfkd if TRUE, compatibility composition is used, otherwise
 *			canonical composition.
 *
 * @returns a pointer to a buffer holding the decomposed string.
 *			The buffer is unterminated. The maximum length is
 *			UTF32_NFKD_REPLACE_MAXLEN characters. The returned pointer points
 *			to a static buffer which might get overwritten by subsequent
 *			calls to this function.
 */
static inline const guint32 *
utf32_decompose_char(guint32 uc, size_t *len, gboolean nfkd)
{
	static guint32 buf[2][256];
	const guint32 *old;
	guint32 *p, *cur;
	size_t size, start;

	old = utf32_decompose_single_char(uc, &size, nfkd);
	if (1 == size && uc == old[0]) {
		*len = 1;
		return old;
	}

	cur = buf[0];
	/* This must be copied because the next call to
	 * utf32_decompose_nfkd_char_single() might modify
	 * the buffer that ``old'' points to.
	 */
	memcpy(buf[1], old, size * sizeof *old);
	old = buf[1];
	start = 0;

	for (;;) {	
		size_t avail, i;
		const guint32 *mod;
		
		mod = NULL;
		p = &cur[start];
		avail = G_N_ELEMENTS(buf[0]) - start;

		for (i = start; i < size; i++) {
			const guint32 *q;
			size_t n;
		
			q = utf32_decompose_single_char(old[i], &n, nfkd);
			if (!mod && (n > 1 || *q != old[i]))
				mod = &old[i];
				   	
			g_assert(n <= avail);
			avail -= n;
			while (n-- > 0)
				*p++ = *q++;
		}

		if (!mod)
			break;

		start = mod - old;
		size = p - cur;

		/* swap ``cur'' and ``old'' for next round */
		old = cur;
		cur = cur == buf[0] ? buf[1] : buf[0];
	}

	*len = size;
	return old;
}

/**
 * Determines the length of an UTF-32 string.
 *
 * @param s a NUL-terminated UTF-32 string.
 * @returns the length in characters (not bytes!) of the string ``s''.
 */
size_t
utf32_strlen(const guint32 *s)
{
	const guint32 *p = s;

	g_assert(s != NULL);

	while (*p != 0x0000)
		p++;

	return p - s;
}

/**
 * Determines the length of a UTF-32 string inspecting at most ``maxlen''
 * characters (not bytes!). This can safely be used with unterminated UTF-32
 * strings if ``maxlen'' has an appropriate value.
 *
 * To detect whether the actual string is longer than ``maxlen'' characters,
 * just check if ``string[maxlen]'' is 0x0000, if and only if the returned
 * value equals maxlen. Otherwise, the returned value is indeed the
 * complete length of the UTF-32 string.
 *
 * @param s an UTF-32 string.
 * @param maxlen the maximum number of characters to inspect.
 *
 * @returns the length in characters (not bytes!) of the string ``s''.
 */
size_t
utf32_strmaxlen(const guint32 *s, size_t maxlen)
{
	const guint32 *p = s;

	g_assert(s != NULL);
	g_assert(maxlen <= INT_MAX);

	while (maxlen-- > 0 && *p != 0x0000) {
		p++;
	}

	return p - s;
}

/**
 * Decomposes an UTF-8 encoded string.
 *
 * The UTF-8 string written to ``dst'' is always NUL-terminated unless
 * ``size'' is zero. If the size of ``dst'' is too small to hold the
 * complete decomposed string, the resulting string will be truncated but
 * the validity of the UTF-8 encoding will be preserved. Truncation is
 * indicated by the return value being equal to or greater than ``size''.
 *
 * @param src a UTF-8 encoded string.
 * @param dst a pointer to a buffer which will hold the decomposed string.
 * @param size the number of bytes ``dst'' can hold.
 * @param nfkd if TRUE, compatibility composition is used, otherwise
 *			canonical composition.
 *
 * @returns the length in bytes (not characters!) of completely decomposed
 *			string.
 */
static inline size_t
utf8_decompose(const gchar *src, gchar *out, size_t size, gboolean nfkd)
{
	const guint32 *d;
	guint32 uc;
	gint retlen;
	size_t d_len, len = 0, new_len = 0;

	g_assert(src != NULL);
	g_assert(size == 0 || out != NULL);
	g_assert(size <= INT_MAX);

	if (size-- > 0) {
		gchar *dst = out;

		while (*src != '\0') {
			size_t utf8_len;
			gchar buf[256], utf8_buf[7], *q;

			len = utf8_decode_lookahead(src, len);
			uc = utf8_decode_char(src, len, &retlen, FALSE);
			if (uc == 0x0000)
				break;

			src += retlen;
			len -= retlen;
			d = utf32_decompose_char(uc, &d_len, nfkd);
			q = buf;
			while (d_len-- > 0) {
				utf8_len = utf8_encode_char(*d++, utf8_buf);
				g_assert((size_t) (&buf[sizeof buf] - q) >= utf8_len);
				memcpy(q, utf8_buf, utf8_len);
				q += utf8_len;
			}

			utf8_len = q - buf;
			new_len += utf8_len;
			if (new_len > size)
				break;

			memcpy(dst, buf, utf8_len);
			dst += utf8_len;
		}
		*dst = '\0';
		
		if (!utf8_canonical_sorted(out))
			utf8_sort_canonical(out);
		g_assert(utf8_canonical_sorted(out));
	}

	while (*src != '\0') {
		len = utf8_decode_lookahead(src, len);
		uc = utf8_decode_char(src, len, &retlen, FALSE);
		if (uc == 0x0000)
			break;
		
		src += retlen;
		len -= retlen;
		d = utf32_decompose_char(uc, &d_len, nfkd);
		while (d_len-- > 0)
			new_len += utf8_encoded_char_len(*d++);
	}

	return new_len;
}

/**
 * Decomposes (NFD) an UTF-8 encoded string.
 *
 * The UTF-8 string written to ``dst'' is always NUL-terminated unless
 * ``size'' is zero. If the size of ``dst'' is too small to hold the
 * complete decomposed string, the resulting string will be truncated but
 * the validity of the UTF-8 encoding will be preserved. Truncation is
 * indicated by the return value being equal to or greater than ``size''.
 *
 * @param src a UTF-8 encoded string.
 * @param dst a pointer to a buffer which will hold the decomposed string.
 * @param size the number of bytes ``dst'' can hold.
 *
 * @returns the length in bytes (not characters!) of completely decomposed
 *			string.
 */
size_t
utf8_decompose_nfd(const gchar *src, gchar *out, size_t size)
{
	return utf8_decompose(src, out, size, FALSE);
}

/**
 * Decomposes (NFKD) an UTF-8 encoded string.
 *
 * The UTF-8 string written to ``dst'' is always NUL-terminated unless
 * ``size'' is zero. If the size of ``dst'' is too small to hold the
 * complete decomposed string, the resulting string will be truncated but
 * the validity of the UTF-8 encoding will be preserved. Truncation is
 * indicated by the return value being equal to or greater than ``size''.
 *
 * @param src a UTF-8 encoded string.
 * @param dst a pointer to a buffer which will hold the decomposed string.
 * @param size the number of bytes ``dst'' can hold.
 *
 * @returns the length in bytes (not characters!) of completely decomposed
 *			string.
 */
size_t
utf8_decompose_nfkd(const gchar *src, gchar *out, size_t size)
{
	return utf8_decompose(src, out, size, TRUE);
}

/**
 * Decomposes an UTF-32 encoded string.
 *
 */
static inline size_t
utf32_decompose(const guint32 *in, guint32 *out, size_t size, gboolean nfkd)
{
	const guint32 *d, *s = in;
	guint32 *p = out;
	guint32 uc;
	size_t d_len;

	g_assert(in != NULL);
	g_assert(size == 0 || out != NULL);
	g_assert(size <= INT_MAX);

	if (size-- > 0) {
		for (/* NOTHING */; 0x0000 != (uc = *s); s++) {
			d = utf32_decompose_char(uc, &d_len, nfkd);
			if (d_len > size)
				break;
			size -= d_len;
			while (d_len-- > 0) {
				*p++ = *d++;
			}
		}
		*p = 0x0000;

		utf32_sort_canonical(out);
	}

	while (0x0000 != (uc = *s++)) {
		d = utf32_decompose_char(uc, &d_len, nfkd);
		p += d_len;
	}

	return p - out;
}

/**
 * Decomposes (NFD) an UTF-32 encoded string.
 *
 */
size_t
utf32_decompose_nfd(const guint32 *in, guint32 *out, size_t size)
{
	return utf32_decompose(in, out, size, FALSE);
}

/**
 * Decomposes (NFKD) an UTF-32 encoded string.
 *
 */
size_t
utf32_decompose_nfkd(const guint32 *in, guint32 *out, size_t size)
{
	return utf32_decompose(in, out, size, TRUE);
}

typedef guint32 (* utf32_remap_func)(guint32 uc);
	
/**
 * Copies the UTF-8 string ``src'' to ``dst'' remapping all characters
 * using ``remap''.
 * If the created string is as long as ``size'' or larger, the string in
 * ``dst'' will be truncated. ``dst'' is always NUL-terminated unless ``size''
 * is zero.
 * The returned value is the length of the converted string ``src''
 * regardless of the ``size'' parameter. ``src'' must be validly UTF-8
 * encoded, otherwise the string will be truncated.
 *
 * @param dst the target buffer
 * @param src an UTF-8 string
 * @param size the size of dst in bytes 
 * @param remap a function that takes a single UTF-32 character and returns
 *        a single UTF-32 character.
 * @return the length in bytes of the converted string ``src''.
 */
static size_t
utf8_remap(gchar *dst, const gchar *src, size_t size, utf32_remap_func remap)
{
	guint32 uc;
	gint retlen;
	size_t len = 0, new_len = 0;

	g_assert(dst != NULL);
	g_assert(src != NULL);
	g_assert(remap != NULL);
	g_assert(size <= INT_MAX);

	if (size-- > 0) {
		while (*src != '\0') {
			size_t utf8_len;
			gchar utf8_buf[7];

			len = utf8_decode_lookahead(src, len);
			uc = utf8_decode_char(src, len, &retlen, FALSE);
			if (uc == 0x0000)
				break;

			src += retlen;
			len -= retlen;
			uc = remap(uc);
			utf8_len = utf8_encode_char(uc, utf8_buf);
			new_len += utf8_len;
			if (new_len > size)
				break;
			
			memcpy(dst, utf8_buf, utf8_len);
			dst += utf8_len;
		}
		*dst = '\0';
	}

	while (*src != '\0') {
		len = utf8_decode_lookahead(src, len);
		uc = utf8_decode_char(src, len, &retlen, FALSE);
		if (uc == 0x0000)
			break;

		src += retlen;
		len -= retlen;
		uc = remap(uc);
		new_len += utf8_encoded_char_len(uc);
	}

	return new_len;
}

/**
 * Copies the UTF-32 string ``src'' to ``dst'' remapping all characters
 * using ``remap''.
 * If the created string is as long as ``size'' or larger, the string in
 * ``dst'' will be truncated. ``dst'' is always NUL-terminated unless ``size''
 * is zero.
 * The returned value is the length of the converted string ``src''
 * regardless of the ``size'' parameter. ``src'' must be validly UTF-8
 * encoded, otherwise the string will be truncated.
 *
 * @param dst the target buffer
 * @param src an UTF-8 string
 * @param size the size of dst in bytes 
 * @param remap a function that takes a single UTF-32 character and returns
 *        a single UTF-32 character.
 * @return the length in bytes of the converted string ``src''.
 */
static size_t
utf32_remap(guint32 *dst, const guint32 *src, size_t size,
	utf32_remap_func remap)
{
	const guint32 *s = src;
	guint32 *p = dst;

	g_assert(dst != NULL);
	g_assert(src != NULL);
	g_assert(remap != NULL);
	g_assert(size <= INT_MAX);

	if (size > 0) {
		guint32 *end, uc;
		
		end = &dst[size - 1];
		for (p = dst; p != end && 0x0000 != (uc = *s); p++, s++) {
			*p = remap(uc);
		}
		*p = 0x0000;
	}

	if (0x0000 != *s)
		p += utf32_strlen(s);

	return p - dst;
}

/**
 * Copies ``src'' to ``dst'' converting all characters to lowercase. If
 * the string is as long as ``size'' or larger, the string in ``dst'' will
 * be truncated. ``dst'' is always NUL-terminated unless ``size'' is zero.
 * The returned value is the length of the converted string ``src''
 * regardless of the ``size'' parameter.
 *
 * @param dst the target buffer
 * @param src an UTF-32 string
 * @param size the size of dst in bytes 
 * @return the length in characters of the converted string ``src''.
 */
size_t
utf32_strlower(guint32 *dst, const guint32 *src, size_t size)
{
	g_assert(dst != NULL);
	g_assert(src != NULL);
	g_assert(size <= INT_MAX);

	return utf32_remap(dst, src, size, utf32_lowercase);
}	

/**
 * Copies ``src'' to ``dst'' converting all characters to uppercase. If
 * the string is as long as ``size'' or larger, the string in ``dst'' will
 * be truncated. ``dst'' is always NUL-terminated unless ``size'' is zero.
 * The returned value is the length of the converted string ``src''
 * regardless of the ``size'' parameter.
 *
 * @param dst the target buffer
 * @param src an UTF-32 string
 * @param size the size of dst in bytes 
 * @return the length in characters of the converted string ``src''.
 */
size_t
utf32_strupper(guint32 *dst, const guint32 *src, size_t size)
{
	g_assert(dst != NULL);
	g_assert(src != NULL);
	g_assert(size <= INT_MAX);

	return utf32_remap(dst, src, size, utf32_uppercase);
}	

/**
 * Copies ``src'' to ``dst'' converting all characters to lowercase. If
 * the string is as long as ``size'' or larger, the string in ``dst'' will
 * be truncated. ``dst'' is always NUL-terminated unless ``size'' is zero.
 * The returned value is the length of the converted string ``src''
 * regardless of the ``size'' parameter. ``src'' must be validly UTF-8
 * encoded, otherwise the string will be truncated.
 *
 * @param dst the target buffer
 * @param src an UTF-8 string
 * @param size the size of dst in bytes 
 * @return the length in bytes of the converted string ``src''.
 */
size_t
utf8_strlower(gchar *dst, const gchar *src, size_t size)
{
	g_assert(dst != NULL);
	g_assert(src != NULL);
	g_assert(size <= INT_MAX);

	return utf8_remap(dst, src, size, utf32_lowercase);
}	

/**
 * Copies ``src'' to ``dst'' converting all characters to uppercase. If
 * the string is as long as ``size'' or larger, the string in ``dst'' will
 * be truncated. ``dst'' is always NUL-terminated unless ``size'' is zero.
 * The returned value is the length of the converted string ``src''
 * regardless of the ``size'' parameter. ``src'' must be validly UTF-8
 * encoded, otherwise the string will be truncated.
 *
 * @param dst the target buffer
 * @param src an UTF-8 string
 * @param size the size of dst in bytes 
 * @return the length in bytes of the converted string ``src''.
 */
size_t
utf8_strupper(gchar *dst, const gchar *src, size_t size)
{
	g_assert(dst != NULL);
	g_assert(src != NULL);
	g_assert(size <= INT_MAX);

	return utf8_remap(dst, src, size, utf32_uppercase);
}	

/**
 * Copies the UTF-8 string ``src'' to a newly allocated buffer converting all
 * characters to lowercase.
 *
 * @param src an UTF-8 string
 * @return a newly allocated buffer containing the lowercased string. 
 */
gchar *
utf8_strlower_copy(const gchar *src)
{
	gchar c, *dst;
	size_t len, size;

	g_assert(src != NULL);
	
	len = utf8_strlower(&c, src, sizeof c);
	g_assert(c == '\0');
	size = len + 1;
	dst = g_malloc(size);
	len = utf8_strlower(dst, src, size);
	g_assert(size == len + 1);
	g_assert(len == strlen(dst));

	return dst;
}

/**
 * Copies the UTF-8 string ``src'' to a newly allocated buffer converting all
 * characters to uppercase.
 *
 * @param src an UTF-8 string
 * @return a newly allocated buffer containing the uppercased string. 
 */
gchar *
utf8_strupper_copy(const gchar *src)
{
	gchar c, *dst;
	size_t len, size;

	g_assert(src != NULL);
	
	len = utf8_strupper(&c, src, sizeof c);
	g_assert(c == '\0');
	size = len + 1;
	dst = g_malloc(size);
	len = utf8_strupper(dst, src, size);
	g_assert(size == len + 1);
	g_assert(len == strlen(dst));

	return dst;
}

/**
 * Filters characters that are ignorable for query strings. *space
 * should be initialized to TRUE for the first character of a string.
 * ``space'' is used to prevent adding multiple space characters i.e.,
 * a space should not be followed by a space.
 *
 * @param uc an UTF-32 character
 * @param space pointer to a gboolean holding the current space state
 * @param last should be TRUE if ``uc'' is the last character of the string.
 * @return	zero if the character should be skipped, otherwise the
 *			character itself or a replacement character.
 */
static inline guint32
utf32_filter_char(guint32 uc, gboolean *space, gboolean last)
{
	uni_gc_t gc;

	g_assert(space != NULL);

	gc = utf32_general_category(uc);
	switch (gc) {
	case UNI_GC_LETTER_LOWERCASE:
	case UNI_GC_LETTER_OTHER:
	case UNI_GC_LETTER_MODIFIER:
	case UNI_GC_NUMBER_DECIMAL:
	case UNI_GC_OTHER_NOT_ASSIGNED:
		*space = FALSE;
		return uc;

	case UNI_GC_OTHER_CONTROL:
		if (uc == '\n')
			return uc;
		break;

	case UNI_GC_MARK_NONSPACING:
		/* Do not skip the japanese " and � kana marks and so on */
		switch (uc) {
		/* Japanese voiced sound marks */
		case 0x3099:
		case 0x309A:
			/* Virama signs */
		case 0x0BCD:
		case 0x094D:
		case 0x09CD:
		case 0x0A4D:
		case 0x0ACD:
		case 0x0B4D:
		case 0x0CCD:
		case 0x1039:
		case 0x1714:
		case 0x0C4D:
			/* Nukta signs */
		case 0x093C:
		case 0x09BC:
		case 0x0A3C:
		case 0x0ABC:
		case 0x0B3C:
		case 0x0CBC:
			/* Greek Ypogegrammeni */
		case 0x0345:
			/* Tibetan */
		case 0x0F71:
		case 0x0F72:
		case 0x0F7A:
		case 0x0F7B:
		case 0x0F7C:
		case 0x0F7D:
		case 0x0F80:
		case 0x0F74:
		case 0x0F39:
		case 0x0F18:
		case 0x0F19:
		case 0x0F35:
		case 0x0F37:
		case 0x0FC6:
		case 0x0F82:
		case 0x0F83:
		case 0x0F84:
		case 0x0F86:
		case 0x0F87:

			/* Others : not very sure we must keep them or not ... */

			/* Myanmar */
		case 0x1037:
			/* Sinhala */
		case 0x0DCA:
			/* Thai */
		case 0x0E3A:
			/* Hanundo */
		case 0x1734:
			/* Devanagari */
		case 0x0951:
		case 0x0952:
			/* Lao */
		case 0x0EB8:
		case 0x0EB9:
			/* Limbu */
		case 0x193B:
		case 0x1939:
		case 0x193A:
			/* Mongolian */
		case 0x18A9:
			return uc;
		}
		break;

	case UNI_GC_PUNCT_OTHER:
	/* XXX: Disabled for backwards compatibility. Especially '.' is
	 *		problematic because filename extensions are not separated
	 *		from the rest of the name otherwise. Also, some people use
	 *		dots instead of spaces in filenames. */
#if 0
		if ('\'' == uc || '*' == uc || '.' == uc)
			return uc;
		/* FALLTHRU */
#endif
		
	case UNI_GC_LETTER_UPPERCASE:
	case UNI_GC_LETTER_TITLECASE:

	case UNI_GC_MARK_SPACING_COMBINE:
	case UNI_GC_MARK_ENCLOSING:

	case UNI_GC_SEPARATOR_PARAGRAPH:
	case UNI_GC_SEPARATOR_LINE:
	case UNI_GC_SEPARATOR_SPACE:

	case UNI_GC_NUMBER_LETTER:
	case UNI_GC_NUMBER_OTHER:

	case UNI_GC_OTHER_FORMAT:
	case UNI_GC_OTHER_PRIVATE_USE:
	case UNI_GC_OTHER_SURROGATE:

	case UNI_GC_PUNCT_DASH:
	case UNI_GC_PUNCT_OPEN:
	case UNI_GC_PUNCT_CLOSE:
	case UNI_GC_PUNCT_CONNECTOR:
	case UNI_GC_PUNCT_INIT_QUOTE:
	case UNI_GC_PUNCT_FINAL_QUOTE:

	case UNI_GC_SYMBOL_MATH:
	case UNI_GC_SYMBOL_CURRENCY:
	case UNI_GC_SYMBOL_MODIFIER:
	case UNI_GC_SYMBOL_OTHER:
		{
			gboolean prev = *space;
			
			*space = TRUE;
			return prev || last ? 0 : 0x0020;
		}
	}

	return 0;
}

/**
 * Remove all the non letter and non digit by looking the unicode symbol type
 * all other characters will be reduce to normal space
 * try to merge continues spaces in the same time
 * keep the important non spacing marks
 *
 * @param src an NUL-terminated UTF-32 string.
 * @param dst the output buffer to hold the modified UTF-32 string.
 * @param size the number of characters (not bytes!) dst can hold.
 * @return The length of the output string.
 */
size_t
utf32_filter(const guint32 *src, guint32 *dst, size_t size)
{
	const guint32 *s;
	guint32 uc, *p;
	gboolean space = TRUE; /* prevent adding leading space */

	g_assert(src != NULL);
	g_assert(size == 0 || dst != NULL);
	g_assert(size <= INT_MAX);

	s = src;
	p = dst;
	
	if (size > 0) {
		guint32 *end;
		
		for (end = &dst[size - 1]; p != end && 0x0000 != (uc = *s); s++) {
			if (0 != (uc = utf32_filter_char(uc, &space, 0x0000 == s[1])))
				*p++ = uc;
		}
		*p = 0x0000;
	}

	while (0x0000 != (uc = *s++)) {
		if (0 != utf32_filter_char(uc, &space, 0x0000 == *s))
			p++;
	}

	return p - dst;
}

#ifdef USE_ICU

/**
 * Convert a string from the locale encoding to internal ICU encoding (UTF-16)
 */
int
locale_to_icu_conv(const gchar *in, int lenin, UChar *out, int lenout)
{
	UErrorCode error = U_ZERO_ERROR;
	int r;

	g_assert(use_icu);
	r = ucnv_toUChars(conv_icu_locale, out, lenout, in, lenin, &error);

	return (error != U_ZERO_ERROR && error != U_BUFFER_OVERFLOW_ERROR) ? 0 : r;
}

/**
 * Convert a string from UTF-8 encoding to internal ICU encoding (UTF-16)
 */
int
utf8_to_icu_conv(const gchar *in, int lenin, UChar *out, int lenout)
{
	UErrorCode error = U_ZERO_ERROR;
	int r;

	g_assert(use_icu);
	r = ucnv_toUChars(conv_icu_utf8, out, lenout, in, lenin, &error);

	return (error != U_ZERO_ERROR && error != U_BUFFER_OVERFLOW_ERROR) ? 0 : r;
}

/**
 * Convert a string from ICU encoding (UTF-16) to UTF8 encoding (fast)
 */
int
icu_to_utf8_conv(const UChar *in, int lenin, gchar *out, int lenout)
{
	UErrorCode error = U_ZERO_ERROR;
	int r;

	g_assert(use_icu);
	r = ucnv_fromUChars(conv_icu_utf8, out, lenout, in, lenin, &error);

	return (error != U_ZERO_ERROR && error != U_BUFFER_OVERFLOW_ERROR &&
			error != U_STRING_NOT_TERMINATED_WARNING) ? 0 : r;
}

/**
 * Compact a string as specified in unicode
 */
int
unicode_NFC(const UChar *source, gint32 len, UChar *result, gint32 rlen)
{
	UErrorCode error = U_ZERO_ERROR;
	int r;

	g_assert(use_icu);
	r = unorm_normalize(source, len, UNORM_NFC, 0, result, rlen, &error);

	return (error != U_ZERO_ERROR && error != U_BUFFER_OVERFLOW_ERROR) ? 0 : r;
}

/**
 * Compact a string as specified in unicode
 */
int
unicode_NFKC(const UChar *source, gint32 len, UChar *result, gint32 rlen)
{
	UErrorCode error = U_ZERO_ERROR;
	int r;

	g_assert(use_icu);
	r = unorm_normalize(source, len, UNORM_NFKC, 0, result, rlen, &error);

	return (error != U_ZERO_ERROR && error != U_BUFFER_OVERFLOW_ERROR) ? 0 : r;
}

/**
 * Expand and K a string as specified in unicode
 * K will transform special character in the standard form
 * for instance : The large japanese space will be transform to a normal space
 */
int
unicode_NFKD(const UChar *source, gint32 len, UChar *result, gint32 rlen)
{
	UErrorCode error = U_ZERO_ERROR;
	int r;

	g_assert(use_icu);
	r = unorm_normalize (source, len, UNORM_NFKD, 0, result, rlen, &error);

	return (error != U_ZERO_ERROR && error != U_BUFFER_OVERFLOW_ERROR) ? 0 : r;
}

int
unicode_NFD(const UChar *source, gint32 len, UChar *result, gint32 rlen)
{
	UErrorCode error = U_ZERO_ERROR;
	int r;

	g_assert(use_icu);
	r = unorm_normalize (source, len, UNORM_NFD, 0, result, rlen, &error);

	return (error != U_ZERO_ERROR && error != U_BUFFER_OVERFLOW_ERROR) ? 0 : r;
}

/**
 * Upper case a string
 * This is usefull to transorm the german sset to SS
 * Note : this will not transform hiragana to katakana
 */
int
unicode_upper(const UChar *source, gint32 len, UChar *result, gint32 rlen)
{
	UErrorCode error = U_ZERO_ERROR;
	int r;

	g_assert(use_icu);
	r = u_strToUpper(result, rlen, source, len, NULL, &error);

	return (error != U_ZERO_ERROR && error != U_BUFFER_OVERFLOW_ERROR) ? 0 : r;
}

/**
 * Lower case a string
 */
int
unicode_lower(const UChar *source, gint32 len, UChar *result, gint32 rlen)
{
	UErrorCode error = U_ZERO_ERROR;
	int r;

	g_assert(use_icu);
	r = u_strToLower(result, rlen, source, len, NULL, &error);

	return (error != U_ZERO_ERROR && error != U_BUFFER_OVERFLOW_ERROR) ? 0 : r;
}

/**
 * Remove all the non letter and non digit by looking the unicode symbol type
 * all other characters will be reduce to normal space
 * try to merge continues spaces in the same time
 * keep the important non spacing marks
 */
int
unicode_filters(const UChar *source, gint32 len, UChar *result)
{
	int i, j;
	int space = 1;

	g_assert(use_icu);

	for (i = 0, j = 0; i < len; i++) {
		UChar uc = source[i];
		
		switch (u_charType(uc)) {
		case U_LOWERCASE_LETTER :
		case U_OTHER_LETTER :
		case U_MODIFIER_LETTER :
		case U_DECIMAL_DIGIT_NUMBER :
		case U_UNASSIGNED :
			result[j++] = uc;
			space = 0;
			break;

		case U_CONTROL_CHAR :
			if (uc == '\n')
				result[j++] = uc;
			break;

		case U_NON_SPACING_MARK :
			/* Do not skip the japanese " and � kana marks and so on */

			switch (uc) {
				/* Japanese voiced sound marks */
			case 0x3099:
			case 0x309A:
				/* Virama signs */
			case 0x0BCD:
			case 0x094D:
			case 0x09CD:
			case 0x0A4D:
			case 0x0ACD:
			case 0x0B4D:
			case 0x0CCD:
			case 0x1039:
			case 0x1714:
			case 0x0C4D:
				/* Nukta signs */
			case 0x093C:
			case 0x09BC:
			case 0x0A3C:
			case 0x0ABC:
			case 0x0B3C:
			case 0x0CBC:
				/* Greek Ypogegrammeni */
			case 0x0345:
				/* Tibetan */
			case 0x0F71:
			case 0x0F72:
			case 0x0F7A:
			case 0x0F7B:
			case 0x0F7C:
			case 0x0F7D:
			case 0x0F80:
			case 0x0F74:
			case 0x0F39:
			case 0x0F18:
			case 0x0F19:
			case 0x0F35:
			case 0x0F37:
			case 0x0FC6:
			case 0x0F82:
			case 0x0F83:
			case 0x0F84:
			case 0x0F86:
			case 0x0F87:

		/* Others : not very sure we must keep them or not ... */

				/* Myanmar */
			case 0x1037:
				/* Sinhala */
			case 0x0DCA:
				/* Thai */
			case 0x0E3A:
				/* Hanundo */
			case 0x1734:
				/* Devanagari */
			case 0x0951:
			case 0x0952:
				/* Lao */
			case 0x0EB8:
			case 0x0EB9:
				/* Limbu */
			case 0x193B:
			case 0x1939:
			case 0x193A:
				/* Mongolian */
			case 0x18A9:
				result[j++] = uc;
			}
			break;

		case U_OTHER_PUNCTUATION :
	/* XXX: Disabled for backwards compatibility. Especially '.' is
	 *		problematic because filename extensions are not separated
	 *		from the rest of the name otherwise. Also, some people use
	 *		dots instead of spaces in filenames. */
#if 0
			if ('\'' == uc || '*' == uc || '.' == uc) {
				result[j++] = uc;
				break;
			}
			/* FALLTHRU */
#endif

		case U_UPPERCASE_LETTER :
		case U_TITLECASE_LETTER :
		case U_PARAGRAPH_SEPARATOR :
		case U_COMBINING_SPACING_MARK :
		case U_LINE_SEPARATOR :
		case U_LETTER_NUMBER :
		case U_OTHER_NUMBER :
		case U_SPACE_SEPARATOR :
		case U_FORMAT_CHAR :
		case U_PRIVATE_USE_CHAR :
		case U_SURROGATE :
		case U_DASH_PUNCTUATION :
		case U_START_PUNCTUATION :
		case U_END_PUNCTUATION :
		case U_CONNECTOR_PUNCTUATION :
		case U_MATH_SYMBOL :
		case U_CURRENCY_SYMBOL :
		case U_MODIFIER_SYMBOL :
		case U_OTHER_SYMBOL :
		case U_INITIAL_PUNCTUATION :
		case U_FINAL_PUNCTUATION :
		case U_CHAR_CATEGORY_COUNT :
			if (0 == space && 0x0000 != source[i + 1])
				result[j++] = 0x0020;
			space = 1;
			break;
		}
	}
	return j;
}

/**
 * Apply the NFKD/NFC algo to have nomalized keywords
 * The string `in' MUST be valid UTF-8 or that function would return rubbish.
 */
gchar *
unicode_canonize(const gchar *in)
{
	UChar *qtmp1;
	UChar *qtmp2;
	int	len, maxlen;
	gchar *out;

	g_assert(use_icu);

	len = strlen(in);
	maxlen = (len + 1) * 6; /* Max 6 bytes for one char in utf8 */

	g_assert(utf8_is_valid_string(in, len));

	qtmp1 = (UChar *) g_malloc(maxlen * sizeof(UChar));
	qtmp2 = (UChar *) g_malloc(maxlen * sizeof(UChar));

	len = utf8_to_icu_conv(in, len, qtmp2, maxlen);
	len = unicode_NFKD(qtmp2, len, qtmp1, maxlen);
	len = unicode_upper(qtmp1, len, qtmp2, maxlen);
	len = unicode_lower(qtmp2, len, qtmp1, maxlen);
	len = unicode_filters(qtmp1, len, qtmp2);
	len = unicode_NFC(qtmp2, len, qtmp1, maxlen);

	out = g_malloc(len + 1);
	len = icu_to_utf8_conv(qtmp1, len, out, len);
	out[len] = '\0';

	G_FREE_NULL(qtmp1);
	G_FREE_NULL(qtmp2);

	return out;
}

#endif	/* USE_ICU */

/**
 * @return	TRUE if ICU was successfully initialized. If FALSE is returned
 *			none of the ICU-related functions must be used.
 */
gboolean
icu_enabled(void)
{
	return use_icu;
}

/*
 * Is the locale using the latin alphabet?
 */
gboolean
is_latin_locale(void)
{
	return latin_locale;
}

static GHashTable *utf32_compose_roots;

/**
 * Finds the composition of two UTF-32 characters.
 *
 * @param a an UTF-32 character (should be a starter)
 * @param b an UTF-32 character
 *
 * @return	zero if there's no composition for the characters. Otherwise,
 *			the composed character is returned.
 */
static guint32
utf32_compose_char(guint32 a, guint32 b)
{
	GSList *sl;
	gpointer key;
	
	key = GUINT_TO_POINTER(a);
	sl = g_hash_table_lookup(utf32_compose_roots, key);
	for (/* NOTHING */; sl; sl = g_slist_next(sl)) {
		guint i;
		guint32 c;

		i = GPOINTER_TO_UINT(sl->data);
		c = utf32_nfkd_lut[i].d[1];
		if (b == c) {
			return utf32_nfkd_lut[i].c & ~UTF32_F_MASK;
		} else if (b < c) {
			/* The lists are sorted */
			break;
		}
	}

	return 0;
}

/**
 * Finds the next ``starter'' character (combining class zero) in the
 * string starting at ``s''. Note that NUL is also a ``starter''.
 *
 * @param s an NUL-terminated UTF-32 string.
 * @return a pointer to the next ``starter'' character in ``s''.
 */
static inline guint32 *
utf32_next_starter(const guint32 *s)
{
	while (0 != utf32_combining_class(*s))
		s++;
	return deconstify_guint32(s);
}

/**
 * Composes an UTF-32 encoded string in-place. The modified string
 * might be shorter but is never longer than the original string.
 *
 * NB:	We assume that a direct composition, eliminates at most one
 *		character. Further, the string must be in canonical order.
 *
 * @param src an NUL-terminated UTF-32 string.
 * @return	the length in characters (not bytes!) of the possibly
 *			modified string.
 */
size_t
utf32_compose(guint32 *src)
{
	guint32 *s, *p, uc, *end;

	g_assert(src != NULL);

	s = utf32_next_starter(src);
	if (0 == *s)
		return s - src;

	/* The end of the string is determined in advance because a composition
	 * can cause a ``hole''. Instead of rejoining the string each time,
	 * the erased composite character is replaced with a NUL which is then
	 * skipped when scanning the same position again.
     */	   
	end = s + utf32_strlen(s);
	p = s;
		
	while (s != end) {
		guint32 *q = s;

		uc = *s;
		for (;;) {
			guint32 c, uc2;

			/* We must skip over previously erased characters but
			 * not run over ``end''. */
			do {	
				uc2 = *++q;
			} while (uc2 == 0 && q != end);
		
			c = utf32_compose_char(uc, uc2);
			if (!c) {
				guint cc = utf32_combining_class(uc2);
				
				if (
					0 == cc ||
					cc == utf32_combining_class(q[1])
				) {
					/* Record the final composition and go to the next
					 * write position */
					*p++ = uc;

					/* Make sure we fill possibly created holes */
					while (++s != q) {
						if (0 != (c = *s))
							*p++ = c;
					}

					break;
				}
			} else {
				*q = 0;	/* Erase the composite character */
				uc = c;	/* The preliminary composition */
				
				/* However, there might be a composition for this
				 * character as well. Thus, restart. */
				q = s;
			}
		}
	}
	*p = 0x0000;

	return p - src;
}

/**
 */
guint32 *
utf32_compose_nfc(const guint32 *src)
{
	guint32 *nfd;
	size_t size, n;
	
	/* Convert to NFKD */
	size = 1 + utf32_decompose(src, NULL, 0, FALSE);
	nfd = g_malloc(size * sizeof *nfd);
	n = utf32_decompose(src, nfd, size, FALSE);
	g_assert(size - 1 == n);

	/* Convert to NFC */
	n = utf32_compose(nfd);
	n = utf32_compose_hangul(nfd);

	return nfd;
}

/**
 */
gchar *
utf8_compose_nfc(const gchar *src)
{
	guint32 *dst32;

	g_assert(utf8_is_valid_string(src, 0));
	
	{	
		size_t size, n;
		guint32 *s;
		
		size = 1 + utf8_to_utf32(src, NULL, 0);
		s = g_malloc(size * sizeof *s);
		n = 1 + utf8_to_utf32(src, s, size);
		g_assert(n == size);
		
		dst32 = utf32_compose_nfc(s);
		if (dst32 != s)
			G_FREE_NULL(s);
	}

	{
		size_t size, n;
		gchar *dst;
		
		size = 1 + utf32_to_utf8(dst32, NULL, 0);
		dst = g_malloc(size * sizeof *dst);
		n = 1 + utf32_to_utf8(dst32, dst, size);
		g_assert(n == size);

		G_FREE_NULL(dst32);
		return dst;
	}
}


/**
 * Apply the NFKD/NFC algo to have nomalized keywords
 */
guint32 *
utf32_canonize(const guint32 *src)
{
	guint32 *nfkd, *nfd;
	size_t size, n;
	
	/* Convert to NFKD */
	size = 1 + utf32_decompose(src, NULL, 0, TRUE);
	nfkd = g_malloc(size * sizeof *nfkd);
	n = utf32_decompose(src, nfkd, size, TRUE);
	g_assert(size - 1 == n);

	/* FIXME: Must convert 'szett' to "ss" */
	n = utf32_strlower(nfkd, nfkd, size);
	g_assert(size - 1 == n);

	n = utf32_filter(nfkd, nfkd, size);
	g_assert(size - 1 >= n);

	/* Convert to NFD; this might be unnecessary if the previous
	 * operations did not destroy the NFKD */
	size = 1 + utf32_decompose(nfkd, NULL, 0, FALSE);
	nfd = g_malloc(size * sizeof *nfd);
	n = utf32_decompose(nfkd, nfd, size, FALSE);
	g_assert(size - 1 == n);

	G_FREE_NULL(nfkd);

	/* Convert to NFC */
	n = utf32_compose(nfd);
	n = utf32_compose_hangul(nfd);

	return nfd;
}

/**
 * Apply the NFKD/NFC algo to have nomalized keywords
 */
gchar *
utf8_canonize(const gchar *src)
{
	guint32 *dst32;

	g_assert(utf8_is_valid_string(src, 0));
	
	{	
		size_t size, n;
		guint32 *s;
		
		size = 1 + utf8_to_utf32(src, NULL, 0);
		s = g_malloc(size * sizeof *s);
		n = 1 + utf8_to_utf32(src, s, size);
		g_assert(n == size);
		
		dst32 = utf32_canonize(s);
		if (dst32 != s)
			G_FREE_NULL(s);
	}

	{
		size_t size, n;
		gchar *dst;
		
		size = 1 + utf32_to_utf8(dst32, NULL, 0);
		dst = g_malloc(size * sizeof *dst);
		n = 1 + utf32_to_utf8(dst32, dst, size);
		g_assert(n == size);

		G_FREE_NULL(dst32);

		return dst;
	}
}

/**
 * Helper function to sort the lists of ``utf32_compose_roots''.
 */
static int
compose_root_cmp(gconstpointer a, gconstpointer b)
{
	guint i = GPOINTER_TO_UINT(a), j = GPOINTER_TO_UINT(b);

	g_assert(i < G_N_ELEMENTS(utf32_nfkd_lut));
	g_assert(j < G_N_ELEMENTS(utf32_nfkd_lut));
	return CMP(utf32_nfkd_lut[i].d[1], utf32_nfkd_lut[j].d[1]);
}

/**
 * This is a helper for unicode_compose_init() to create the lookup
 * table used by utf32_compose_char(). The first character of the
 * decomposition sequence is used as key, the index into the
 * ``utf32_nfkd_lut'' is used as value.
 */
static void 
unicode_compose_add(guint idx)
{
	GSList *sl, *new_sl;
	gpointer key;

	key = GUINT_TO_POINTER(utf32_nfkd_lut[idx].d[0]);
	sl = g_hash_table_lookup(utf32_compose_roots, key);
	new_sl = g_slist_insert_sorted(sl,
			GUINT_TO_POINTER(idx), compose_root_cmp);
	if (sl != new_sl)
		g_hash_table_insert(utf32_compose_roots, key, new_sl);
}

static void
unicode_compose_init(void)
{
	size_t i;

	/* Check order and consistency of the general category lookup table */
	for (i = 0; i < G_N_ELEMENTS(utf32_general_category_lut); i++) {
		size_t len;
		guint32 uc;
		uni_gc_t gc;

		uc = utf32_general_category_lut[i].uc;
		gc = utf32_general_category_lut[i].gc;
		len = utf32_general_category_lut[i].len;
		
		g_assert(len > 0); /* entries are at least one character large */

		if (i > 0) {
			size_t prev_len;
			guint32 prev_uc;
			uni_gc_t prev_gc;

			prev_uc = utf32_general_category_lut[i - 1].uc;
			prev_gc = utf32_general_category_lut[i - 1].gc;
			prev_len = utf32_general_category_lut[i - 1].len;
			
			g_assert(prev_uc < uc);	/* ordered */
			g_assert(prev_uc + prev_len <= uc); /* non-overlapping */
			/* The category must changed with each entry, unless
			 * there's a gap */
			g_assert(prev_gc != gc || prev_uc + prev_len < uc);
		}
		
		do {
			g_assert(gc == utf32_general_category(uc));
			uc++;
		} while (--len != 0);
	}
	
	/* Check order and consistency of the composition exclusions table */
	for (i = 0; i < G_N_ELEMENTS(utf32_composition_exclusions); i++) {
		guint32 uc;

		uc = utf32_composition_exclusions[i];
		g_assert(i == 0 || uc > utf32_composition_exclusions[i - 1]);
		g_assert(utf32_composition_exclude(uc));
	}
	
	/* Create the composition lookup table */
	utf32_compose_roots = g_hash_table_new(NULL, NULL);

	for (i = 0; i < G_N_ELEMENTS(utf32_nfkd_lut); i++) {
		guint32 uc;
		
		uc = utf32_nfkd_lut[i].c;
		
		g_assert(i == 0 ||
			(uc & ~UTF32_F_MASK) > (utf32_nfkd_lut[i - 1].c & ~UTF32_F_MASK));

		if (!(uc & UTF32_F_NFKD)) {
			const guint32 *s;
			
			uc &= ~UTF32_F_MASK;
			s = utf32_decompose_lookup(uc, FALSE);
			g_assert(s);
			g_assert(s[0] != 0);
			
			/* Singletons are excluded from compositions */
			if (0 == s[1])
				continue;

			/* Decomposed sequences beginning with a non-starter are excluded
	 		 * from compositions */
			if (0 != utf32_combining_class(s[0]))
				continue;

			/* Special exclusions */
			if (utf32_composition_exclude(uc))
				continue;

			/* NB:	utf32_compose() assumes that each direct composition
	 		 *		eliminates at most one character.
			 */
			g_assert(s[0] != 0 && s[1] != 0 && s[2] == 0);
			
			unicode_compose_add(i);
		}
	}

#if 0 && defined(USE_GLIB2)	
	for (i = 0; i <= 0x10FFFD; i++) {
		guint32 uc;
		GUnicodeType gt;

		uc = i;
		gt = g_unichar_type(uc);
		switch (utf32_general_category(uc)) {
		case UNI_GC_LETTER_UPPERCASE:
			g_assert(G_UNICODE_UPPERCASE_LETTER == gt);
			break;
		case UNI_GC_LETTER_LOWERCASE:
			g_assert(G_UNICODE_LOWERCASE_LETTER == gt);
			break;
		case UNI_GC_LETTER_TITLECASE:
			g_assert(G_UNICODE_TITLECASE_LETTER == gt);
			break;
		case UNI_GC_LETTER_MODIFIER:
			g_assert(G_UNICODE_MODIFIER_LETTER == gt);
			break;
		case UNI_GC_LETTER_OTHER:
			g_assert(G_UNICODE_OTHER_LETTER == gt);
			break;
		case UNI_GC_MARK_NONSPACING:
			g_assert(G_UNICODE_NON_SPACING_MARK == gt);
			break;
		case UNI_GC_MARK_SPACING_COMBINE:
			g_assert(G_UNICODE_COMBINING_MARK == gt);
			break;
		case UNI_GC_MARK_ENCLOSING:
			g_assert(G_UNICODE_ENCLOSING_MARK == gt);
			break;
		case UNI_GC_NUMBER_DECIMAL:
			g_assert(G_UNICODE_DECIMAL_NUMBER == gt);
			break;
		case UNI_GC_NUMBER_LETTER:
			g_assert(G_UNICODE_LETTER_NUMBER == gt);
			break;
		case UNI_GC_NUMBER_OTHER:
			g_assert(G_UNICODE_OTHER_NUMBER == gt);
			break;
		case UNI_GC_PUNCT_CONNECTOR:
			g_assert(G_UNICODE_CONNECT_PUNCTUATION == gt);
			break;
		case UNI_GC_PUNCT_DASH:
			g_assert(G_UNICODE_DASH_PUNCTUATION == gt);
			break;
		case UNI_GC_PUNCT_OPEN:
			g_assert(G_UNICODE_OPEN_PUNCTUATION == gt);
			break;
		case UNI_GC_PUNCT_CLOSE:
			g_assert(G_UNICODE_CLOSE_PUNCTUATION == gt);
			break;
		case UNI_GC_PUNCT_INIT_QUOTE:
			g_assert(G_UNICODE_INITIAL_PUNCTUATION == gt);
			break;
		case UNI_GC_PUNCT_FINAL_QUOTE:
			g_assert(G_UNICODE_FINAL_PUNCTUATION == gt);
			break;
		case UNI_GC_PUNCT_OTHER:
			g_assert(G_UNICODE_OTHER_PUNCTUATION == gt);
			break;
		case UNI_GC_SYMBOL_MATH:
			g_assert(G_UNICODE_MATH_SYMBOL == gt);
			break;
		case UNI_GC_SYMBOL_CURRENCY:
			g_assert(G_UNICODE_CURRENCY_SYMBOL == gt);
			break;
		case UNI_GC_SYMBOL_MODIFIER:
			g_assert(G_UNICODE_MODIFIER_SYMBOL == gt);
			break;
		case UNI_GC_SYMBOL_OTHER:
			g_assert(G_UNICODE_OTHER_SYMBOL == gt);
			break;
		case UNI_GC_SEPARATOR_SPACE:
			g_assert(G_UNICODE_SPACE_SEPARATOR == gt);
			break;
		case UNI_GC_SEPARATOR_LINE:
			g_assert(G_UNICODE_LINE_SEPARATOR == gt);
			break;
		case UNI_GC_SEPARATOR_PARAGRAPH:
			g_assert(G_UNICODE_PARAGRAPH_SEPARATOR == gt);
			break;
		case UNI_GC_OTHER_CONTROL:
			g_assert(G_UNICODE_CONTROL == gt);
			break;
		case UNI_GC_OTHER_FORMAT:
			g_assert(G_UNICODE_FORMAT == gt);
			break;
		case UNI_GC_OTHER_SURROGATE:
			g_assert(G_UNICODE_SURROGATE == gt);
			break;
		case UNI_GC_OTHER_PRIVATE_USE:
			g_assert(G_UNICODE_PRIVATE_USE == gt);
			break;
		case UNI_GC_OTHER_NOT_ASSIGNED:
			g_assert(G_UNICODE_UNASSIGNED == gt);
			break;
		}
	}
#endif

#if 0 && defined(USE_GLIB2)	
	for (;;) {	
		guint32 test[32];
		guint32 q[1024], *x, *y;
		gchar s[1024], t[1024], *s_nfc;
		size_t size;

#if 1 
		for (i = 0; i < G_N_ELEMENTS(test) - 1; i++) {
			guint32 uc;
			
			do {
				uc = random_value(0x10FFFF);
			} while (
				!uc ||
				UNICODE_IS_SURROGATE(uc) ||
				UNICODE_IS_BYTE_ORDER_MARK(uc) ||
				UNICODE_IS_ILLEGAL(uc)
			);
			test[i] = uc;
		}
		test[i] = 0;
#endif

#if 0 
		test[0] = 0x3271;
		test[1] = 0x26531;
		test[2] = 0;
#endif
	
#if 0 
		test[0] = 0x1ed;
	   	test[1] = 0x945e4;
		test[2] = 0;
#endif

#if 0 
		test[0] = 0x00a8;
	   	test[1] = 0x0711;
		test[2] = 0x301;
		test[3] = 0;
#endif
		
#if 0 
		test[0] = 0xef0b8;
		test[1] = 0x56ecd;
	   	test[2] = 0x6b325;
	   	test[3] = 0x46fe6;
	   	test[4] = 0;
#endif
		
#if 0 
		test[0] = 0x40d;
		test[1] = 0x3d681;
	   	test[2] = 0x1087ae;
	   	test[3] = 0x61ba1;
	   	test[4] = 0;
#endif

#if 0 
		test[0] = 0x32b;
		test[1] = 0x93c;
	   	test[2] = 0x22f0;
	   	test[3] = 0xcb90;
	   	test[4] = 0;
#endif

#if 0 
		/* This fails with GLib 2.6.0 because g_utf8_normalize() 
		 * eats the Hangul Jamo character when using G_NORMALIZE_NFC. */
		test[0] = 0x1112;
		test[1] = 0x1174;
	   	test[2] = 0x11a7;
	   	test[3] = 0;
#endif
	
		size = 1 + utf32_decompose_nfkd(test, NULL, 0);
		y = g_malloc(size * sizeof *y);
		utf32_decompose_nfkd(test, y, size);
		x = utf32_strdup(y);
		utf32_compose(x);
		utf32_compose_hangul(x);
		utf32_to_utf8(x, t, sizeof t);
		
		utf32_to_utf8(test, s, sizeof s);
		
#if !defined(USE_ICU)
		s_nfc = g_utf8_normalize(s, (gssize) -1, G_NORMALIZE_NFKC);
#else
		{
			size_t len, maxlen;
			UChar *qtmp1, *qtmp2;
			
			maxlen = strlen(s) * 6 + 1;
			qtmp1 = (UChar *) g_malloc(maxlen * sizeof(UChar));
			qtmp2 = (UChar *) g_malloc(maxlen * sizeof(UChar));
			len = utf8_to_icu_conv(s, strlen(s), qtmp1, maxlen);
			len = unicode_NFC(qtmp1, len, qtmp2, maxlen);
			s_nfc = g_malloc0((len * 6) + 1);
			len = icu_to_utf8_conv(qtmp2, len, s_nfc, len * 6);
			s_nfc[len] = '\0';
			G_FREE_NULL(qtmp2);
			G_FREE_NULL(qtmp1);
		}
#endif

		g_assert(s_nfc != NULL);
		utf8_to_utf32(s_nfc, q, G_N_ELEMENTS(q));

		if (0 != strcmp(s_nfc, t))
			G_BREAKPOINT();

		G_FREE_NULL(x);	
		G_FREE_NULL(y);	
		G_FREE_NULL(s_nfc);	
	}
#endif
}

static void
unicode_decompose_init(void)
{
#if 0 && defined(USE_GLIB2)
	size_t i;

	/* Check all single Unicode characters */
	for (i = 0; i <= 0x10FFFD; i++) {
		guint size;
		gchar buf[256];
		gchar utf8_char[7];
		gchar *s;

		if (
			UNICODE_IS_SURROGATE(i) ||
			UNICODE_IS_BYTE_ORDER_MARK(i) ||
			UNICODE_IS_ILLEGAL(i)
		) {
			continue;
		}

		size = g_unichar_to_utf8(i, utf8_char);
		g_assert((gint) size >= 0 && size < sizeof utf8_char);
		utf8_char[size] = '\0';
		utf8_decompose_nfd(utf8_char, buf, G_N_ELEMENTS(buf));
#if !defined(USE_ICU)
		s = g_utf8_normalize(utf8_char, -1, G_NORMALIZE_NFD);
#else
		{
			size_t len, maxlen;
			UChar *qtmp1, *qtmp2;
			
			maxlen = 1024;
			qtmp1 = (UChar *) g_malloc(maxlen * sizeof(UChar));
			qtmp2 = (UChar *) g_malloc(maxlen * sizeof(UChar));
			len = utf8_to_icu_conv(utf8_char, strlen(utf8_char), qtmp2, maxlen);
			g_assert(i == 0 || len != 0);
			len = unicode_NFKD(qtmp2, len, qtmp1, maxlen);
			g_assert(i == 0 || len != 0);
			s = g_malloc0((len * 6) + 1);
			len = icu_to_utf8_conv(qtmp2, len, s, len * 6);
			g_assert(i == 0 || len != 0);
			s[len] = '\0';
			G_FREE_NULL(qtmp2);
			G_FREE_NULL(qtmp1);
		}
#endif

		if (strcmp(s, buf)) {
			g_message("\n0x%04X\nbuf=\"%s\"\ns=\"%s\"", i, buf, s);

#if GLIB_CHECK_VERSION(2, 4, 0) /* Glib >= 2.4.0 */
			/*
			 * The normalized strings should be identical. However, older
			 * versions of GLib do not normalize some characters properly.
			 */
			G_BREAKPOINT();
#endif /* GLib >= 2.4.0 */

		}
		G_FREE_NULL(s);
	}

	g_message("random value: %u", (guint) random_value(~0));
	
	/* Check random Unicode strings */
	for (i = 0; i < 10000000; i++) {
		gchar buf[256 * 7];
		guint32 test[32], out[256];
		gchar *s, *t;
		size_t j, utf8_len, utf32_len, m, n;

		/* Check random strings */
		utf32_len = random_value(G_N_ELEMENTS(test) - 2) + 1;
		g_assert(utf32_len < G_N_ELEMENTS(test));
		for (j = 0; j < utf32_len; j++) {
			guint32 uc;

			do {
				uc = random_value(0x10FFFF);
				if (
						UNICODE_IS_SURROGATE(uc) ||
						UNICODE_IS_BYTE_ORDER_MARK(uc) ||
						UNICODE_IS_ILLEGAL(uc)
				   ) {
					uc = 0;
				}
			} while (!uc);
			test[j] = uc;
		}
		test[j] = 0;

#if 0
		/* This test case checks that the canonical sorting works i.e., 
		 * 0x0ACD must appear before all 0x05AF. */
		j = 0;
		test[j++] = 0x00B3;
		test[j++] = 0x05AF;
		test[j++] = 0x05AF;
		test[j++] = 0x05AF;
		test[j++] = 0x0ACD;
		test[j] = 0;
		utf32_len = j;

		g_assert(!utf32_canonical_sorted(test));
#endif

#if 0
		/* This test case checks that the canonical sorting uses a
		 * stable sort algorithm i.e., preserves the relative order
		 * of equal elements.  */
		j = 0;
		test[j++] = 0x0065;
		test[j++] = 0x0301;
		test[j++] = 0x01D165;
		test[j++] = 0x0302;
		test[j++] = 0x0302;
		test[j++] = 0x0304;
		test[j++] = 0x01D166;
		test[j++] = 0x01D165;
		test[j++] = 0x0302;
		test[j++] = 0x0300;
		test[j++] = 0x0305;
		test[j++] = 0x01D166;
		test[j] = 0;
		utf32_len = j;

		g_assert(!utf32_canonical_sorted(test));
#endif

#if 0 
		j = 0;	
		test[j++] = 0x32b;
		test[j++] = 0x93c;
		test[j++] = 0x22f0;
		test[j++] = 0xcb90;
		test[j] = 0;
		utf32_len = j;
#endif

#if 1
		j = 0;	
		test[j++] = 0x239f;
		test[j++] = 0xcd5c;
		test[j++] = 0x11a7;
		test[j++] = 0x6d4c;
		test[j] = 0;
		utf32_len = j;
#endif


		utf8_len = utf32_to_utf8(test, buf, G_N_ELEMENTS(buf));
		g_assert(utf8_len < sizeof buf);
		g_assert(utf32_len <= utf8_len);
	
		n = utf8_is_valid_string(buf, 0);
		g_assert(utf8_len >= n);
		g_assert(utf32_len == n);
		g_assert(n == utf8_is_valid_string(buf, utf8_len));
				
		n = utf8_to_utf32(buf, out, G_N_ELEMENTS(out));
		g_assert(n == utf32_len);
		g_assert(0 == memcmp(test, out, n * sizeof test[0]));
		
		n = utf8_decompose_nfkd(buf, NULL, 0) + 1;
		t = g_malloc(n);
		m = utf8_decompose_nfkd(buf, t, n);
		g_assert(m == n - 1);
		g_assert(utf8_canonical_sorted(t));
	
#if !defined(USE_ICU)
		s = g_utf8_normalize(buf, -1, G_NORMALIZE_NFKD);
#else
		{
			size_t len, maxlen;
			UChar *qtmp1, *qtmp2;
			
			maxlen = strlen(buf) * 6 + 1;
			qtmp1 = (UChar *) g_malloc(maxlen * sizeof(UChar));
			qtmp2 = (UChar *) g_malloc(maxlen * sizeof(UChar));
			len = utf8_to_icu_conv(buf, strlen(buf), qtmp1, maxlen);
			len = unicode_NFKD(qtmp1, len, qtmp2, maxlen);
			s = g_malloc0((len * 6) + 1);
			len = icu_to_utf8_conv(qtmp2, len, s, len * 6);
			s[len] = '\0';
			G_FREE_NULL(qtmp2);
			G_FREE_NULL(qtmp1);
		}
#endif

		if (0 != strcmp(s, t)) {
			const gchar *x, *y;
			guint32 *zx, *zy;
		
			/* Convert to UTF-32 so that the characters can be easily
			 * checked from a debugger */	
			zx = g_malloc0(1024 * sizeof *zx);
			utf8_to_utf32(s, zx, 1024);
			zy = g_malloc0(1024 * sizeof *zy);
			utf8_to_utf32(t, zy, 1024);
			
			printf("s=\"%s\"\nt=\"%s\"\n", s, t);

			for (x = s, y = t; *x != '\0'; x++, y++)
				if (*x != *y)
					break;

			g_message("x=\"%s\"\ny=\"%s\"\n, *x=%x, *y=%x\n",
				x, y,
				utf8_decode_char(x, strlen(x), NULL, FALSE),
				utf8_decode_char(y, strlen(y), NULL, FALSE));
			
#if GLIB_CHECK_VERSION(2, 4, 0) /* Glib >= 2.4.0 */
			/*
			 * The normalized strings should be identical. However, older
			 * versions of GLib do not normalize some characters properly.
			 */
			G_BREAKPOINT();
#endif /* GLib >= 2.4.0 */
			
			G_FREE_NULL(zx);
			G_FREE_NULL(zy);

		}
		G_FREE_NULL(s);
		G_FREE_NULL(t);
	}
#endif /* USE_GLIB2 */
}

/* vi: set ts=4 sw=4 cindent: */

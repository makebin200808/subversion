/*
 * svn_string.h :  counted_length strings for Subversion
 *                 (using apr's memory pools)
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */


#ifndef SVN_STRING_H
#define SVN_STRING_H

#include <apr.h>
#include <apr_pools.h>       /* APR memory pools for everyone. */
#include <apr_strings.h>

#include "svn_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */



/* DATA TYPES

   There are two string datatypes: svn_string_t and svn_stringbuf_t.
   The former is a simple pointer/length pair useful for passing around
   strings (or arbitrary bytes) with a counted length. svn_stringbuf_t is
   buffered to enable efficient appending of strings without an allocation
   and copy for each append operation.

   svn_string_t contains a "const char *" for its data, so it is most
   appropriate for constant data and for functions which expect constant,
   counted data. Functions should generally use "const svn_string_t *" as
   their parameter to indicate they are expecting a constant, counted
   string.

   svn_stringbuf_t uses a plain "char *" for its data, so it is most
   appropriate for modifiable data.


   INVARIANT

   Both structures maintain a significant invariant:

       s->data[s->len] == '\0'

   The functions defined within this header file will maintain the invariant
   (which does imply that memory is allocated/defined as len+1 bytes). If
   code outside of the svn_string.h functions manually builds these
   structures, then they must enforce this invariant.

   Note that an svn_string(buf)_t may contain binary data, which means that
   strlen(s->data) does not have to equal s->len. The null terminator is
   provided to make it easier to pass s->data to C string interfaces.
*/

typedef struct
{
  const char *data;
  apr_size_t len;
} svn_string_t;


typedef struct
{
  char *data;                /* pointer to the bytestring */
  apr_size_t len;            /* length of bytestring */
  apr_size_t blocksize;      /* total size of buffer allocated */
  /* pool from which this string was originally allocated, and is not
     necessarily specific to this string.  This is used only for
     allocating more memory from when the string needs to grow.  */
  apr_pool_t *pool;          
} svn_stringbuf_t;



/* Create a new bytestring containing a C string (null-terminated), or
   containing a generic string of bytes (NON-null-terminated) */
svn_stringbuf_t * svn_stringbuf_create (const char *cstring, 
                                     apr_pool_t *pool);
svn_stringbuf_t * svn_stringbuf_ncreate (const char *bytes,
                                      const apr_size_t size, 
                                      apr_pool_t *pool);

/* Create a new bytestring by formatting CSTRING (null-terminated)
   from varargs, which are as appropriate for apr_psprintf. */
svn_stringbuf_t *svn_stringbuf_createf (apr_pool_t *pool,
                                     const char *fmt,
                                     ...)
       __attribute__ ((format (printf, 2, 3)));

/* Create a new bytestring by formatting CSTRING (null-terminated)
   from a va_list (see svn_stringbuf_createf). */
svn_stringbuf_t *svn_stringbuf_createv (apr_pool_t *pool,
                                     const char *fmt,
                                     va_list ap)
       __attribute__ ((format (printf, 2, 0)));

/* Make sure that the string STR has at least MINIMUM_SIZE bytes of
   space available in the memory block.  (MINIMUM_SIZE should include
   space for the terminating null character.)  */
void svn_stringbuf_ensure (svn_stringbuf_t *str,
                        apr_size_t minimum_size);

/* Set a bytestring STR to VALUE */
void svn_stringbuf_set (svn_stringbuf_t *str, const char *value);

/* Set a bytestring STR to empty (0 length). */
void svn_stringbuf_setempty (svn_stringbuf_t *str);

/* Return true if a bytestring is empty (has length zero). */
svn_boolean_t svn_stringbuf_isempty (const svn_stringbuf_t *str);

/* Chop NBYTES bytes off end of STR, but not more than STR->len. */
void svn_stringbuf_chop (svn_stringbuf_t *str, apr_size_t bytes);

/* Fill bytestring STR with character C. */
void svn_stringbuf_fillchar (svn_stringbuf_t *str, const unsigned char c);

/* Append either a string of bytes, an svn_stringbuf_t, or a C-string
   onto TARGETSTR.  reallocs() if necessary.  TARGETSTR is affected,
   nothing else is. */
void svn_stringbuf_appendbytes (svn_stringbuf_t *targetstr,
                             const char *bytes, 
                             const apr_size_t count);
void svn_stringbuf_appendstr (svn_stringbuf_t *targetstr, 
                           const svn_stringbuf_t *appendstr);
void svn_stringbuf_appendcstr (svn_stringbuf_t *targetstr,
                            const char *cstr);

/* Return a duplicate of ORIGNAL_STRING. */
svn_stringbuf_t *svn_stringbuf_dup (const svn_stringbuf_t *original_string,
                                 apr_pool_t *pool);


/* Return TRUE iff STR1 and STR2 have identical length and data. */
svn_boolean_t svn_stringbuf_compare (const svn_stringbuf_t *str1, 
                                  const svn_stringbuf_t *str2);

/** convenience routines **/

/* Return offset of first non-whitespace character in STR, or -1 if none. */
apr_size_t svn_stringbuf_first_non_whitespace (const svn_stringbuf_t *str);

/* Strips whitespace from both sides of STR (modified in place). */
void svn_stringbuf_strip_whitespace (svn_stringbuf_t *str);

/* Return position of last occurrence of CHAR in STR, or return
   STR->len if no occurrence. */ 
apr_size_t svn_stringbuf_find_char_backward (const svn_stringbuf_t *str, char ch);

/* Chop STR back to CHAR, inclusive.  Returns number of chars
   chopped, so if no such CHAR in STR, chops nothing and returns 0. */
apr_size_t svn_stringbuf_chop_back_to_char (svn_stringbuf_t *str, char ch);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* SVN_STRING_H */

/* ----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */

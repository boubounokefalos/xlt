/* XLT - PDF text extraction. Portable (host-tested).
 *
 * Not a renderer: it finds each page's text (in content-stream order, with
 * line breaks and spaces guessed from the text positions) and the largest
 * JPEG image on the page, which is what a scanned PDF consists of. The file
 * is read piece by piece (xref table or stream, object streams), so a big PDF
 * costs only its cross-reference table in memory. */
#ifndef PDF_H
#define PDF_H

#include <stdint.h>

enum {
    PDF_OK = 0,
    PDF_E_READ = -1,      /* can't open or read the file */
    PDF_E_FORMAT = -2,    /* not a PDF, or too damaged to find its pages */
    PDF_E_ENCRYPTED = -3, /* encrypted (even with an empty password): not supported */
    PDF_E_MEMORY = -4,
};

typedef struct PdfDoc PdfDoc;

/* Opens the document and walks its page tree. NULL on failure, with *err set. */
PdfDoc *pdf_open(const char *path, int *err);
void pdf_close(PdfDoc *d);
int pdf_page_count(const PdfDoc *d);

typedef struct {
    uint32_t img_off, img_len; /* the page's largest JPEG (DCTDecode) image: file offset of its data, 0 if none */
    int img_w, img_h;
    int undecodable;           /* glyphs whose font has no Unicode mapping (left out) */
} PdfPageInfo;

/* Extracts page `page` (0-based) as UTF-8 into out (at most cap - 1 bytes,
 * NUL-terminated). legacy_greek: fonts without a Unicode map are read as
 * Windows-1253 instead of 1252 (old Greek documents). Returns the length,
 * or a PDF_E_* code. */
int pdf_page_text(PdfDoc *d, int page, int legacy_greek, char *out, int cap, PdfPageInfo *info);

#endif

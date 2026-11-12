#!/usr/bin/env python3
"""Writes the PDF fixtures for tests/test_pdf.c into the directory given.

Each file exercises one path of src/pdf.c: a classic xref table with
inherited resources, an xref stream with object streams and a PNG predictor,
a composite font read through /ToUnicode, /Differences and the Symbol font,
a scanned page (one JPEG), a broken xref (rescan) and an encrypted file.
"""
import io
import os
import struct
import sys
import zlib


def build(objs, trailer_extra=b"", startxref_delta=0, garbage_xref=False):
    """objs: {num: bytes} (body without 'N 0 obj'). Classic xref table."""
    out = io.BytesIO()
    out.write(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offs = {}
    for num in sorted(objs):
        offs[num] = out.tell()
        out.write(b"%d 0 obj\n" % num + objs[num] + b"\nendobj\n")
    xref = out.tell()
    size = max(objs) + 1
    if garbage_xref:
        out.write(b"xref\n0 2\nthis is not an xref table\n")
    else:
        out.write(b"xref\n0 %d\n" % size)
        out.write(b"0000000000 65535 f \n")
        for n in range(1, size):
            if n in offs:
                out.write(b"%010d 00000 n \n" % offs[n])
            else:
                out.write(b"0000000000 00000 f \n")
    out.write(b"trailer\n<< /Size %d /Root 1 0 R %s>>\n" % (size, trailer_extra))
    out.write(b"startxref\n%d\n%%%%EOF\n" % (xref + startxref_delta))
    return out.getvalue()


def stream(d, data, compress=False):
    if compress:
        data = zlib.compress(data)
        d = d + b" /Filter /FlateDecode"
    return b"<< " + d + b" /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"


def simple_pdf():
    p1 = (b"BT /F1 12 Tf 14 TL 72 720 Td (Hello World) Tj T* [(Kern) -50 (ed) -400 (words)] TJ "
          b"0 -14 Td (Left) Tj 100 0 Td (Right) Tj ET")
    p2 = b"BT /F1 12 Tf 72 720 Td (Caf\\351 na\\357ve \\(ok\\)) Tj ET"
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /Resources << /Font << /F1 5 0 R >> >> >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 6 0 R >>",
        4: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents [7 0 R 8 0 R] >>",
        5: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        6: stream(b"", p1),
        # page 2 is split over two streams between an operator's operands
        7: stream(b"", p2[:p2.index(b"(Caf")]),
        8: stream(b"", p2[p2.index(b"(Caf"):], compress=True),
    }
    return objs


def utf16(s):
    return s.encode("utf-16-be").hex().upper().encode()


def modern_pdf():
    """Xref stream + object stream + predictor; Type0 font with a ToUnicode CMap."""
    greek = "Γειά σου"
    # glyph codes 1..8 for the characters, via bfrange (array and plain) and bfchar
    cmap = (b"/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
            b"/CMapName /Test def 1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
            b"2 beginbfrange\n"
            b"<0001> <0004> [<" + utf16("Γ") + b"> <" + utf16("ε") + b"> <" + utf16("ι") + b"> <" +
            utf16("ά") + b">]\n"
            b"<0010> <0012> <" + utf16("σ") + b">\n"   # 0x10 σ, 0x11 τ, 0x12 υ
            b"endbfrange\n3 beginbfchar\n<0005> <0020>\n<0006> <" + utf16("ο") + b">\n"
            b"<0007> <00660069>\nendbfchar\nendcmap CMapName currentdict /CMap defineresource pop end end\n")
    codes = [1, 2, 3, 4, 5, 0x10, 6, 0x12]
    shown = b"".join(struct.pack(">H", c) for c in codes)
    content = (b"BT /F2 10 Tf 1 0 0 1 50 700 Tm <" + shown.hex().encode() + b"> Tj "
               b"0 -40 Td <0007> Tj ET")
    regular = {  # stays outside the object stream
        6: stream(b"", content, compress=True),
        7: stream(b"", cmap, compress=True),
    }
    compressed = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /Resources << /Font << /F2 4 0 R >> >> /Contents 6 0 R >>",
        4: b"<< /Type /Font /Subtype /Type0 /BaseFont /Test /Encoding /Identity-H "
           b"/DescendantFonts [5 0 R] /ToUnicode 7 0 R >>",
        5: b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Test /DW 500 /W [1 [600 500 300 550] 16 18 520] >>",
    }
    out = io.BytesIO()
    out.write(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
    offs = {}
    for num in sorted(regular):
        offs[num] = out.tell()
        out.write(b"%d 0 obj\n" % num + regular[num] + b"\nendobj\n")
    # object stream 8
    head, body = b"", b""
    order = sorted(compressed)
    for n in order:
        head += b"%d %d " % (n, len(body))
        body += compressed[n] + b"\n"
    data = head + body
    offs[8] = out.tell()
    out.write(b"8 0 obj\n" + stream(b"/Type /ObjStm /N %d /First %d" % (len(order), len(head)), data, True) +
              b"\nendobj\n")
    # xref stream 9, W [1 4 2], PNG Up predictor
    xref_off = out.tell()
    rows = []
    for n in range(10):
        if n == 0:
            rows.append(struct.pack(">BIH", 0, 0, 65535))
        elif n in compressed:
            rows.append(struct.pack(">BIH", 2, 8, order.index(n)))
        elif n == 9:
            rows.append(struct.pack(">BIH", 1, xref_off, 0))
        else:
            rows.append(struct.pack(">BIH", 1, offs[n], 0))
    raw, prev = b"", bytes(7)
    for r in rows:
        raw += b"\x02" + bytes((r[i] - prev[i]) & 0xFF for i in range(7))
        prev = r
    xs = zlib.compress(raw)
    out.write(b"9 0 obj\n<< /Type /XRef /Size 10 /Root 1 0 R /W [1 4 2] /Filter /FlateDecode "
              b"/DecodeParms << /Predictor 12 /Columns 7 >> /Length %d >>\nstream\n" % len(xs) + xs +
              b"\nendstream\nendobj\n")
    out.write(b"startxref\n%d\n%%%%EOF\n" % xref_off)
    return out.getvalue(), greek


def differences_pdf():
    c1 = b"BT /F1 12 Tf 72 700 Td (ABC) Tj ET BT /F2 12 Tf 72 680 Td (abg) Tj ET BT /F3 12 Tf 72 660 Td (\\301\\342) Tj ET"
    return {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R /F2 5 0 R /F3 7 0 R >> >> /Contents 6 0 R >>",
        4: b"<< /Type /Font /Subtype /Type1 /BaseFont /Custom /Encoding << /Type /Encoding "
           b"/BaseEncoding /WinAnsiEncoding /Differences [65 /alpha /beta /gamma] >> >>",
        5: b"<< /Type /Font /Subtype /Type1 /BaseFont /Symbol >>",
        6: stream(b"", c1),
        7: b"<< /Type /Font /Subtype /TrueType /BaseFont /ArialGreek >>",
    }


def scan_pdf(jpeg):
    return {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: b"<< /Type /Page /Parent 2 0 R /Resources << /XObject << /Im0 5 0 R /Im1 6 0 R >> >> /Contents 4 0 R >>",
        4: stream(b"", b"q 612 0 0 792 0 0 cm /Im0 Do Q q 10 0 0 10 0 0 cm /Im1 Do Q"),
        5: b"<< /Type /XObject /Subtype /Image /Width 64 /Height 48 /ColorSpace /DeviceRGB /BitsPerComponent 8 "
           b"/Filter /DCTDecode /Length %d >>\nstream\n" % len(jpeg) + jpeg + b"\nendstream",
        6: stream(b"/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceGray /BitsPerComponent 8",
                  b"\x00\xff\xff\x00"),
    }


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    def w(name, data):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)

    w("simple.pdf", build(simple_pdf()))
    data, _ = modern_pdf()
    w("modern.pdf", data)
    w("differences.pdf", build(differences_pdf()))
    from PIL import Image
    img = Image.new("RGB", (64, 48), (200, 30, 30))
    b = io.BytesIO()
    img.save(b, "JPEG", quality=80)
    w("scan.pdf", build(scan_pdf(b.getvalue())))
    w("broken.pdf", build(simple_pdf(), startxref_delta=-7))
    w("broken2.pdf", build(simple_pdf(), garbage_xref=True))
    w("encrypted.pdf", build(simple_pdf(), trailer_extra=b"/Encrypt << /Filter /Standard /V 1 >> "))
    w("notpdf.pdf", b"hello, this is not a PDF\n" * 10)


if __name__ == "__main__":
    main()

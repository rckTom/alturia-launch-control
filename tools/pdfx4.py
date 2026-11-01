#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pikepdf>=9"]
# ///
"""Turn a white-ink UV DTF sheet exported by Inkscape into PDF/X-4
(ISO 15930-7:2008).

PDF/X-4 needs a CMYK output intent, and in CMYK white is no ink at all, so
the white fill is moved into a Separation colour space named after the
printer's white-ink channel (SPOT, ask the print shop for their name). Its
alternate is 100 % magenta, so a RIP that does not know the name prints
something obviously wrong instead of an empty sheet.

Everything else PDF/X-4 asks for is added: output intent with an embedded
ICC profile, TrimBox/BleedBox, XMP with GTS_PDFXVersion, Trapped = False,
PDF 1.6 header. The input must be a single-colour sheet: plain white vector
fills, no images, no fonts (export with text converted to paths).

Usage: uv run --script tools/pdfx4.py <in.pdf> [<out.pdf>] --title TITLE
       [--spot White] [--icc PROFILE]
"""

import argparse
import uuid
from datetime import datetime, timezone
from pathlib import Path

import pikepdf
from pikepdf import Array, Dictionary, Name, Operator

SPOT = "White"
ICC = Path("/usr/share/color/icc/colord/FOGRA39L_coated.icc")
CONDITION = ("FOGRA39", "Coated FOGRA39 (ISO 12647-2:2004)")
PDFX_NS = "http://www.npes.org/pdfx/ns/id/"


def spot_colour_space(pdf, spot):
    """[/Separation /<spot> /DeviceCMYK <tint 0..1 -> 0 t 0 0>]"""
    tint = Dictionary(FunctionType=2, Domain=[0, 1], C0=[0, 0, 0, 0],
                      C1=[0, 1, 0, 0], N=1)
    return Array([Name.Separation, Name("/" + spot), Name.DeviceCMYK,
                  pdf.make_indirect(tint)])


def recolour(pdf, page):
    """Replace the white `rg` fills with the spot colour at full tint."""
    out = []
    for operands, op in pikepdf.parse_content_stream(page):
        name = str(op)
        if name == "rg":
            if [float(v) for v in operands] != [1.0, 1.0, 1.0]:
                raise SystemExit(f"non-white fill {list(operands)}")
            out += [([Name.CS0], Operator("cs")), ([1], Operator("scn"))]
        elif name in ("RG", "g", "G", "k", "K", "cs", "CS", "sc", "scn",
                      "SC", "SCN", "Do", "BI", "Tj", "TJ"):
            raise SystemExit(f"unexpected operator {name}: only white "
                             "vector fills are supported")
        else:
            out.append((operands, op))
    page.Contents = pdf.make_stream(pikepdf.unparse_content_stream(out))


def convert(src, dst, title, spot, icc):
    pdf = pikepdf.open(src, allow_overwriting_input=True)
    for page in pdf.pages:
        recolour(pdf, page)
        if "/Resources" not in page.obj:
            page.obj.Resources = Dictionary()
        page.obj.Resources.ColorSpace = Dictionary(CS0=spot_colour_space(pdf, spot))
        if "/Group" in page.obj:  # DeviceRGB blending space, nothing blends
            del page.obj["/Group"]
        page.TrimBox = page.MediaBox
        page.BleedBox = page.MediaBox

    profile = pdf.make_stream(icc.read_bytes())
    profile.N = 4
    pdf.Root.OutputIntents = Array([Dictionary(
        Type=Name.OutputIntent, S=Name.GTS_PDFX,
        OutputConditionIdentifier=CONDITION[0],
        OutputCondition=CONDITION[1], Info=CONDITION[1],
        RegistryName="http://www.color.org",
        DestOutputProfile=profile)])

    now = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
    with pdf.open_metadata(set_pikepdf_as_editor=False) as meta:
        meta["dc:title"] = title
        meta["xmp:CreatorTool"] = "Inkscape"
        meta["xmp:CreateDate"] = now
        meta["xmp:ModifyDate"] = now
        meta["xmp:MetadataDate"] = now
        meta["pdf:Producer"] = f"pikepdf {pikepdf.__version__}"
        meta["pdf:Trapped"] = "False"
        meta["xmpMM:DocumentID"] = f"uuid:{uuid.uuid4()}"
        meta["xmpMM:InstanceID"] = f"uuid:{uuid.uuid4()}"
        meta["xmpMM:VersionID"] = "1"
        meta["xmpMM:RenditionClass"] = "default"
        meta[f"{{{PDFX_NS}}}GTS_PDFXVersion"] = "PDF/X-4"
    pdf.docinfo.Trapped = Name("/False")
    pdf.docinfo.GTS_PDFXVersion = "PDF/X-4"
    pdf.save(dst, force_version="1.6", fix_metadata_version=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src", type=Path)
    ap.add_argument("dst", type=Path, nargs="?")
    ap.add_argument("--title", required=True)
    ap.add_argument("--spot", default=SPOT)
    ap.add_argument("--icc", type=Path, default=ICC)
    a = ap.parse_args()
    convert(a.src, a.dst or a.src, a.title, a.spot, a.icc)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build SHARDING-OVERVIEW.pptx -- a presentable summary of the hogd db
sharding work, Jun 29 - Aug 18 2026."""

from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn
import copy

# ---------------------------------------------------------------- palette
INK        = RGBColor(0x16, 0x1E, 0x1D)
INK_SOFT   = RGBColor(0x48, 0x53, 0x4F)
FAINT      = RGBColor(0x7C, 0x87, 0x83)
PAPER      = RGBColor(0xF7, 0xF8, 0xF6)
WHITE      = RGBColor(0xFF, 0xFF, 0xFF)
RULE       = RGBColor(0xD2, 0xD8, 0xD4)
RULE_STR   = RGBColor(0xB6, 0xBF, 0xBA)
ACCENT     = RGBColor(0x0D, 0x6A, 0x60)
ACC_SOFT   = RGBColor(0xDC, 0xEA, 0xE6)
AMBER      = RGBColor(0x8A, 0x5A, 0x0E)
AMB_SOFT   = RGBColor(0xF1, 0xE7, 0xD3)
CRIT       = RGBColor(0x94, 0x32, 0x2A)
CRIT_SOFT  = RGBColor(0xF2, 0xDF, 0xDC)

SANS = "Trebuchet MS"   # headings / labels -- ships on Windows + macOS
SERIF = "Georgia"       # running prose
MONO = "Consolas"       # data, code, identifiers

W, H = Inches(13.333), Inches(7.5)
M = Inches(0.85)                      # side margin
CW = W - 2 * M                        # content width

prs = Presentation()
prs.slide_width, prs.slide_height = W, H
BLANK = prs.slide_layouts[6]

# ---------------------------------------------------------------- helpers

def slide(notes=None):
    s = prs.slides.add_slide(BLANK)
    bg = s.background.fill
    bg.solid()
    bg.fore_color.rgb = PAPER
    if notes:
        s.notes_slide.notes_text_frame.text = notes
    return s


def box(s, l, t, w, h, fill=None, line=None, lw=0.75, shape=MSO_SHAPE.RECTANGLE):
    sh = s.shapes.add_shape(shape, l, t, w, h)
    if fill is None:
        sh.fill.background()
    else:
        sh.fill.solid()
        sh.fill.fore_color.rgb = fill
    if line is None:
        sh.line.fill.background()
    else:
        sh.line.color.rgb = line
        sh.line.width = Pt(lw)
    sh.shadow.inherit = False
    sh.text_frame.word_wrap = True
    return sh


def text(s, l, t, w, h, runs, size=14, font=SERIF, color=INK, bold=False,
         align=PP_ALIGN.LEFT, spacing=1.15, space_after=6, caps=False,
         letter=None, anchor=MSO_ANCHOR.TOP):
    """runs: str, or list of paragraphs; a paragraph is a str or list of
    (text, {opts}) tuples."""
    tb = s.shapes.add_textbox(l, t, w, h)
    tf = tb.text_frame
    tf.word_wrap = True
    tf.vertical_anchor = anchor
    tf.margin_left = tf.margin_right = tf.margin_top = tf.margin_bottom = 0
    paras = runs if isinstance(runs, list) else [runs]
    for i, para in enumerate(paras):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = align
        p.line_spacing = spacing
        p.space_after = Pt(space_after)
        chunks = para if isinstance(para, list) else [(para, {})]
        for txt, o in chunks:
            r = p.add_run()
            r.text = txt.upper() if o.get("caps", caps) else txt
            f = r.font
            f.name = o.get("font", font)
            f.size = Pt(o.get("size", size))
            f.bold = o.get("bold", bold)
            f.color.rgb = o.get("color", color)
            sp = o.get("letter", letter)
            if sp is not None:
                f._rPr.set("spc", str(int(sp * 100)))
    return tb


def rule(s, t, l=M, w=None, color=RULE, weight=0.75):
    w = CW if w is None else w
    ln = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, l, t, w, Pt(weight))
    ln.fill.solid()
    ln.fill.fore_color.rgb = color
    ln.line.fill.background()
    ln.shadow.inherit = False
    return ln


def header(s, kicker, title, sub=None, tsize=33):
    text(s, M, Inches(0.52), CW, Inches(0.3), kicker, size=10.5, font=MONO,
         color=ACCENT, caps=True, letter=1.6, space_after=0)
    text(s, M, Inches(0.86), CW, Inches(0.8), title, size=tsize, font=SANS,
         color=INK, bold=True, spacing=0.94, space_after=0)
    y = Inches(1.52) if tsize >= 30 else Inches(1.42)
    rule(s, y)
    if sub:
        text(s, M, y + Inches(0.22), Inches(9.6), Inches(0.6), sub,
             size=15, color=INK_SOFT, spacing=1.28, space_after=0)
        return y + Inches(0.95)
    return y + Inches(0.34)


def footer(s, n):
    rule(s, H - Inches(0.62), color=RULE)
    text(s, M, H - Inches(0.5), Inches(8), Inches(0.3),
         "hogd database sharding  ·  vp/sharding-v2-db-abstraction  ·  PR #3108",
         size=9, font=MONO, color=FAINT, letter=0.6, space_after=0)
    text(s, W - M - Inches(1.2), H - Inches(0.5), Inches(1.2), Inches(0.3),
         f"{n:02d}", size=9, font=MONO, color=FAINT, align=PP_ALIGN.RIGHT,
         space_after=0)


def bullets(s, l, t, w, items, size=14.5, gap=0.2, dotcolor=ACCENT,
            spacing=1.22):
    """items: list of (lead, rest); lead is bold sans, rest serif.

    Advances by measured line count: a proportional face averages ~0.0072in of
    advance per point of size per character, so chars-per-line is width / (size
    * 0.0072). gap is the extra space between items, not part of the line box.
    """
    y = t
    for lead, rest in items:
        box(s, l, y + Inches(0.105), Pt(4.5), Pt(4.5), fill=dotcolor,
            shape=MSO_SHAPE.OVAL)
        runs = []
        if lead:
            runs.append((lead, {"font": SANS, "bold": True, "size": size,
                                "color": INK}))
            if rest:
                runs.append(("  ", {}))
        if rest:
            runs.append((rest, {"color": INK_SOFT, "size": size}))
        avail = w.inches - 0.28
        text(s, l + Inches(0.28), y, w - Inches(0.28), Inches(0.4), [runs],
             size=size, spacing=spacing, space_after=0)
        chars = len(lead or "") + len(rest or "") + (2 if lead and rest else 0)
        per_line = max(1, int(avail / (size * 0.0072)))
        lines = max(1, -(-chars // per_line))
        y = y + Inches(lines * size * spacing / 72.0 + gap)
    return y


def table(s, l, t, w, cols, rows, widths, size=12.5, rowh=0.36, headh=0.34):
    """cols: header strings. rows: list of list of (text, {opts})|str."""
    n_r, n_c = len(rows) + 1, len(cols)
    gt = s.shapes.add_table(n_r, n_c, l, t, w,
                            Inches(headh + rowh * len(rows))).table
    gt.first_row = False
    gt.horz_banding = False
    # kill default style banding
    tblPr = gt._tbl.find(qn('a:tblPr'))
    for a in ('firstRow', 'bandRow'):
        if tblPr.get(a):
            tblPr.set(a, '0')
    total = sum(widths)
    for i, frac in enumerate(widths):
        gt.columns[i].width = Emu(int(w * frac / total))
    gt.rows[0].height = Inches(headh)
    for i in range(len(rows)):
        gt.rows[i + 1].height = Inches(rowh)

    def cell(c, chunks, hdr=False, top=None):
        c.fill.solid()
        c.fill.fore_color.rgb = ACC_SOFT if hdr else WHITE
        c.margin_left = c.margin_right = Inches(0.11)
        c.margin_top = c.margin_bottom = Inches(0.035)
        c.vertical_anchor = MSO_ANCHOR.MIDDLE
        tf = c.text_frame
        tf.word_wrap = True
        p = tf.paragraphs[0]
        p.line_spacing = 1.06
        if isinstance(chunks, str):
            chunks = [(chunks, {})]
        for txt, o in chunks:
            r = p.add_run()
            r.text = txt.upper() if hdr else txt
            f = r.font
            f.name = o.get("font", MONO if hdr else SERIF)
            f.size = Pt(o.get("size", 9.5 if hdr else size))
            f.bold = o.get("bold", hdr)
            f.color.rgb = o.get("color", ACCENT if hdr else INK)
            if hdr:
                f._rPr.set("spc", "110")
        if o.get("align") == "r" or (isinstance(chunks, list) and
                                     chunks and chunks[0][1].get("align") == "r"):
            p.alignment = PP_ALIGN.RIGHT

    for j, ct in enumerate(cols):
        cell(gt.cell(0, j), ct if isinstance(ct, str) else ct[0], hdr=True)
        if not isinstance(ct, str) and ct[1].get("align") == "r":
            gt.cell(0, j).text_frame.paragraphs[0].alignment = PP_ALIGN.RIGHT
    for i, row in enumerate(rows):
        for j, ct in enumerate(row):
            cell(gt.cell(i + 1, j), ct)
    return gt


def chip(s, l, t, label, fg, bg, w=Inches(0.95)):
    c = box(s, l, t, w, Inches(0.24), fill=bg)
    tf = c.text_frame
    tf.margin_left = tf.margin_right = Inches(0.05)
    tf.margin_top = tf.margin_bottom = 0
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = label.upper()
    r.font.name, r.font.size, r.font.bold = MONO, Pt(8.5), True
    r.font.color.rgb = fg
    r.font._rPr.set("spc", "90")
    return c


N = [0]
def done(s):
    N[0] += 1
    footer(s, N[0])

# ================================================================ 1 title
s = slide("Frame it in one sentence: the database stops being a single process "
          "without the rest of the system knowing, and without changing what a "
          "block header says. Everything else serves that. Two and a half "
          "months: Jun 29 to Aug 18.")
band = box(s, 0, 0, W, Inches(0.14), fill=ACCENT)
text(s, M, Inches(1.15), CW, Inches(0.3),
     "Engineering overview  ·  29 Jun – 18 Aug 2026", size=11.5, font=MONO,
     color=ACCENT, caps=True, letter=1.8, space_after=0)
text(s, M, Inches(1.62), Inches(10.6), Inches(1.9),
     "Sharding the hogd database", size=58, font=SANS, color=INK, bold=True,
     spacing=0.92, space_after=0)
text(s, M, Inches(3.42), Inches(8.4), Inches(1.0),
     "One logical database, N hogdb processes — routed by address, invisible "
     "to consensus, and validated end-to-end on the cluster.",
     size=19, color=INK_SOFT, spacing=1.34, space_after=0)
rule(s, Inches(4.72), color=INK, weight=1.5)
facts = [("Branch", "vp/sharding-v2", "PR #3108 · draft"),
         ("Commits", "44", "47 files · ~2,650 ins."),
         ("New tests", "31", "across 5 new suites"),
         ("Cluster runs", "175", "9 hosts · 1/2/4 shards"),
         ("Issues filed", "10", "6 outside sharding")]
fw = CW / 5
for i, (k, v, sub) in enumerate(facts):
    x = M + Emu(int(fw * i))
    text(s, x, Inches(4.98), Emu(int(fw)) - Inches(0.2), Inches(0.25), k,
         size=9.5, font=MONO, color=FAINT, caps=True, letter=1.4, space_after=0)
    text(s, x, Inches(5.28), Emu(int(fw)) - Inches(0.2), Inches(0.32), v,
         size=18, font=SANS, color=INK, bold=True, space_after=0)
    text(s, x, Inches(5.66), Emu(int(fw)) - Inches(0.15), Inches(0.3), sub,
         size=9, font=MONO, color=FAINT, space_after=0)
text(s, M, H - Inches(0.95), Inches(8), Inches(0.3), "Vinita Pawar",
     size=12, font=SANS, color=INK_SOFT, space_after=0)
N[0] += 1

# ================================================================ 2 problem
s = slide("Name the three clients early -- the whole talk comes back to them. "
          "They had nothing in common before this work; the routing core is "
          "what they now share. The failure mode to stress: disagreement "
          "between two clients is silent. You get Not Found, which most call "
          "sites read as 'no such account'.")
y = header(s, "The problem", "hogdb was one process holding all of state",
           "Balances, auth data, contract data, retrocausal queues, wasm code, "
           "hog ages — all in a single hogdb. That process is the memory and "
           "write-bandwidth ceiling for the whole validator.")
text(s, M, y, CW, Inches(0.3),
     "Three subsystems talk to it directly, each with its own concurrency model",
     size=11, font=MONO, color=FAINT, caps=True, letter=1.3, space_after=0)
cards = [("mempool scheduler", "async, coroutine RPC", "ShardedDbSched"),
         ("hogexec", "synchronous, per-transaction", "DatabaseProxy"),
         ("auth server", "async, subscription-driven", "DBAuthClient")]
cw3 = (CW - Inches(0.5)) / 3
for i, (t1, t2, t3) in enumerate(cards):
    x = M + Emu(int((cw3 + Inches(0.25)) * i))
    box(s, x, y + Inches(0.42), Emu(int(cw3)), Inches(1.28), fill=WHITE, line=RULE)
    box(s, x, y + Inches(0.42), Pt(3), Inches(1.28), fill=ACCENT)
    text(s, x + Inches(0.28), y + Inches(0.62), Emu(int(cw3)) - Inches(0.5),
         Inches(0.3), t1, size=16, font=SANS, color=INK, bold=True, space_after=0)
    text(s, x + Inches(0.28), y + Inches(0.98), Emu(int(cw3)) - Inches(0.5),
         Inches(0.3), t2, size=13, color=INK_SOFT, space_after=0)
    text(s, x + Inches(0.28), y + Inches(1.32), Emu(int(cw3)) - Inches(0.5),
         Inches(0.3), t3, size=10.5, font=MONO, color=ACCENT, space_after=0)
yy = y + Inches(2.0)
box(s, M, yy, CW, Inches(1.05), fill=CRIT_SOFT)
box(s, M, yy, Pt(3.5), Inches(1.05), fill=CRIT)
text(s, M + Inches(0.32), yy + Inches(0.17), Inches(2.6), Inches(0.25),
     "The core failure mode", size=9.5, font=MONO, color=CRIT, caps=True,
     letter=1.3, space_after=0)
text(s, M + Inches(0.32), yy + Inches(0.46), CW - Inches(0.7), Inches(0.5),
     "If two clients disagree about which shard owns an address, the write "
     "lands on one shard and the read asks another. There is no error — you "
     "get Not Found, which most call sites treat as “no such account.”",
     size=14, color=INK, spacing=1.25, space_after=0)
done(s)

# ================================================================ 3 invariant
s = slide("State this as the thesis. The three bullets are the three ways it "
          "breaks in practice: different code, different hash version, or the "
          "right code pointed at the wrong socket. Each got its own mechanism, "
          "and each shipped after a real bug.")
box(s, 0, 0, W, H, fill=INK)
box(s, 0, 0, W, Inches(0.14), fill=ACCENT)
text(s, M, Inches(1.5), Inches(2.6), Inches(0.3), "The one invariant",
     size=11, font=MONO, color=RGBColor(0x57, 0xC4, 0xB2), caps=True,
     letter=1.7, space_after=0)
text(s, M, Inches(2.0), Inches(10.4), Inches(1.5),
     "A read must repeat the routing its write used.",
     size=46, font=SANS, color=WHITE, bold=True, spacing=1.0, space_after=0)
rule(s, Inches(4.05), l=M, w=Inches(2.2), color=RGBColor(0x57, 0xC4, 0xB2),
     weight=2)
items = [("Same routing function in all three clients",
          "— so it lives in one shared header, not three implementations."),
         ("Identified by kShardFunctionId, persisted per shard",
          "— changing it is a deliberate, detected event."),
         ("Shards bound by declared shard id",
          "— never by connection-arrival order.")]
yy = Inches(4.42)
for lead, rest in items:
    box(s, M, yy + Inches(0.11), Pt(4.5), Pt(4.5),
        fill=RGBColor(0x57, 0xC4, 0xB2), shape=MSO_SHAPE.OVAL)
    text(s, M + Inches(0.3), yy, Inches(11.2), Inches(0.35),
         [[(lead, {"font": SANS, "bold": True, "size": 15,
                   "color": RGBColor(0xE4, 0xEA, 0xE7)}),
           ("  " + rest, {"size": 15, "color": RGBColor(0xA5, 0xB0, 0xAC)})]],
         spacing=1.2, space_after=0)
    yy += Inches(0.58)
done(s)

# ================================================================ 4 diagram
s = slide("The zero-copy path is worth pausing on: notify_vetted batches reach "
          "~250 MB. Materialising per-shard sub-batches would double that. "
          "Instead the split records index vectors plus a borrow of the source, "
          "and the gather view serialises the source items in place.")
y = header(s, "What shipped", "One routing rule, three client paths",
           "A new module — hogd/db_shard_routing — owns key placement, batch "
           "splitting and concurrent fan-out. Every subsystem goes through it "
           "instead of holding its own database client.")
bw, bh = Inches(2.75), Inches(0.86)
lx, cx, rx = M, Inches(5.3), Inches(9.55)
labels = [("mempool scheduler", "ShardedDbSched"),
          ("auth server", "DBAuthClient"),
          ("hogexec", "DatabaseProxy")]
for i, (a, b) in enumerate(labels):
    ty = y + Inches(0.1 + i * 1.12)
    box(s, lx, ty, bw, bh, fill=WHITE, line=RULE_STR)
    text(s, lx + Inches(0.18), ty + Inches(0.14), bw - Inches(0.3), Inches(0.3),
         a, size=13.5, font=SANS, color=INK, bold=True, space_after=0)
    text(s, lx + Inches(0.18), ty + Inches(0.46), bw - Inches(0.3), Inches(0.3),
         b, size=10, font=MONO, color=FAINT, space_after=0)
    ar = box(s, lx + bw + Inches(0.12), ty + Inches(0.3), Inches(0.72),
             Inches(0.22), fill=ACC_SOFT, shape=MSO_SHAPE.RIGHT_ARROW)
    ar.line.color.rgb = RULE_STR
    ar.line.width = Pt(0.5)
# core
core_h = Inches(2.82)
box(s, cx, y + Inches(0.1), Inches(3.65), core_h, fill=ACC_SOFT, line=ACCENT, lw=1.5)
text(s, cx + Inches(0.28), y + Inches(0.3), Inches(3.1), Inches(0.3),
     "db_shard_routing", size=17, font=SANS, color=ACCENT, bold=True, space_after=0)
core_lines = ["shard_of_address()", "split_by_shard()", "gather_positional()",
              "ShardedClient fan-out", "order_shards_by_id()"]
for i, cl in enumerate(core_lines):
    text(s, cx + Inches(0.28), y + Inches(0.78 + i * 0.36), Inches(3.2),
         Inches(0.3), cl, size=12, font=MONO,
         color=INK_SOFT if i < 4 else FAINT, space_after=0)
for i in range(3):
    ty = y + Inches(0.1 + i * 1.12)
    ar = box(s, cx + Inches(3.77), ty + Inches(0.3), Inches(0.62), Inches(0.22),
             fill=ACC_SOFT, shape=MSO_SHAPE.RIGHT_ARROW)
    ar.line.color.rgb = RULE_STR
    ar.line.width = Pt(0.5)
    if i < 2:
        box(s, rx, ty, Inches(2.9), bh, fill=WHITE, line=RULE_STR)
        text(s, rx + Inches(0.18), ty + Inches(0.14), Inches(2.5), Inches(0.3),
             f"hogdb shard {i}", size=13.5, font=SANS, color=INK, bold=True,
             space_after=0)
        text(s, rx + Inches(0.18), ty + Inches(0.46), Inches(2.6), Inches(0.3),
             "owns 'B' 'D' 'K' 'Q' 'A' keys" if i == 0
             else "+ own globals, own block tx",
             size=10, font=MONO, color=FAINT, space_after=0)
    else:
        d = box(s, rx, ty, Inches(2.9), bh, line=RULE)
        d.line.dash_style = 4
        text(s, rx + Inches(0.18), ty + Inches(0.3), Inches(2.5), Inches(0.3),
             "…  shard N−1", size=13, font=MONO, color=FAINT, space_after=0)
yy = y + Inches(3.14)
rule(s, yy)
text(s, M, yy + Inches(0.16), CW, Inches(0.3),
     [[("Divergence between the three is a data-loss bug, not a performance "
        "bug", {"font": SANS, "bold": True, "size": 13.5, "color": INK}),
       ("  — which is why the routing lives in one shared header.",
        {"size": 13.5, "color": INK_SOFT})]], space_after=0)
done(s)

# ================================================================ 5 what is sharded
s = slide("Walk the placement column, not the rows. Four behaviours, and each "
          "one is forced by a constraint. The two rows to dwell on: 'K' and "
          "'Q' carry a key field after the address and routing ignores it -- "
          "that is deliberate co-location, so a single-account transaction "
          "touches a single shard. The cost is that a hot account cannot be "
          "split, and that is the first thing to revisit if a hot-account "
          "workload shows up. 'R' is declared and hashed but nothing writes "
          "it; if it ever does, it needs a placement decision. Tie the last "
          "column back to the block hash: every hashed item must land on "
          "exactly one shard, or the partials stop summing.")
y = header(s, "The data model", "What actually gets sharded",
           "A ramdb key is a one-byte prefix and then 32-byte fields. Ten "
           "prefixes, four placement behaviours — and the last column is what "
           "makes the partial hashes sum.")

KEYED = [("'B'", "address → native token balance", "yes"),
         ("'D'", "address → auth data", "yes"),
         ("'M'", "address → deployment metadata", "yes"),
         ("'K'", "address | key → contract data", "yes"),
         ("'Q'", "address | key → retrocausal queue block", "yes"),
         ("'U'", "address → replay cache", "yes"),
         ("'A'", "address → hog age", "no")]

rows = []
for k, desc, hashed in KEYED:
    note = "  (key ignored)" if k in ("'K'", "'Q'") else ""
    rows.append([
        [(k, {"font": MONO, "bold": True, "size": 12})],
        [(desc, {"size": 11.5})],
        [("keyed", {"font": SANS, "bold": True, "size": 11.5, "color": INK}),
         ("  shard_of_address" + note, {"font": MONO, "size": 10,
                                       "color": FAINT})],
        [(hashed, {"font": MONO, "size": 10.5, "align": "r",
                   "color": ACCENT if hashed == "yes" else FAINT})]])
rows.append([
    [("'C'", {"font": MONO, "bold": True, "size": 12})],
    [("code hash → wasm code", {"size": 11.5})],
    [("content-addressed", {"font": SANS, "bold": True, "size": 11.5,
                            "color": AMBER}),
     ("  shard 0", {"font": MONO, "size": 10, "color": FAINT})],
    [("yes", {"font": MONO, "size": 10.5, "align": "r", "color": ACCENT})]])
rows.append([
    [("'<'", {"font": MONO, "bold": True, "size": 12})],
    [("db tip, cost model, shard identity", {"size": 11.5})],
    [("replicated", {"font": SANS, "bold": True, "size": 11.5, "color": ACCENT}),
     ("  every shard", {"font": MONO, "size": 10, "color": FAINT})],
    [("no", {"font": MONO, "size": 10.5, "align": "r", "color": FAINT})]])
rows.append([
    [("'R'", {"font": MONO, "bold": True, "size": 12, "color": FAINT})],
    [("wasm code refcount", {"size": 11.5, "color": FAINT})],
    [("undecided", {"font": SANS, "bold": True, "size": 11.5, "color": FAINT}),
     ("  declared, nothing writes it", {"font": MONO, "size": 10,
                                        "color": FAINT})],
    [("yes", {"font": MONO, "size": 10.5, "align": "r", "color": FAINT})]])

table(s, M, y, Inches(11.6),
      ["Key", "Contents", "Placement", ("In db hash", {"align": "r"})],
      rows, [0.55, 3.5, 3.9, 0.95], size=11.5, rowh=0.265, headh=0.28)

yy = y + Inches(3.20)
cw2 = (CW - Inches(0.3)) / 2
box(s, M, yy, Emu(int(cw2)), Inches(1.0), fill=WHITE, line=RULE)
box(s, M, yy, Pt(3.5), Inches(1.0), fill=ACCENT)
text(s, M + Inches(0.3), yy + Inches(0.14), Emu(int(cw2)) - Inches(0.5),
     Inches(0.25), "Why 'K' and 'Q' ignore their key field", size=9.5,
     font=MONO, color=ACCENT, caps=True, letter=1.2, space_after=0)
text(s, M + Inches(0.3), yy + Inches(0.40), Emu(int(cw2)) - Inches(0.55),
     Inches(0.5),
     "Deliberate co-location: everything one account owns lands on one shard, "
     "so a single-account transaction touches a single shard. The cost is that "
     "a hot account cannot be split.", size=11, color=INK_SOFT,
     spacing=1.2, space_after=0)
rx2 = M + Emu(int(cw2)) + Inches(0.3)
box(s, rx2, yy, Emu(int(cw2)), Inches(1.0), fill=WHITE, line=RULE)
box(s, rx2, yy, Pt(3.5), Inches(1.0), fill=AMBER)
text(s, rx2 + Inches(0.3), yy + Inches(0.14), Emu(int(cw2)) - Inches(0.5),
     Inches(0.25), "Not a key at all — the remaining gap", size=9.5,
     font=MONO, color=AMBER, caps=True, letter=1.2, space_after=0)
text(s, rx2 + Inches(0.3), yy + Inches(0.40), Emu(int(cw2)) - Inches(0.55),
     Inches(0.5),
     "Viewing snapshot ids are minted per shard, so they are not routed by "
     "anything. Snapshots open on the primary while reads route to the owner — "
     "the one placement hole left.", size=11, color=INK_SOFT, spacing=1.2,
     space_after=0)
done(s)

# ================================================================ 5 decisions A
s = slide("The prettiest part of the design is the additive hash. Shard count "
          "becomes a local deployment decision, invisible to consensus -- a "
          "1-shard and a 4-shard validator produce byte-identical headers. "
          "That falls out of LtHash16 being a homomorphic multiset hash; with "
          "a Merkle root it would have needed a protocol change.")
y = header(s, "Design decisions  ·  1 of 2", "Placement, hashing, identity")
dec = [("Key placement", "Everything an account owns lands on one shard",
        "Seven of the ten key prefixes route on the address alone, so "
        "everything one account owns lands on one shard. Code is "
        "content-addressed to shard 0; globals are replicated to every shard. "
        "Four placement behaviours, each forced by a constraint."),
       ("Block hash", "Partial hashes that sum",
        "Each shard hashes only its own items. LtHash16 values are additive, "
        "so the partials sum to exactly the unsharded hash. The block header "
        "therefore does not depend on how many shards a validator runs — with "
        "no protocol change."),
       ("Shard identity", "A shard knows which shard it is",
        "--shard-id / --total-shards, persisted in a record excluded from the "
        "consensus hash. The persisted record is authoritative and flags may "
        "only confirm it; relabelling a populated database is fail-stop. "
        "Clients bind by declared id.")]
cw3 = (CW - Inches(0.44)) / 3
for i, (tag, ttl, body) in enumerate(dec):
    x = M + Emu(int((cw3 + Inches(0.22)) * i))
    box(s, x, y, Emu(int(cw3)), Inches(4.5), fill=WHITE, line=RULE)
    box(s, x, y, Emu(int(cw3)), Pt(3), fill=ACCENT)
    text(s, x + Inches(0.26), y + Inches(0.28), Emu(int(cw3)) - Inches(0.5),
         Inches(0.25), tag, size=9.5, font=MONO, color=ACCENT, caps=True,
         letter=1.4, space_after=0)
    text(s, x + Inches(0.26), y + Inches(0.6), Emu(int(cw3)) - Inches(0.5),
         Inches(0.8), ttl, size=17, font=SANS, color=INK, bold=True,
         spacing=1.02, space_after=0)
    text(s, x + Inches(0.26), y + Inches(1.62), Emu(int(cw3)) - Inches(0.5),
         Inches(2.6), body, size=12.5, color=INK_SOFT, spacing=1.26,
         space_after=0)
done(s)

# ================================================================ 6 decisions B
s = slide("Rule 2 -- drain before rethrowing -- was found in review, not in "
          "testing. There are two ways to throw: the rethrown RPC error from a "
          "co_await, and send_call throwing synchronously while still "
          "launching. The second is nastier because some Asyncs exist and some "
          "do not yet. Fixing it once in a shared drain_awaits is why all four "
          "helpers have the same structure.")
y = header(s, "Design decisions  ·  2 of 2", "Fan-out, batching, control plane")
dec = [("Fan-out discipline", "Launch all, drain, visit every shard",
        "All shard calls are issued before any is awaited, so N shards cost "
        "one round trip. A throw mid-gather drains its siblings first — "
        "abandoning a running x23::Async is a use-after-free. And no fan-out "
        "ever early-returns: stopping partway through a commit leaves some "
        "shards committed and others only prepared, which abort cannot undo."),
       ("Zero copy", "Splitting without duplicating 250 MB",
        "notify_vetted batches reach ~250 MB. Rather than materialise "
        "per-shard sub-batches, the split records only per-shard index vectors "
        "plus a borrow of the source, and a gather view serialises the source "
        "items in place through an x23 serialize_equiv specialisation."),
       ("Control plane", "Replicated globals, agreement required",
        "Every shard runs the whole block lifecycle against its own globals. "
        "Writes go to all; reads require unanimity — get_tip fail-stops on "
        "partial genesis or any tip mismatch. That is the startup detector for "
        "a crash mid-commit-fan-out. Loud beats livelock.")]
cw3 = (CW - Inches(0.44)) / 3
for i, (tag, ttl, body) in enumerate(dec):
    x = M + Emu(int((cw3 + Inches(0.22)) * i))
    box(s, x, y, Emu(int(cw3)), Inches(4.5), fill=WHITE, line=RULE)
    box(s, x, y, Emu(int(cw3)), Pt(3), fill=ACCENT)
    text(s, x + Inches(0.26), y + Inches(0.28), Emu(int(cw3)) - Inches(0.5),
         Inches(0.25), tag, size=9.5, font=MONO, color=ACCENT, caps=True,
         letter=1.4, space_after=0)
    text(s, x + Inches(0.26), y + Inches(0.6), Emu(int(cw3)) - Inches(0.5),
         Inches(0.8), ttl, size=17, font=SANS, color=INK, bold=True,
         spacing=1.02, space_after=0)
    text(s, x + Inches(0.26), y + Inches(1.62), Emu(int(cw3)) - Inches(0.5),
         Inches(2.6), body, size=12.5, color=INK_SOFT, spacing=1.26,
         space_after=0)
done(s)

# ================================================================ 7 timeline
s = slide("Five phases. Two of them are bring-up, two are hardening, one is "
          "measurement. If asked which was hardest: phase 3, because the auth "
          "stall had no log signal at all -- height was climbing fast with "
          "tx_count 0, which looks exactly like healthy consensus.")
y = header(s, "How it went", "Five phases, June to August")
phases = [
    ("Phase 1", "29 Jun – 9 Jul", "Make the executor shard-shaped, then extract the layer",
     "Reshaped hogexec's single db client into a per-shard array, turned on "
     "per-key routing, made writes a two-phase fan-out. Then pulled routing, "
     "splitting and gathering into db_shard_routing so all three clients could "
     "share it."),
    ("Phase 2", "16 – 24 Jul", "First 2-shard cluster runs — and the block-0 stall",
     "First multi-shard run stalled at block 0 with a mempool SIGSEGV: a "
     "genesis-delta ordering race. Fixed with a readiness barrier. By 24 Jul a "
     "run completed all 1,000 work batches, ~39.9M txs at ~18,000 tx/s."),
    ("Phase 3", "27 Jul – 4 Aug", "Correctness hardening, and the silent auth stall",
     "Shard identity end-to-end, fail-stop on divergent tips, per-block "
     "transfer barrier, shard-owner-only genesis, golden routing values. "
     "Removing replicated genesis exposed a latent single-shard auth binding: "
     "16,945 empty blocks in 28 minutes, no crash, nothing in any log."),
    ("Phase 4", "3 – 4 Aug", "Review, tests, and a validated run",
     "Six review rounds; five fan-out bugs in one pass. 31 tests across five "
     "new suites, CI clean. The 4 Aug run: 79,490,644 txs across 343 non-empty "
     "blocks, shards balanced 49% / 50%, auth_failed zero, no cores."),
    ("Phase 5", "10 – 18 Aug", "Benchmark campaign — 175 runs at 1, 2 and 4 shards",
     "Built a repeatable harness and swept shard count, executor count, cache "
     "size, batch size and workload shape. Produced the performance and "
     "capacity answers — and four more bugs, none of them sharding's."),
]
yy = y + Inches(0.0)
for i, (ph, when, ttl, body) in enumerate(phases):
    if i:
        rule(s, yy - Inches(0.12))
    text(s, M, yy, Inches(1.5), Inches(0.22), ph, size=9, font=MONO,
         color=ACCENT, caps=True, letter=1.4, space_after=0)
    text(s, M, yy + Inches(0.24), Inches(1.5), Inches(0.25), when, size=11,
         font=MONO, color=FAINT, space_after=0)
    text(s, M + Inches(1.6), yy - Inches(0.03), Inches(10.1), Inches(0.3), ttl,
         size=15.5, font=SANS, color=INK, bold=True, space_after=0)
    text(s, M + Inches(1.6), yy + Inches(0.29), Inches(10.2), Inches(0.5), body,
         size=12, color=INK_SOFT, spacing=1.18, space_after=0)
    yy += Inches(1.02)
done(s)

# ================================================================ 8 throughput
s = slide("Be precise here. Throughput is flat then declining. The phase split "
          "is the useful part: commit gets 24% faster with shards while "
          "nomination gets 61% slower and swamps the gain. Nomination is where "
          "the scheduler does its per-address hog-age reads -- scatter_gather "
          "batches correctly, one RPC per shard, but waits for the slowest "
          "shard. That is the engineering target if throughput matters. Also: "
          "executors do matter where shards do not -- 1 vs 2 executors is "
          "-1.5% at one shard but -21% at four.")
y = header(s, "Results  ·  throughput", "Sharding does not make it faster",
           "Two executors, 1M-tx batches, matched block windows.")
table(s, M, y, Inches(7.4),
      ["Shards", ("tx/s", {"align": "r"}), ("vs 1 shard", {"align": "r"}),
       ("% of linear", {"align": "r"}), ("Median block", {"align": "r"})],
      [["1", [("211,182", {"font": MONO, "align": "r"})],
        [("—", {"font": MONO, "align": "r"})],
        [("—", {"font": MONO, "align": "r"})],
        [("4.11 s", {"font": MONO, "align": "r"})]],
       ["2", [("214,325", {"font": MONO, "align": "r"})],
        [("+1.5%", {"font": MONO, "align": "r", "color": ACCENT, "bold": True})],
        [("51%", {"font": MONO, "align": "r"})],
        [("4.17 s", {"font": MONO, "align": "r"})]],
       ["4", [("196,985", {"font": MONO, "align": "r"})],
        [("−6.7%", {"font": MONO, "align": "r", "color": CRIT, "bold": True})],
        [("23%", {"font": MONO, "align": "r"})],
        [("4.59 s", {"font": MONO, "align": "r"})]]],
      [1.0, 1.1, 1.2, 1.2, 1.3], rowh=0.38)
yy = y + Inches(1.62)
box(s, M, yy, Inches(7.4), Inches(1.62), fill=WHITE, line=RULE)
box(s, M, yy, Pt(3.5), Inches(1.62), fill=ACCENT)
text(s, M + Inches(0.3), yy + Inches(0.2), Inches(6.6), Inches(0.25),
     "Where the time goes", size=9.5, font=MONO, color=ACCENT, caps=True,
     letter=1.3, space_after=0)
text(s, M + Inches(0.3), yy + Inches(0.5), Inches(6.7), Inches(1.0),
     [[("Commit gets 24% faster with shards ", {"font": SANS, "bold": True,
                                                "size": 13.5, "color": ACCENT}),
       ("(2,194 → 1,666 ms) while ", {"size": 13.5, "color": INK_SOFT}),
       ("nomination gets 61% slower ", {"font": SANS, "bold": True,
                                        "size": 13.5, "color": CRIT}),
       ("(1,352 → 2,180 ms) and swamps the gain.", {"size": 13.5,
                                                    "color": INK_SOFT})]],
     spacing=1.25, space_after=0)
text(s, M + Inches(0.3), yy + Inches(1.14), Inches(6.7), Inches(0.4),
     "Nomination is where the scheduler does its per-address hog-age reads.",
     size=12, color=INK_SOFT, space_after=0)
# right column
rx2 = M + Inches(7.85)
text(s, rx2, y, Inches(3.9), Inches(0.3), "The engineering target", size=11,
     font=MONO, color=FAINT, caps=True, letter=1.3, space_after=0)
text(s, rx2, y + Inches(0.36), Inches(4.0), Inches(2.8),
     "scatter_gather batches correctly — one RPC per shard, not one per "
     "account — but it waits for the slowest shard, so its cost grows with "
     "shard count.\n\nThat is the specific thing to fix if sharding "
     "throughput matters. Nobody has worked it yet.",
     size=13.5, color=INK_SOFT, spacing=1.3, space_after=8)
box(s, rx2, y + Inches(2.5), Inches(4.0), Inches(0.9), fill=ACC_SOFT)
text(s, rx2 + Inches(0.24), y + Inches(2.66), Inches(3.6), Inches(0.6),
     "Executors do matter where shards do not: 1 vs 2 executors is −1.5% at "
     "one shard, but −21% at four.", size=12, color=INK, spacing=1.22,
     space_after=0)
done(s)

# ================================================================ 9 capacity
s = slide("This is the result that justifies the work. Both unsharded runs "
          "failed; every sharded run finished. The occupancy imbalance is "
          "deliberate -- shard 0 carries 2-3x its peers. And state the caveat "
          "out loud, because it bounds every capacity number in the campaign.")
y = header(s, "Results  ·  capacity", "Sharding buys headroom — which is what it was for",
           "State grows with transaction count (--num-virtual-accts), 10 GB per shard.")
table(s, M, y, Inches(11.6),
      ["Shards", "Occupancy", "Outcome", ("Txs committed", {"align": "r"})],
      [["1", [("78%", {"font": MONO})],
        [("stalled — mempool aborted", {"color": CRIT, "bold": True})],
        [("33M", {"font": MONO, "align": "r"})]],
       ["2", [("77% / 35%", {"font": MONO})],
        [("completed", {"color": ACCENT, "bold": True})],
        [("40.5M", {"font": MONO, "align": "r"})]],
       ["4", [("62% / 20% / 21% / 20%", {"font": MONO})],
        [("completed", {"color": ACCENT, "bold": True})],
        [("39M", {"font": MONO, "align": "r"})]]],
      [0.8, 2.0, 2.4, 1.4], rowh=0.4)
yy = y + Inches(1.75)
text(s, M, yy, Inches(11.6), Inches(0.4),
     [[("Both unsharded runs failed; every sharded run finished.",
        {"font": SANS, "bold": True, "size": 17, "color": INK})]],
     space_after=0)
yy += Inches(0.6)
box(s, M, yy, Inches(11.6), Inches(1.85), fill=AMB_SOFT)
box(s, M, yy, Pt(3.5), Inches(1.85), fill=AMBER)
text(s, M + Inches(0.32), yy + Inches(0.2), Inches(9), Inches(0.25),
     "The caveat that bounds every capacity number", size=9.5, font=MONO,
     color=AMBER, caps=True, letter=1.3, space_after=0)
text(s, M + Inches(0.32), yy + Inches(0.52), Inches(11), Inches(1.2),
     "The mempool holds at most 5,000,000 pending transactions and silently "
     "discards the rest — a hardcoded constant with an existing TODO asking "
     "for it to be a flag. Offering 1.3 billion transactions committed only "
     "78.8M.\nRaising it is the prerequisite for any large-capacity "
     "demonstration — but not before the detached-coroutine guard lands, "
     "because a 50M backlog reaches 41.8 GB of mempool RSS and 252 MB db "
     "messages. That is issue #10.",
     size=13, color=INK, spacing=1.28, space_after=0)
done(s)

# ================================================================ 10 bugs
s = slide("Lead with the framing: most of these are not sharding bugs. They "
          "are pre-existing assumptions that only break when something "
          "exercises them, and sharding was the first thing to do so. That "
          "reframes the list from 'sharding is unstable' to 'sharding is a "
          "load-bearing test of main'.")
y = header(s, "Side effects", "Sharding was a load-bearing test of main",
           "Ten issues written up with repro steps and artifacts. Most are not "
           "sharding bugs.")
rows = []
data = [("10", "hogdb dies silently at a 50M backlog and takes the mempool "
         "with it — unguarded co_await in a detached coroutine, third call site",
         "pre-existing", "open"),
        ("9", "Every benchmark abandons its mempool backlog: the load driver "
         "stops nominating when its batch list runs out", "groundhog-go", "open"),
        ("8", "Event server work dir never deleted — reached 146 GB and "
         "stalled five consecutive runs", "pre-existing", "fixed"),
        ("7", "Hog-conservation invariant is whole-db but evaluated per shard: "
         "assertion-enabled multi-shard runs die at genesis", "shard-adjacent",
         "open"),
        ("5", "Genesis reports complete before its hog accounting lands — "
         "1 shard wins the race by luck, N shards lose it reliably",
         "pre-existing", "fixed"),
        ("4", "Auth server bound to one shard → 100% auth-fail and empty "
         "blocks forever", "pre-existing", "fixed"),
        ("3", "chuckd panics on teardown; the race window scales with submit "
         "latency, which 2-shard fan-out widens to ~120 ms", "groundhog-go",
         "open"),
        ("1", "Mempool SIGSEGV in process_transfers; 2-shard runs stall at "
         "block 0", "sharding", "fixed")]
for num, desc, origin, st in data:
    stc = (ACCENT, ACC_SOFT) if st == "fixed" else (AMBER, AMB_SOFT)
    rows.append([[(num, {"font": MONO, "size": 11})],
                 [(desc, {"size": 11.5})],
                 [(origin, {"font": MONO, "size": 10, "color": FAINT})],
                 [(st.upper(), {"font": MONO, "size": 9.5, "bold": True,
                                "color": stc[0]})]])
table(s, M, y, Inches(11.6), ["#", "Issue", "Origin", "Status"], rows,
      [0.4, 6.6, 1.5, 1.0], size=11.5, rowh=0.44, headh=0.3)
done(s)

# ================================================================ 11 findings
s = slide("The CHECK-is-UB finding is the one to escalate outside this "
          "project. It cost days on a 37 GB core to establish that a clean "
          "CHECK failure was hiding behind a SIGSEGV, and it applies to every "
          "CHECK in every Release binary we ship. Also note the third one: "
          "every performance number measured from this build directory before "
          "10 Aug was assertion-enabled.")
y = header(s, "Side effects", "Three findings worth escalating beyond this project")
find = [("A failing CHECK in a Release build is undefined behaviour",
         "Under NDEBUG, x23::panic compiles to std::unreachable(), so a "
         "failing CHECK at -O3 became a SIGSEGV — no CHECK FAILED line, "
         "garbage backtrace. Days on a 37 GB core to establish that a clean "
         "assertion failure was hiding behind a segfault. This applies to "
         "every CHECK in every Release binary we ship.", CRIT, CRIT_SOFT),
        ("An unguarded co_await in a detached coroutine is fatal to the process",
         "On stream failure every in-flight call is handed an empty buffer; "
         "deserialising it throws, and a detached coroutine with no handler "
         "terminates. Three separate call sites have now killed the mempool "
         "this way.", CRIT, CRIT_SOFT),
        ("Release builds in this tree were not NDEBUG",
         "A vendored dependency does add_definitions(-UNDEBUG) for every "
         "non-Debug build, and it lands after the release flags on the "
         "command line. Every performance number measured from this build "
         "directory before 10 August was assertion-enabled.", AMBER, AMB_SOFT)]
yy = y + Inches(0.1)
for i, (ttl, body, fg, bg) in enumerate(find):
    box(s, M, yy, CW, Inches(1.44), fill=WHITE, line=RULE)
    box(s, M, yy, Pt(3.5), Inches(1.44), fill=fg)
    text(s, M + Inches(0.34), yy + Inches(0.2), Inches(0.4), Inches(0.3),
         f"0{i+1}", size=11, font=MONO, color=fg, bold=True, space_after=0)
    text(s, M + Inches(0.95), yy + Inches(0.17), Inches(10.7), Inches(0.35),
         ttl, size=16.5, font=SANS, color=INK, bold=True, space_after=0)
    text(s, M + Inches(0.95), yy + Inches(0.56), Inches(10.9), Inches(0.8),
         body, size=12.5, color=INK_SOFT, spacing=1.24, space_after=0)
    yy += Inches(1.62)
done(s)

# ================================================================ 12 hetero
s = slide("This came up as a question and it is worth having an answer ready. "
          "Yes -- by construction. The only database value that crosses the "
          "consensus boundary is storage_hash in the proposal header, and that "
          "is the summed value. A nice consequence: the routing function does "
          "not have to match across validators either, because placement "
          "changes which shard hashes an item, never the sum. But be honest -- "
          "every cluster run has been homogeneous, so this is verified in unit "
          "tests only.")
y = header(s, "Question", "Can validators run different shard counts?",
           "Yes, by construction — and it is worth knowing exactly why, "
           "because the claim is currently proven by design and unit tests, "
           "not by a heterogeneous run.")
lw = Inches(6.5)
text(s, M, y, lw, Inches(0.3), "Why it holds", size=11, font=MONO,
     color=ACCENT, caps=True, letter=1.4, space_after=0)
bullets(s, M, y + Inches(0.42), lw, [
    ("One agreement point.", "The only database value crossing the consensus "
     "boundary is storage_hash in the proposal header. A validator computes "
     "its own and rejects the proposal on mismatch. No consensus type mentions "
     "shards at all."),
    ("The hash is shard-count independent.", "LtHash16 is a homomorphic "
     "multiset hash; combine_block_results sums the partials, and the sum is "
     "exactly what one unsharded db would produce."),
    ("Every item is hashed exactly once.", "Genesis writes owner-only, with a "
     "throwaway full pass for the global header hash. Code sits on shard 0 at "
     "any N. Hog ages and globals are not hashed at all."),
    ("Routing need not match either.", "Placement changes which shard hashes "
     "an item, never the sum. kShardFunctionId fail-stop only enforces "
     "agreement inside one validator."),
], size=13.5, gap=0.2)
rx2 = M + Inches(6.95)
text(s, rx2, y, Inches(4.9), Inches(0.3), "What would bite today", size=11,
     font=MONO, color=AMBER, caps=True, letter=1.4, space_after=0)
bullets(s, rx2, y + Inches(0.42), Inches(4.85), [
    ("Issue #7 blocks it outright", "on assertion-enabled builds: the "
     "per-shard conservation invariant kills any N>1 validator at genesis."),
    ("Viewing snapshots open on the primary only,", "so a multi-shard "
     "validator's query surface is incomplete — local, not consensus."),
    ("Nomination slows with shard count,", "so a heavily-sharded validator is "
     "a slower proposer. Operational, not correctness."),
], size=13.5, gap=0.2, dotcolor=AMBER)
yy = y + Inches(3.05)
box(s, rx2, yy, Inches(4.85), Inches(1.3), fill=ACC_SOFT)
text(s, rx2 + Inches(0.26), yy + Inches(0.18), Inches(4.3), Inches(0.25),
     "Cheapest way to prove it", size=9.5, font=MONO, color=ACCENT, caps=True,
     letter=1.3, space_after=0)
text(s, rx2 + Inches(0.26), yy + Inches(0.5), Inches(4.4), Inches(0.7),
     "Bring up hog-devnode nodes at 1, 2 and 4 shards against the same "
     "genesis and diff the headers block by block. Far smaller than another "
     "cluster campaign.", size=12.5, color=INK, spacing=1.22, space_after=0)
done(s)

# ================================================================ 13 remaining
s = slide("Keep this up for questions -- it is the agenda for what comes next. "
          "If asked to rank: protocol reconciliation is the merge blocker, the "
          "keyed hash gets harder the more data exists on disk, and viewing "
          "snapshots is the largest remaining piece of design work. An N=4 "
          "correctness run is the cheapest high-value item.")
y = header(s, "Remaining", "What is not done")
left = [("Protocol reconciliation", "MERGE BLOCKER", CRIT, CRIT_SOFT,
         "The ShardCard identity fields sit on an interim protocol branch. "
         "They must land on protocol main and propagate through groundhog-go "
         "before this PR can merge."),
        ("Viewing snapshots", "LARGEST DESIGN GAP", AMBER, AMB_SOFT,
         "Snapshots open on the primary shard only, while snapshot-scoped "
         "reads route by address to the owner. Latent today because the read "
         "path is unexercised."),
        ("A keyed routing hash", "GETS HARDER WITH TIME", AMBER, AMB_SOFT,
         "FNV-1a is unkeyed and therefore grindable. Replacing it needs a key "
         "that survives restarts — and it gets harder the more data exists on "
         "disk.")]
right = [("Commit atomicity", "", FAINT, None,
          "Commit still fans out non-atomically. Partial commit is detected at "
          "startup, not prevented."),
         ("The nomination path", "", FAINT, None,
          "Per-address hog-age reads are why throughput does not scale. Named "
          "target, not yet worked."),
         ("Scale", "", FAINT, None,
          "Unit-tested at N = 2, 4, 8 and benchmarked at 4; every long clean "
          "correctness run is at N = 2."),
         ("Heterogeneous shard counts", "", FAINT, None,
          "Shard-count independence is proven by construction, never observed "
          "across differently-sharded nodes.")]
cw2 = (CW - Inches(0.3)) / 2
yy = y + Inches(0.05)
for ttl, tag, fg, bg, body in left:
    box(s, M, yy, Emu(int(cw2)), Inches(1.44), fill=WHITE, line=RULE)
    box(s, M, yy, Pt(3.5), Inches(1.44), fill=fg)
    text(s, M + Inches(0.3), yy + Inches(0.18), Emu(int(cw2)) - Inches(0.5),
         Inches(0.3), ttl, size=16.5, font=SANS, color=INK, bold=True,
         space_after=0)
    if tag:
        chip(s, M + Inches(0.3), yy + Inches(0.52), tag, fg, bg,
             w=Inches(0.14 * len(tag) + 0.2))
    text(s, M + Inches(0.3), yy + Inches(0.86), Emu(int(cw2)) - Inches(0.55),
         Inches(0.5), body, size=12, color=INK_SOFT, spacing=1.2, space_after=0)
    yy += Inches(1.6)
yy = y + Inches(0.05)
rx2 = M + Emu(int(cw2)) + Inches(0.3)
for ttl, tag, fg, bg, body in right:
    box(s, rx2, yy, Emu(int(cw2)), Inches(1.12), fill=WHITE, line=RULE)
    text(s, rx2 + Inches(0.28), yy + Inches(0.17), Emu(int(cw2)) - Inches(0.5),
         Inches(0.3), ttl, size=15, font=SANS, color=INK, bold=True,
         space_after=0)
    text(s, rx2 + Inches(0.28), yy + Inches(0.5), Emu(int(cw2)) - Inches(0.55),
         Inches(0.5), body, size=12, color=INK_SOFT, spacing=1.2, space_after=0)
    yy += Inches(1.2)
done(s)

# ================================================================ 14 summary
s = slide("If you only get one sentence: the shard count is invisible to "
          "consensus, and every failure mode that used to be true-by-accident "
          "at N=1 now has an explicit mechanism enforcing it.")
y = header(s, "In summary", "If you remember five things")
takeaways = [
    ("The database is now N processes, and the block header does not know it.",
     "LtHash16 additivity makes shard count a local deployment decision rather "
     "than a protocol change."),
    ("One routing rule, one implementation,",
     "shared by the mempool, executor and auth paths. Identity is persisted, "
     "declared, and fail-stop at every seam."),
    ("Fan-out has a discipline — launch all, drain before rethrowing, visit "
     "every shard.", "Each of those three rules is a bug that actually happened."),
    ("Validated on a 2-shard cluster:",
     "79.5M transactions, 343 non-empty blocks, shards balanced 49 / 50, zero "
     "cores and zero invariant failures. Then 175 runs at 1, 2 and 4 shards to "
     "find out what it buys."),
    ("What it buys is capacity, not speed.",
     "Every sharded capacity run finished where the unsharded ones stalled, "
     "while throughput stayed flat because nomination got slower as commit got "
     "faster. Along the way it surfaced ten issues, most sitting on main, "
     "invisible."),
]
yy = y + Inches(0.05)
TSIZE, TSPACE, TGAP = 13.5, 1.26, 0.12
tw = CW - Inches(1.4)
per_line = max(1, int(Emu(tw).inches / (TSIZE * 0.0072)))
for i, (lead, rest) in enumerate(takeaways):
    chars = len(lead) + len(rest) + 2
    lines = max(1, -(-chars // per_line))
    h = Inches(lines * TSIZE * TSPACE / 72.0 + 0.32)
    box(s, M, yy, CW, h, fill=WHITE, line=RULE)
    text(s, M + Inches(0.28), yy + Inches(0.17), Inches(0.5), Inches(0.3),
         f"{i+1:02d}", size=12.5, font=MONO, color=ACCENT, bold=True,
         space_after=0)
    text(s, M + Inches(0.9), yy + Inches(0.16), tw, h - Inches(0.28),
         [[(lead, {"font": SANS, "bold": True, "size": TSIZE, "color": INK}),
           ("  " + rest, {"size": TSIZE, "color": INK_SOFT})]],
         spacing=TSPACE, space_after=0)
    yy += h + Inches(TGAP)
done(s)

# ================================================================ 15 pointers
s = slide("Pointers for anyone who wants the detail. The talk file has the "
          "full 20-slide walkthrough with speaker notes; ISSUES.md has all ten "
          "issues with repro steps and artifact paths; HANDOFF.md documents the "
          "benchmark harness so the campaign is rerunnable.")
box(s, 0, 0, W, H, fill=INK)
box(s, 0, 0, W, Inches(0.14), fill=ACCENT)
text(s, M, Inches(1.4), Inches(6), Inches(0.3), "Where the detail lives",
     size=11, font=MONO, color=RGBColor(0x57, 0xC4, 0xB2), caps=True,
     letter=1.7, space_after=0)
text(s, M, Inches(1.88), Inches(10), Inches(0.8), "Read further",
     size=40, font=SANS, color=WHITE, bold=True, space_after=0)
docs = [("SHARDING-TALK.md", "20-slide technical walkthrough with speaker "
         "notes — the deep version of this deck"),
        ("ISSUES.md", "all ten issues: status, mechanism, repro, artifact "
         "paths and core-dump locations"),
        ("shard-bench/HANDOFF.md", "the benchmark harness and campaign — how "
         "to rerun any of the 175 configurations"),
        ("PR #3108", "44 commits across 47 files; six review rounds; draft "
         "pending protocol reconciliation")]
yy = Inches(3.05)
for name, desc in docs:
    box(s, M, yy + Inches(0.13), Pt(4.5), Pt(4.5),
        fill=RGBColor(0x57, 0xC4, 0xB2), shape=MSO_SHAPE.OVAL)
    text(s, M + Inches(0.3), yy, Inches(11.2), Inches(0.35),
         [[(name, {"font": MONO, "bold": True, "size": 15,
                   "color": RGBColor(0xE4, 0xEA, 0xE7)}),
           ("   " + desc, {"size": 14, "color": RGBColor(0xA5, 0xB0, 0xAC)})]],
         spacing=1.2, space_after=0)
    yy += Inches(0.7)
text(s, M, H - Inches(1.0), Inches(9), Inches(0.3),
     "Vinita Pawar  ·  vp/sharding-v2-db-abstraction  ·  29 Jun – 18 Aug 2026",
     size=11, font=MONO, color=RGBColor(0x77, 0x83, 0x7F), space_after=0)
N[0] += 1

prs.core_properties.title = "hogd Database Sharding"
prs.core_properties.author = "Vinita Pawar"
prs.core_properties.subject = "Engineering overview, 29 Jun - 18 Aug 2026"
prs.core_properties.comments = ("Overview of the hogd database sharding work: "
                                "design, validation, benchmarks and remaining gaps.")
out = "/home/hog12/vinita/hogd/SHARDING-OVERVIEW.pptx"
prs.save(out)
print("wrote", out, len(prs.slides.__iter__.__self__._sldIdLst), "slides")

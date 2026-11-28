"""Build the fstx docs site (GitHub Pages, https://harshchourasia1.github.io/fstx/) from the repo's own Markdown.

    python -m pip install markdown
    python .github/site/build_site.py <out dir>

Pages: index (README: intro, install, quick start, speed, file format, credits, citing), python (python/README.md),
r (the README's "R: ..." sections), format (inst/spec/fstx-format-1.2.md), changes (NEWS.md). The output folder is
what the gh-pages branch holds; nothing here is part of the R or Python package.
"""
import html
import re
import shutil
import sys
from datetime import date
from pathlib import Path

import markdown

REPO = Path(__file__).resolve().parents[2]
SITE = "https://harshchourasia1.github.io/fstx/"
GH = "https://github.com/HarshChourasia1/fstx"
VERSION = re.search(r"^Version:\s*(\S+)", (REPO / "DESCRIPTION").read_text(encoding="utf-8"), re.M).group(1)

NAV = [("index.html", "Home"), ("python.html", "Python"), ("r.html", "R"), ("format.html", "File format"),
       ("changes.html", "Changes")]

# links in the Markdown that point at files the site has its own page for
LINKS = {
    f"{GH}/blob/main/python/README.md#speed": "python.html#speed",
    f"{GH}/blob/main/python/README.md": "python.html",
    f"{GH}/blob/main/inst/spec/fstx-format-1.2.md": "format.html",
    "https://raw.githubusercontent.com/HarshChourasia1/fstx/main/.github/readme/speed.svg": "speed.svg",
    "https://harshchourasia1.github.io/fstx/speed.svg": "speed.svg",
    "(LICENSE)": f"({GH}/blob/main/LICENSE)",
    "(inst/COPYRIGHTS)": f"({GH}/blob/main/inst/COPYRIGHTS)",
}

CSS = """
:root { --bg:#fcfcfb; --fg:#1b1b19; --muted:#5b5a55; --line:#e4e3de; --code:#f1f0ec; --link:#1f63b8; --accent:#2a78d6; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#161615; --fg:#ecebe7; --muted:#b3b2aa; --line:#33332f; --code:#22221f; --link:#7fb0f0; --accent:#3987e5; }
}
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body { margin:0; background:var(--bg); color:var(--fg); font:16px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif; }
header { border-bottom:1px solid var(--line); }
.bar { max-width:900px; margin:0 auto; padding:14px 16px; display:flex; flex-wrap:wrap; gap:6px 20px; align-items:baseline; }
.brand { font-weight:700; font-size:19px; color:var(--fg); text-decoration:none; margin-right:auto; }
.brand span { color:var(--muted); font-weight:400; font-size:14px; margin-left:6px; }
nav a { color:var(--muted); text-decoration:none; font-size:15px; margin-left:14px; }
nav a:first-child { margin-left:0; }
nav a[aria-current] { color:var(--fg); font-weight:600; }
main { max-width:900px; margin:0 auto; padding:8px 16px 48px; }
h1 { font-size:30px; line-height:1.25; margin:28px 0 12px; }
h2 { font-size:22px; margin:36px 0 10px; padding-top:8px; border-top:1px solid var(--line); }
h3 { font-size:18px; margin:26px 0 8px; }
a { color:var(--link); }
p, li { max-width:75ch; }
code { overflow-wrap:anywhere; font:14px/1.5 ui-monospace,SFMono-Regular,Consolas,Menlo,monospace; background:var(--code); padding:1px 4px; border-radius:4px; }
pre { background:var(--code); padding:12px 14px; border-radius:8px; overflow-x:auto; }
pre code { background:none; padding:0; overflow-wrap:normal; }
table { border-collapse:collapse; display:block; overflow-x:auto; font-size:14px; margin:12px 0; }
th, td { border-bottom:1px solid var(--line); padding:5px 10px; text-align:left; vertical-align:top; }
th { font-weight:600; }
img { max-width:100%; height:auto; }
footer { max-width:900px; margin:0 auto; padding:20px 16px 40px; color:var(--muted); font-size:13px; border-top:1px solid var(--line); }
footer a { color:var(--muted); }
"""


def md(text: str) -> str:
    for a, b in LINKS.items():
        text = text.replace(a, b)
    return markdown.markdown(text, extensions=["tables", "fenced_code", "toc", "sane_lists"],
                             extension_configs={"toc": {"permalink": False}})


def sections(text: str) -> list[tuple[str, str]]:
    """Split a README into (heading, block) on '## ' lines outside code fences; the part before the first is ''."""
    out, head, buf, fence = [], "", [], False
    for line in text.splitlines(keepends=True):
        if line.startswith("```"):
            fence = not fence
        if not fence and line.startswith("## "):
            out.append((head, "".join(buf)))
            head, buf = line[3:].strip(), [line]
        else:
            buf.append(line)
    out.append((head, "".join(buf)))
    return out


def page(name: str, title: str, desc: str, body: str) -> str:
    nav = "".join(f'<a href="{href}"{" aria-current=page" if href == name else ""}>{label}</a>' for href, label in NAV)
    url = SITE + ("" if name == "index.html" else name)
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<meta name="description" content="{html.escape(desc)}">
<link rel="canonical" href="{url}">
<meta property="og:title" content="{html.escape(title)}">
<meta property="og:description" content="{html.escape(desc)}">
<meta property="og:url" content="{url}">
<meta property="og:type" content="website">
<meta property="og:image" content="{SITE}speed.png">
<meta name="color-scheme" content="light dark">
<style>{CSS}</style>
</head>
<body>
<header><div class="bar"><a class="brand" href="index.html">fstx<span>{VERSION}</span></a><nav>{nav}</nav></div></header>
<main>
{body}
</main>
<footer>fstx {VERSION} · MIT licence · <a href="{GH}">source on GitHub</a> · <a href="https://pypi.org/project/fstx/">PyPI</a> ·
<a href="https://harshchourasia1.r-universe.dev/fstx">R-universe</a> · built {date.today():%d %b %Y} from the repository's Markdown</footer>
</body>
</html>
"""


def main(out: Path) -> None:
    if out.exists():
        for f in out.iterdir():
            if f.name != ".git":
                shutil.rmtree(f) if f.is_dir() else f.unlink()
    out.mkdir(parents=True, exist_ok=True)

    readme = sections((REPO / "README.md").read_text(encoding="utf-8"))
    home = [b for h, b in readme if not h.startswith("R: ")]
    rpart = [b for h, b in readme if h.startswith("R: ")]
    home_md = "".join(home).replace("The R guide\nfollows.", "The R guide is [its own page](r.html).")
    # the badge lines are for GitHub; the site has its own header
    home_md = re.sub(r"^\[!\[[^\n]*\n", "", home_md, flags=re.M)
    home_md = re.sub(r"^Docs: [^\n]*\n\n", "", home_md, flags=re.M)

    pages = {
        "index.html": ("fstx: fast, safe column-store files for Python and R",
                       "fstx is a column-store file format with Python and R packages on one C++ engine: "
                       "faster writes and reads than Parquet, filters that skip pages, a checksum on every page.",
                       md(home_md)),
        "python.html": ("fstx for Python: read and write fstx files with pyarrow, pandas and polars",
                        "Install with pip install fstx. Read and write pyarrow Tables, pandas and polars DataFrames; "
                        "filters, column and row selection, type mapping, speed against Parquet, Vortex, Lance and DuckDB.",
                        md((REPO / "python" / "README.md").read_text(encoding="utf-8"))),
        "r.html": ("fstx for R: fast data frame files",
                   "write_fstx and read_fstx for R data frames: filters with where, lazy and ALTREP strings, factors, "
                   "int64 and binary columns, appends and compaction.",
                   md("# fstx for R\n\nInstall and quick start are on the [home page](index.html#install).\n\n"
                      + "".join(rpart).replace("## R: ", "## "))),
        "format.html": ("fstx file format 1.2 specification",
                        "The fstx file format 1.2: file layout, header, footer, pages, encodings (ALP, FSST, "
                        "dictionary, delta and more), feature bits and rules for readers and writers.",
                        md((REPO / "inst" / "spec" / "fstx-format-1.2.md").read_text(encoding="utf-8"))),
        "changes.html": ("fstx changes", "What changed in each fstx release.",
                         md((REPO / "NEWS.md").read_text(encoding="utf-8").replace("\n# ", "\n## ").replace("# fstx", "# Changes\n\n## fstx", 1))),
    }
    for name, (title, desc, body) in pages.items():
        (out / name).write_text(page(name, title, desc, body), encoding="utf-8", newline="\n")

    shutil.copyfile(REPO / ".github" / "readme" / "speed.svg", out / "speed.svg")
    png = REPO / ".github" / "readme" / "speed.png"
    if png.exists():
        shutil.copyfile(png, out / "speed.png")
    (out / ".nojekyll").write_text("", encoding="utf-8")
    # Google Search Console ownership file (google<code>.html), if one has been added next to this script
    for f in Path(__file__).parent.glob("google*.html"):
        shutil.copyfile(f, out / f.name)
    today = date.today().isoformat()
    urls = "".join(f"  <url><loc>{SITE}{'' if n == 'index.html' else n}</loc><lastmod>{today}</lastmod></url>\n" for n in pages)
    (out / "sitemap.xml").write_text('<?xml version="1.0" encoding="UTF-8"?>\n'
                                     '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
                                     f"{urls}</urlset>\n", encoding="utf-8", newline="\n")
    print(f"site {VERSION}: {len(pages)} pages -> {out}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: build_site.py <out dir>")
    main(Path(sys.argv[1]).resolve())

"""Draw .github/readme/speed.svg, the speed chart at the top of README.md, from the README's speed table.

    python .github/readme/speed_chart.py

The numbers below are the README "Speed" table (Python, all cores, 27 Nov 2026 official run), in seconds.
Change them only together with that table. Each row's bars are scaled to the slowest of the three in that row.
"""
from decimal import Decimal
from pathlib import Path

FORMATS = ["fstx", "Vortex", "Parquet (snappy)"]
TESTS = ["write", "full read", "one column", "1% row window", "filter"]
DATA = {  # table -> test -> (fstx, Vortex, Parquet snappy), seconds
    "numeric (10M rows x 6)": [("0.0738", "0.233", "0.914"), ("0.0227", "0.0400", "0.0802"), ("0.0117", "0.0207", "0.0441"),
                               ("0.00124", "0.00276", "0.0138"), ("0.0209", "0.0490", "0.0803")],
    "text (2M rows x 4 strings)": [("0.104", "0.330", "0.557"), ("0.0394", "0.0438", "0.0882"), ("0.0190", "0.0189", "0.0874"),
                                   ("0.00152", "0.00279", "0.0891"), ("0.0115", "0.0338", "0.0829")],
    "mixed (5M rows x 8)": [("0.0954", "0.346", "0.869"), ("0.0322", "0.0422", "0.133"), ("0.00613", "0.00528", "0.0349"),
                            ("0.00387", "0.00319", "0.0430"), ("0.0173", "0.0306", "0.121")],
}

W, LEFT, RIGHT = 760, 150, 70          # label column, room for the value after the longest bar
BAR, GAP, ROWPAD, PANELHEAD = 7, 2, 12, 30
FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif"


def ms(s: str) -> str:
    """Seconds as written in the README table -> milliseconds with the same significant digits (0.0400 -> 40.0)."""
    return str(Decimal(s).scaleb(3))


def main() -> None:
    out = []
    y = 0
    head = [
        f'<text x="0" y="22" class="t1" font-size="17" font-weight="600">fstx vs Vortex vs Parquet: time in milliseconds, shorter is faster</text>',
        f'<text x="0" y="42" class="t2" font-size="12">Python, all cores, one 14-core desktop, 27 Nov 2026. Each row is scaled to its slowest format. '
        f'Method and more formats: README, Speed.</text>',
    ]
    # legend
    lx = 0
    for i, name in enumerate(FORMATS):
        head.append(f'<rect x="{lx}" y="56" width="12" height="12" rx="3" class="s{i}"/>')
        head.append(f'<text x="{lx + 18}" y="66.5" class="t1" font-size="12">{name}</text>')
        lx += 18 + 8 * len(name) + 22
    y = 88
    bar_w = W - LEFT - RIGHT
    for table, rows in DATA.items():
        out.append(f'<text x="0" y="{y + 16}" class="t1" font-size="13" font-weight="600">{table}</text>')
        out.append(f'<line x1="0" x2="{W}" y1="{y + 23}" y2="{y + 23}" class="rule"/>')
        y += PANELHEAD
        for test, vals in zip(TESTS, rows):
            h = 3 * BAR + 2 * GAP
            out.append(f'<text x="0" y="{y + h / 2 + 4}" class="t2" font-size="12">{test}</text>')
            nums = [float(v) for v in vals]
            top = max(nums)
            best = min(nums)
            for i, (v, x) in enumerate(zip(vals, nums)):
                by = y + i * (BAR + GAP)
                w = max(2.0, bar_w * x / top)
                out.append(f'<rect x="{LEFT}" y="{by}" width="{w:.1f}" height="{BAR}" rx="2" class="s{i}">'
                           f'<title>{table.split(" ")[0]}, {test}, {FORMATS[i]}: {ms(v)} ms</title></rect>')
                weight = ' font-weight="600"' if x == best else ""
                out.append(f'<text x="{LEFT + w + 5:.1f}" y="{by + BAR - 0.5}" class="t2" font-size="9.5"{weight}>{ms(v)}</text>')
            y += h + ROWPAD
        y += 6
    H = y + 4
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W + 32}" height="{H + 32}" viewBox="-16 -16 {W + 32} {H + 32}" font-family="{FONT}" role="img" aria-label="Bar chart: fstx, Vortex and Parquet (snappy) times in milliseconds for write, full read, one column, 1% row window and filter on three tables; the same numbers are in the README speed table.">
<style>
  .bg {{ fill: #fcfcfb; }} .t1 {{ fill: #0b0b0b; }} .t2 {{ fill: #52514e; }} .rule {{ stroke: #dcdbd6; stroke-width: 1; }}
  .s0 {{ fill: #2a78d6; }} .s1 {{ fill: #eb6834; }} .s2 {{ fill: #1baf7a; }}
  @media (prefers-color-scheme: dark) {{
    .bg {{ fill: #1a1a19; }} .t1 {{ fill: #f0efec; }} .t2 {{ fill: #c3c2b7; }} .rule {{ stroke: #3a3a37; }}
    .s0 {{ fill: #3987e5; }} .s1 {{ fill: #d95926; }} .s2 {{ fill: #199e70; }}
  }}
</style>
<rect x="-16" y="-16" width="{W + 32}" height="{H + 32}" rx="8" class="bg"/>
{chr(10).join(head)}
{chr(10).join(out)}
</svg>
'''
    Path(__file__).with_name("speed.svg").write_text(svg, encoding="utf-8", newline="\n")
    print(f"speed.svg {W + 32} x {H + 32}")


if __name__ == "__main__":
    main()

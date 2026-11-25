#!/usr/bin/env python3
"""bench.py 의 CSV 를 발표용 SVG 꺾은선 그래프로 그린다 (외부 라이브러리 없음).

    python3 bench/plot.py bench/results/macos-idle.csv    bench/results/macos-idle.svg
    python3 bench/plot.py bench/results/macos-workers.csv bench/results/macos-workers.svg

idle CSV 는 x = 노는 연결 수, workers CSV 는 x = 이벤트 루프 수. y 는 초당 처리 요청 수.
failover.py 의 CSV(phase 열이 있음)는 단계별 실패 비율 막대그래프로 그린다.
"""
import csv
import sys
from html import escape

# 색과 모양은 서버 종류에 고정한다 → 어느 그래프에서든 같은 서버는 같은 색(검증된 팔레트 순서).
# 색만으로 구분하지 않도록 모양도 다르게 하고, 선 끝에 이름을 직접 붙인다.
STYLE = {"before": ("#2a78d6", "circle"), "poll": ("#eb6834", "square"),
         "kqueue": ("#1baf7a", "diamond"), "epoll": ("#eda100", "triangle")}
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
LABELS = {"before": "기존 develop", "poll": "새 루프 + poll",
          "kqueue": "새 루프 + kqueue", "epoll": "새 루프 + epoll"}


def nice_max(v):
    """눈금 4칸이 깔끔하게 나뉘는 상한 (예: 73k → 80k)."""
    step = 10 ** (len(str(int(v))) - 1)
    for m in (1, 1.2, 1.6, 2, 2.4, 3, 4, 5, 6, 8, 10):
        if v <= m * step:
            return m * step
    return 10 * step


def marker(shape, x, y, color):
    r = 4.5
    ring = f'stroke="{SURFACE}" stroke-width="2"'
    if shape == "circle":
        return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{color}" {ring}/>'
    if shape == "square":
        return f'<rect x="{x - r:.1f}" y="{y - r:.1f}" width="{2 * r}" height="{2 * r}" rx="1.5" fill="{color}" {ring}/>'
    if shape == "diamond":
        d = r * 1.3
        return f'<path d="M{x:.1f},{y - d:.1f} L{x + d:.1f},{y:.1f} L{x:.1f},{y + d:.1f} L{x - d:.1f},{y:.1f}Z" fill="{color}" {ring}/>'
    d = r * 1.3
    return f'<path d="M{x:.1f},{y - d:.1f} L{x + d:.1f},{y + d * 0.8:.1f} L{x - d:.1f},{y + d * 0.8:.1f}Z" fill="{color}" {ring}/>'


PHASES = [("normal", "3대 정상"), ("killed", "1대 종료"), ("recovered", "다시 켠 뒤"), ("stopped", "1대 멈춤")]
FAILOVER = {"before": ("#2a78d6", "PR #30 이전"), "after": ("#eb6834", "PR #30 (연결 실패만 우회)"),
            "retry": ("#1baf7a", "현재 (응답 시간 초과도 우회)")}


def plot_failover(rows, dst):
    """단계마다 두 서버의 실패 비율(200 이 아닌 응답) 막대. 막대 위에 값을 적는다."""
    series = []
    for r in rows:
        if r["series"] not in series:
            series.append(r["series"])
    val = {(r["series"], r["phase"]): float(r["fail_pct"]) for r in rows}
    W, H = 860, 460
    left, right, top, bottom = 84, 40, 120, 64
    pw, ph = W - left - right, H - top - bottom
    ymax = 40.0

    def py(v):
        return top + ph * (1 - v / ymax)

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family="-apple-system, \'Apple SD Gothic Neo\', \'Noto Sans KR\', sans-serif">',
         f'<rect width="{W}" height="{H}" fill="{SURFACE}"/>',
         f'<text x="{left}" y="38" font-size="20" font-weight="600" fill="{INK}">백엔드 1대가 고장 나면 요청이 실패하는가</text>',
         f'<text x="{left}" y="62" font-size="13" fill="{INK2}">리버스 프록시 뒤 백엔드 3대 · 단계마다 요청 300개 · '
         f'실패 = 200이 아닌 응답(502·504)</text>']
    lx = left
    for s in series:  # 범례
        color, label = FAILOVER.get(s, ("#52514e", s))
        o.append(f'<rect x="{lx}" y="84" width="12" height="12" rx="2" fill="{color}"/>')
        o.append(f'<text x="{lx + 18}" y="95" font-size="13" fill="{INK}">{escape(label)}</text>')
        lx += 36 + 12 * len(label)
    for i in range(5):
        v = ymax * i / 4
        y = py(v)
        o.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + pw}" y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
        o.append(f'<text x="{left - 10}" y="{y + 4:.1f}" font-size="12" fill="{INK2}" text-anchor="end">{v:g}%</text>')
    o.append(f'<text x="22" y="{top + ph / 2:.1f}" font-size="12" fill="{INK2}" text-anchor="middle" '
             f'transform="rotate(-90 22 {top + ph / 2:.1f})">실패한 요청 비율</text>')
    group = pw / len(PHASES)
    bw, gap = 24, 2
    for gi, (phase, label) in enumerate(PHASES):
        cx = left + group * (gi + 0.5)
        o.append(f'<text x="{cx:.1f}" y="{top + ph + 24}" font-size="13" fill="{INK2}" text-anchor="middle">{escape(label)}</text>')
        total = len(series) * bw + (len(series) - 1) * gap
        for si, s in enumerate(series):
            v = val.get((s, phase))
            if v is None:
                continue
            color = FAILOVER.get(s, ("#52514e", s))[0]
            x = cx - total / 2 + si * (bw + gap)
            y = py(v)
            h = top + ph - y
            if h > 0:  # 위쪽만 4px 둥글게, 바닥은 반듯하게
                r = min(4, h)
                o.append(f'<path d="M{x:.1f},{top + ph:.1f} V{y + r:.1f} Q{x:.1f},{y:.1f} {x + r:.1f},{y:.1f} '
                         f'H{x + bw - r:.1f} Q{x + bw:.1f},{y:.1f} {x + bw:.1f},{y + r:.1f} V{top + ph:.1f} Z" fill="{color}"/>')
            o.append(f'<text x="{x + bw / 2:.1f}" y="{y - 6:.1f}" font-size="12" fill="{INK}" '
                     f'text-anchor="middle">{v:g}%</text>')
    o.append(f'<line x1="{left}" y1="{top + ph}" x2="{left + pw}" y2="{top + ph}" stroke="#c3c2b7" stroke-width="1"/>')
    o.append("</svg>")
    open(dst, "w").write("\n".join(o))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    rows = list(csv.DictReader(open(src)))
    if "phase" in rows[0]:
        return plot_failover(rows, dst)
    kind = "workers" if len({r["workers"] for r in rows}) > 1 else "idle"
    xkey = "workers" if kind == "workers" else "idle"

    series = []
    for r in rows:
        if r["series"] not in series:
            series.append(r["series"])
    xs = sorted({int(r[xkey]) for r in rows})
    data = {s: {int(r[xkey]): int(r["rps"]) for r in rows if r["series"] == s} for s in series}
    ymax = nice_max(max(max(d.values()) for d in data.values()) * 1.08)

    W, H = 860, 500
    left, right, top, bottom = 84, 250, 96, 70
    pw, ph = W - left - right, H - top - bottom

    def px(x):
        if kind == "workers":  # 루프 수는 같은 간격으로
            return left + pw * xs.index(x) / (len(xs) - 1)
        return left + pw * x / xs[-1]

    def py(y):
        return top + ph * (1 - y / ymax)

    if kind == "idle":
        title = "노는 연결이 늘어도 처리량이 유지되는가"
        sub = "루프 1개, 활성 연결 50개로 측정 · 다른 연결 수천 개가 keep-alive로 대기 중"
        xlabel = "대기 중인(노는) 연결 수"
    else:
        title = "이벤트 루프를 늘리면 처리량이 느는가"
        sub = "노는 연결 없음 · ab 4개 × 동시 연결 50개 · 서버와 부하 도구가 같은 컴퓨터"
        xlabel = "이벤트 루프 스레드 수 (workers)"

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family="-apple-system, \'Apple SD Gothic Neo\', \'Noto Sans KR\', sans-serif">',
         f'<rect width="{W}" height="{H}" fill="{SURFACE}"/>',
         f'<text x="{left}" y="38" font-size="20" font-weight="600" fill="{INK}">{escape(title)}</text>',
         f'<text x="{left}" y="62" font-size="13" fill="{INK2}">{escape(sub)}</text>']

    for i in range(5):  # 가로 눈금선
        v = ymax * i / 4
        y = py(v)
        o.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + pw}" y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
        label = f'{v / 1000:g}k' if v else '0'
        o.append(f'<text x="{left - 10}" y="{y + 4:.1f}" font-size="12" fill="{INK2}" '
                 f'text-anchor="end">{label}</text>')
    o.append(f'<text x="22" y="{top + ph / 2:.1f}" font-size="12" fill="{INK2}" text-anchor="middle" '
             f'transform="rotate(-90 22 {top + ph / 2:.1f})">초당 처리 요청 수 (req/s)</text>')
    for x in xs:
        o.append(f'<text x="{px(x):.1f}" y="{top + ph + 22}" font-size="12" fill="{INK2}" '
                 f'text-anchor="middle">{x:,}</text>')
    o.append(f'<text x="{left + pw / 2:.1f}" y="{H - 18}" font-size="12" fill="{INK2}" '
             f'text-anchor="middle">{escape(xlabel)}</text>')

    # 선 → 점 → 오른쪽 끝 이름표 (이름표가 겹치지 않게 세로로 밀어 냄)
    ends = []
    for i, s in enumerate(series):
        color, shape = STYLE.get(s, ("#52514e", "circle"))
        pts = [(px(x), py(data[s][x])) for x in xs if x in data[s]]
        o.append(f'<polyline points="{" ".join(f"{a:.1f},{b:.1f}" for a, b in pts)}" fill="none" '
                 f'stroke="{color}" stroke-width="2" stroke-linejoin="round"/>')
        for (a, b) in pts:
            o.append(marker(shape, a, b, color))
        last = xs[-1]
        ends.append([pts[-1][1], s, data[s][last]])
    ends.sort()
    for k in range(1, len(ends)):
        if ends[k][0] - ends[k - 1][0] < 34:
            ends[k][0] = ends[k - 1][0] + 34
    for y, s, v in ends:
        x0 = left + pw + 14
        color, shape = STYLE.get(s, ("#52514e", "circle"))
        o.append(marker(shape, x0, y - 4, color))
        o.append(f'<text x="{x0 + 12}" y="{y:.1f}" font-size="13" font-weight="600" fill="{INK}">'
                 f'{escape(LABELS.get(s, s))}</text>')
        o.append(f'<text x="{x0 + 12}" y="{y + 16:.1f}" font-size="12" fill="{INK2}">{v:,} req/s</text>')

    o.append("</svg>")
    open(dst, "w").write("\n".join(o))


if __name__ == "__main__":
    main()

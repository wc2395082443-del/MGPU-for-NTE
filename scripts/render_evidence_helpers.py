import html
import math

COLORS = {'direct': '#7296c0', 'compute': '#ac87e4', 'copy': '#43c8b0', 'color': '#43c8b0',
          'guides': '#2b98a9', 'interval': '#455367', 'neural': '#e883a3'}
LABELS = ['4080S main DIRECT queries', '4080S COMPUTE queries', '4080S outgoing COPY',
          '5080 incoming COPY / waits', '5080 NR']


def text(x, y, value, size=14, color='#dbe6f3', weight='400'):
    return f'<text x="{x:.3f}" y="{y:.3f}" font-size="{size}" fill="{color}" font-weight="{weight}">{html.escape(str(value))}</text>'


def wrap(width, height, items):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" role="img" width="{width}" height="{height}" viewBox="0 0 {width} {height}">'
            '<rect width="100%" height="100%" fill="#0c1624"/><g font-family="Segoe UI,Arial,sans-serif">'
            + ''.join(items) + '</g></svg>')


def maximum_time(data):
    return math.ceil(max(bar['end'] for run in data['runs'] for bar in run['bars']) / 20) * 20


def panel(run, heading, top, maximum, role):
    left, plot, height = 250, 1110, 204
    scale = plot / maximum
    first_row = top + 90
    items = [text(23, top + 25, heading, 23, weight='700'),
             text(23, top + 50, f"5 cycles: {run['source_end']:.3f} ms | mean: {run['means']['period']:.3f} ms | {run['capture']}", 14, '#a4b7cb')]
    for lane, label in enumerate(LABELS):
        y = first_row + lane * 33
        items.extend([text(15, y + 16, label, 12),
                      f'<rect x="{left}" y="{y - 3}" width="{plot}" height="27" fill="#172438" rx="3"/>'])
    for boundary in run['bins']:
        position = left + boundary['start'] * scale
        items.append(f'<path class="frame-guide" d="M{position:.3f},{first_row - 20} V{first_row + 158}" stroke="#536d8d" stroke-dasharray="4 4"/>')
        items.append(text(position + 4, first_row - 9, f"F{boundary['frame']} · {boundary['period']:.2f}", 11))
    finish = left + run['source_end'] * scale
    items.append(f'<path d="M{finish:.3f},{first_row - 20} V{first_row + 158}" stroke="#f5c77d" stroke-width="1.3"/>')
    items.append(text(finish + 5, first_row + 176, 'five-cycle end', 11, '#f5c77d'))
    for bar in run['bars']:
        if bar['end'] <= bar['start']:
            continue
        position = left + bar['start'] * scale
        width = (bar['end'] - bar['start']) * scale
        y = first_row + bar['lane'] * 33
        tip = f"{run['version']} F{bar['frame']} | {LABELS[bar['lane']]} | {bar['kind']} | {bar['start']:.6f}–{bar['end']:.6f} ms"
        items.append(f'<rect class="mark" data-run="{role}" data-lane="{bar["lane"]}" data-frame="{bar["frame"]}" '
                     f'data-start-ms="{bar["start"]:.6f}" data-end-ms="{bar["end"]:.6f}" x="{position:.6f}" y="{y}" width="{width:.6f}" height="20" '
                     f'opacity="{.55 if bar["frame"] == 0 else 1}" fill="{COLORS[bar["kind"]]}"><title>{html.escape(tip)}</title></rect>')
    for tick in range(0, maximum + 1, 20):
        items.append(text(left + tick * scale, first_row + height, f'{tick} ms', 11, '#9fb1c7'))
    return items


def comparison_svg(data):
    maximum = maximum_time(data)
    items = [text(24, 34, 'Before / after: five measured cycles on one fixed millisecond scale', 24, weight='700'),
             text(24, 62, 'M339c vs M340 + ForceTagging. Historical captures with COPY, tagging and resource-path changes.', 14, '#a4b7cb')]
    items += panel(data['runs'][0], 'BEFORE — M339c, before staged COPY handoff', 91, maximum, 'before')
    items += panel(data['runs'][1], 'AFTER — M340 + ForceTagging, historical measured improvement', 416, maximum, 'after')
    items.extend([text(24, 745, 'Cyan: whole/color COPY. Teal: guides. Purple: COMPUTE query. Pink: neural processing. Gray: between-stage interval.', 13),
                  text(24, 770, 'F0 is prior-frame work; the tail after F5 is shown. Main / COMPUTE queries include execution and dependencies.', 13, '#a4b7cb'),
                  text(24, 795, 'Both versions already overlap prior NR with source DIRECT. The new COPY receiving overlap is shown separately below.', 13, '#f5c77d')])
    return wrap(1400, 820, items)


def current_svg(data):
    items = panel(data['runs'][2], 'CURRENT RELEASE — M364 + M363, detailed capture ON (separate comparison scope)', 5, maximum_time(data), 'current')
    items.extend([text(24, 330, 'Detailed collection ON. Recording quality: main-stream loss 1; fine-job truncation 867.', 13, '#a4b7cb')])
    return wrap(1400, 355, items)


def copy_svg(data):
    items = [text(24, 33, 'One actual frame: receiving can start while guides are still being sent', 23, weight='700'),
             text(24, 60, 'First selected source frame from each capture. Both examples use producer COPY start = 0.', 14, '#a4b7cb')]
    left, scale = 250, 125
    for run_index, run in enumerate(data['runs'][:2]):
        top = 108 + run_index * 120
        items.append(text(24, top - 7, 'M339c — before' if run_index == 0 else 'M340 — historical after', 17, weight='600'))
        for lane in (2, 3):
            y = top + (lane - 2) * 35
            items.extend([text(40, y + 16, '4080S outgoing' if lane == 2 else '5080 incoming', 13),
                          f'<rect x="{left}" y="{y - 3}" width="1000" height="27" fill="#172438" rx="3"/>'])
        for bar in run['example']:
            begin = max(0, bar['start'])
            position = left + begin * scale
            width = (bar['end'] - begin) * scale
            y = top + (bar['lane'] - 2) * 35
            tip = f"{run['version']} {bar['kind']} {begin:.6f}–{bar['end']:.6f} ms"
            items.append(f'<rect class="copy-mark" x="{position:.6f}" y="{y}" width="{width:.6f}" height="20" fill="{COLORS[bar["kind"]]}"><title>{html.escape(tip)}</title></rect>')
            if width > 75:
                items.append(text(position + 6, y + 14, bar['kind'], 11, '#07121e'))
        for tick in range(9):
            items.append(text(left + tick * scale, top + 81, f'{tick} ms', 11, '#9fb1c7'))
    items.append(text(24, 335, 'Whole-window COPY intersection: 0.000 -> 1.216 ms. Whole transport elapsed: 7.073 -> 6.013 ms.', 15, '#49cfb3', '600'))
    return wrap(1400, 360, items)

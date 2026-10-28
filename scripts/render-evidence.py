import html
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / 'docs/nte/evidence'
FIGURES = ROOT / 'docs/nte/figures'


def text(x, y, content, size=15, color='#dbe6f3', weight='400'):
    return f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" font-weight="{weight}">{html.escape(str(content))}</text>'


def svg(width, height, elements):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
            f'role="img"><rect width="100%" height="100%" fill="#0c1624"/><g font-family="Segoe UI,Arial,sans-serif">'
            + ''.join(elements) + '</g></svg>')


def performance():
    measurements = json.loads((EVIDENCE / 'measurements.json').read_text(encoding='utf-8-sig'))
    items = [text(28, 39, 'Historical NTE measurements', 24, weight='700'),
             text(28, 68, '4080 SUPER game render / RTX 5080 neural rendering', 14, '#9fb1c7'),
             text(28, 109, 'Unique NR completions / second', 17, weight='600'),
             text(510, 109, 'Full transfer elapsed / ms', 17, weight='600')]
    colors = ['#647c95', '#49cfb3', '#60a5fa']
    for index, row in enumerate(measurements['Measurements']):
        y = 143 + index * 56
        items.extend([text(28, y + 18, row['Version'], 13),
                      f'<rect x="190" y="{y}" width="{row["NeuralHz"] * 7:.2f}" height="25" rx="4" fill="{colors[index]}"/>',
                      text(195 + row['NeuralHz'] * 7, y + 18, f'{row["NeuralHz"]:.3f}', 13),
                      f'<rect x="615" y="{y}" width="{row["TransferMs"] * 30:.2f}" height="25" rx="4" fill="{colors[index]}"/>',
                      text(620 + row['TransferMs'] * 30, y + 18, f'{row["TransferMs"]:.3f}', 13)])
    items.extend([text(28, 343, '+25.3% historical NR cadence; tagging/resource path also changed.', 16, '#49cfb3', '600'),
                  text(28, 370, 'Not a controlled A/B. M341 saved 0.510 ms transfer but did not raise overall FPS.', 14),
                  text(28, 399, 'Later runs still reported ~28 base FPS. Sixfold SDK counts are not physical display FPS.', 14, '#f5c77d')])
    (FIGURES / 'performance.svg').write_text(svg(960, 430, items), encoding='utf-8')


def timeline():
    data = json.loads((EVIDENCE / 'm340-five-frames.json').read_text(encoding='utf-8-sig'))
    labels = ['4080S main DIRECT queries', '4080S COMPUTE queries', 'Capture / normalization subset',
              '4080S outgoing COPY', '5080 incoming COPY / waits', '5080 neural rendering', 'Output snapshot (not display)']
    colors = {'direct': '#7296c0', 'compute': '#ac87e4', 'capture': '#e6ba6c', 'color': '#43c8b0',
              'guides': '#2b98a9', 'interval': '#455367', 'neural': '#e883a3', 'output': '#e8d589'}
    width, height, left, plot_width = 1200, 550, 252, 914
    maximum = max(bar['end'] for bar in data['bars'])
    scale = plot_width / maximum
    items = [text(25, 37, 'Five measured source-ready cycles — M340, 2026-10-21', 23, weight='700'),
             text(25, 65, 'One shared time axis. Bars retain actual overlaps; query coverage can include GPU waits.', 14, '#a4b7cb')]
    for index, label in enumerate(labels):
        y = 142 + index * 43
        items.extend([text(18, y + 17, label, 13),
                      f'<rect x="{left}" y="{y - 4}" width="{plot_width}" height="32" rx="3" fill="#172438"/>'])
    for boundary in data['bins']:
        position = left + boundary['start'] * scale
        items.append(f'<path d="M{position:.2f},118 V440" stroke="#50647e" stroke-dasharray="4 4"/>')
        items.append(text(position + 4, 108, f'F{boundary["frame"]} · {boundary["period"]:.2f} ms', 12))
    for bar in data['bars']:
        start = left + bar['start'] * scale
        y = 142 + bar['lane'] * 43
        duration = bar['end'] - bar['start']
        tip = f'F{bar["frame"]} / {labels[bar["lane"]]} / {bar["kind"]}: {bar["start"]:.4f}–{bar["end"]:.4f} ms ({duration:.4f} ms)'
        items.append(f'<rect class="mark" data-lane="{bar["lane"]}" x="{start:.3f}" y="{y}" width="{max(duration * scale, .3):.3f}" height="24" fill="{colors[bar["kind"]]}"><title>{html.escape(tip)}</title></rect>')
    for tick in range(0, int(maximum) + 1, 20):
        items.append(text(left + tick * scale, 466, f'{tick} ms', 12, '#a4b7cb'))
    items.extend([text(25, 502, 'F0 is previous-frame work. Capture is a subset. Source bins do not prove individual engine-frame ownership.', 13, '#f5c77d'),
                  text(25, 527, 'Historical 104-frame window: 33.451 NR completions/s. This figure is not the M364 release benchmark.', 13, '#a4b7cb')])
    chart = svg(width, height, items)
    (FIGURES / 'five-frames.svg').write_text(chart, encoding='utf-8')
    buttons = ''.join(f'<label><input type="checkbox" checked data-lane="{index}">{html.escape(label)}</label>' for index, label in enumerate(labels))
    document = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MGPU for NTE — measured five-frame timeline</title><style>
body{background:#0c1624;color:#dbe6f3;font:15px/1.6 system-ui;margin:24px}h1{font-size:25px}
.chart{overflow:auto;border:1px solid #30465f;border-radius:12px}svg{display:block}label{display:inline-block;margin:5px 18px 5px 0}
button{padding:9px 15px;margin:12px 0;background:#253d59;color:white;border:0;border-radius:6px}a{color:#68d9c4}.mark:hover{stroke:white;stroke-width:2}
</style><h1>MGPU for NTE: five measured frames</h1><p>Hover a bar for exact timing. Toggle lanes below. All data is embedded; no network requests or invented display frames.</p>
<section>BUTTONS</section><div class="chart">CHART</div><button id="download">Download original plotted data</button>
<p>Source: M340 foreground recording, 2026-10-21. These are timestamp brackets, not exclusive hardware running time or final display scanout. <a href="PERFORMANCE.zh-CN.md">Measurement scope</a>.</p>
<script type="application/json" id="data">DATA</script><script>
document.querySelectorAll('input[data-lane]').forEach(input=>input.addEventListener('change',()=>{
 document.querySelectorAll('.mark[data-lane="'+input.dataset.lane+'"]').forEach(mark=>mark.style.display=input.checked?'':'none');
}));document.getElementById('download').onclick=()=>{const blob=new Blob([document.getElementById('data').textContent],{type:'application/json'});const url=URL.createObjectURL(blob);const link=document.createElement('a');link.href=url;link.download='m340-five-frames.json';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000)};
</script></html>'''
    document = document.replace('BUTTONS', buttons).replace('CHART', chart).replace('DATA', json.dumps(data, ensure_ascii=True).replace('</', '<\\/'))
    (ROOT / 'docs/nte/five-frame-timeline.html').write_text(document, encoding='utf-8')


if __name__ == '__main__':
    FIGURES.mkdir(parents=True, exist_ok=True)
    performance()
    timeline()
    print('Rendered historical measurements and actual five-frame SVG/HTML.')

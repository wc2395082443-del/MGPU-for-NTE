import html
import json
from pathlib import Path

from render_evidence_helpers import comparison_svg, current_svg, copy_svg


ROOT = Path(__file__).resolve().parents[1]


def render():
    evidence = ROOT / 'docs/nte/evidence/five-frame-comparison.json'
    data = json.loads(evidence.read_text(encoding='utf-8'))
    figures = ROOT / 'docs/nte/figures'
    plots = {'five-frame-comparison.svg': comparison_svg(data),
             'copy-handoff-comparison.svg': copy_svg(data),
             'm364-instrumented-five-frames.svg': current_svg(data)}
    for name, content in plots.items():
        (figures / name).write_text(content, encoding='utf-8')
    before, after, current = data['runs']
    stats = data['statistics']
    rows = [
        ('所示五周期总时长', before['source_end'], after['source_end'], 'ms'),
        ('所示五周期均值', before['means']['period'], after['means']['period'], 'ms'),
        ('所示五周期倒数节奏', 1000 / before['means']['period'], 1000 / after['means']['period'], '/s'),
        ('完整窗口唯一NR完成', stats['old']['neural_cadence_hz'], stats['new']['neural_cadence_hz'], '/s'),
        ('完整窗口传输elapsed', stats['old']['transfer_elapsed_ms'], stats['new']['transfer_elapsed_ms'], 'ms'),
        ('完整窗口收发COPY交集', stats['old_copy_overlap_ms'], stats['new_copy_overlap_ms'], 'ms'),
    ]
    table = ''.join(f'<tr><td>{html.escape(label)}</td><td>{old:.3f} {unit}</td><td>{new:.3f} {unit}</td></tr>'
                    for label, old, new, unit in rows)
    document = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MGPU for NTE — 优化前后五帧流水线</title><style>
:root{color-scheme:dark}body{max-width:1500px;margin:25px auto;padding:0 18px;background:#0c1624;color:#dbe6f3;font:15px/1.7 system-ui}
h1{font-size:27px;margin-bottom:8px}h2{font-size:21px}p{max-width:1180px}a{color:#67dbc2}.note{padding:14px 18px;background:#263044;border-left:4px solid #f5c77d;border-radius:5px}
.chart{overflow-x:auto;border:1px solid #30465f;border-radius:10px;margin:16px 0}.chart svg{display:block;max-width:none}.mark:hover{stroke:#fff;stroke-width:1.5}table{border-collapse:collapse;width:100%;max-width:1000px}th,td{padding:9px 14px;border:1px solid #30465f;text-align:left}th{background:#172438}button{padding:9px 14px;margin:10px 12px 10px 0;background:#294867;color:white;border:0;border-radius:6px}label{margin-right:18px;display:inline-block}details{margin-top:22px}summary{font-size:19px;cursor:pointer}.hidden{display:none}
</style><h1>优化前后：两组各五帧，使用同一毫秒刻度</h1>
<p>M339c（分阶段COPY改动前）对比M340＋ForceTagging（改进后）。两次历史前台采样使用同一毫秒刻度。</p>
<div class="note">主要变化：<strong>4080S发送引导时，5080已经开始接收颜色</strong>。两版均有NR与游戏DIRECT重叠。当前M364＋M363的重采集五帧可在下方展开。</div>
<p><label><input id="guides" type="checkbox" checked>显示帧分界</label><label><input id="compute" type="checkbox" checked>显示COMPUTE查询</label><label><input id="previous" type="checkbox" checked>显示F0前一帧尾部</label></p>
<div class="chart" id="historical">HISTORICAL</div><h2>颜色先行交接：放大同一源帧的收发COPY</h2>
<p>各取第一个目标源帧，以出站COPY开始为零点。悬停可见实际端点，灰色段表示两次接收之间的等待。</p>
<div class="chart">COPY</div><h2>数字分母分开列</h2><table><thead><tr><th>指标</th><th>M339c</th><th>M340＋ForceTagging</th></tr></thead><tbody>ROWS</tbody></table>
<p>五周期缩短约38.476ms（20.23%）；完整窗口NR节奏历史提升25.29%，两者分母不同。M341后续又省0.510ms传输，但没有测到整体提帧。</p>
<details id="current"><summary>查看首发M364＋M363的实测五帧（详细采集开启）</summary>
<p class="note">2026-10-27重采集4.983–5.212秒窗口，五周期228.463ms。整轮main记录丢1条，867个fine任务截断。</p>
<div class="chart">CURRENT</div><p>图中保留主DIRECT、COMPUTE查询、出站COPY、接收COPY和NR五条轨道。</p></details>
<button id="download">导出完整对比数据JSON</button><p><a href="COMPARISON.zh-CN.md">数据来源</a> · <a href="PERFORMANCE.zh-CN.md">完整性能说明</a></p>
<script id="comparison-data" type="application/json">DATA</script><script>
const toggle=(selector,enabled)=>document.querySelectorAll(selector).forEach(item=>item.style.display=enabled?'':'none');
document.getElementById('guides').onchange=event=>toggle('.frame-guide',event.target.checked);
document.getElementById('compute').onchange=event=>toggle('.mark[data-lane="1"]',event.target.checked);
document.getElementById('previous').onchange=event=>toggle('.mark[data-frame="0"]',event.target.checked);
document.getElementById('download').onclick=()=>{const blob=new Blob([document.getElementById('comparison-data').textContent],{type:'application/json'});const url=URL.createObjectURL(blob);const anchor=document.createElement('a');anchor.href=url;anchor.download='nte-five-frame-comparison.json';anchor.click();setTimeout(()=>URL.revokeObjectURL(url),1000)};
</script></html>'''
    for placeholder, value in (('HISTORICAL', plots['five-frame-comparison.svg']),
                               ('COPY', plots['copy-handoff-comparison.svg']),
                               ('CURRENT', plots['m364-instrumented-five-frames.svg']), ('ROWS', table),
                               ('DATA', json.dumps(data, ensure_ascii=True).replace('</', '<\\/'))):
        document = document.replace(placeholder, value) if placeholder != 'COPY' else document.replace('>COPY</div>', '>' + value + '</div>')
    (ROOT / 'docs/nte/five-frame-comparison.html').write_text(document, encoding='utf-8')
    print('Rendered historical before/after, exact COPY handoff zoom, and separately labelled M364 instrumented window.')


if __name__ == '__main__':
    render()

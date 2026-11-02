#!/usr/bin/env python3
"""Create a local, standalone centroid GT annotation page (no network service)."""
import argparse
import base64
import hashlib
import json
from pathlib import Path

TEMPLATE=r'''<!doctype html><html lang="ko"><meta charset="utf-8"><title>나사 정답 위치 표시</title>
<style>body{margin:24px;background:#10151f;color:#e7edf5;font:16px system-ui}button,input{font:inherit;margin:6px;padding:8px}canvas{width:min(100%,1200px);height:auto;border:1px solid #596577;cursor:crosshair}p{max-width:1050px;line-height:1.6}.notice{color:#ffd485}button{cursor:pointer}</style>
<h1>나사 정답 위치 표시</h1><p>이미지에서 확인 가능한 실제 나사의 중심을 하나씩 클릭하세요. 모델의 예측은 표시하지 않습니다. 마지막 점 삭제 후 다시 찍을 수 있습니다. 흐려서 나사를 확인할 수 없다면 임의로 점을 추가하지 마세요.</p>
<p class="notice">19개라는 총개수만으로 위치 정답이 완성되지 않습니다. 이 파일은 한 정지 프레임의 정답이며 턴테이블 통과 이벤트의 정답을 대신하지 않습니다.</p>
<div><button id="undo">마지막 점 삭제</button><button id="clear">전체 지우기</button><span id="count"></span></div>
<canvas id="canvas"></canvas><p><label><input type="checkbox" id="confirmed">표시한 점이 각각 서로 다른 실제 나사임을 확인했습니다.</label></p>
<button id="save">정답 JSON 저장</button><p id="message"></p>
<script>
const metadata=__META__, points=[];const canvas=document.getElementById('canvas'),ctx=canvas.getContext('2d'),img=new Image();
function draw(){ctx.drawImage(img,0,0);points.forEach((p,i)=>{let x=p.x*img.width,y=p.y*img.height;ctx.beginPath();ctx.arc(x,y,12,0,2*Math.PI);ctx.strokeStyle='#00ffbf';ctx.lineWidth=3;ctx.stroke();ctx.font='24px sans-serif';ctx.fillStyle='#00ffbf';ctx.fillText(i+1,x+14,y)});document.getElementById('count').textContent=`표시 ${points.length} / 확인된 전체 ${metadata.expected_count}`}
img.onload=()=>{canvas.width=img.width;canvas.height=img.height;draw()};img.src='data:image/jpeg;base64,__IMAGE__';
canvas.onclick=e=>{const r=canvas.getBoundingClientRect();points.push({x:(e.clientX-r.left)/r.width,y:(e.clientY-r.top)/r.height,class_id:1});document.getElementById('confirmed').checked=false;draw()};
document.getElementById('undo').onclick=()=>{points.pop();document.getElementById('confirmed').checked=false;draw()};document.getElementById('clear').onclick=()=>{points.length=0;document.getElementById('confirmed').checked=false;draw()};
document.getElementById('save').onclick=()=>{if(points.length!==metadata.expected_count||!document.getElementById('confirmed').checked){document.getElementById('message').textContent='전체 개수와 위치를 검토하고 확인란을 체크해야 저장할 수 있습니다.';return}const report={...metadata,width:img.width,height:img.height,reviewed:true,scope:'one static image, human centroid labels',points:points.map((p,i)=>({...p,object_id:'screw-'+(i+1)}))};const url=URL.createObjectURL(new Blob([JSON.stringify(report,null,2)],{type:'application/json'}));const a=document.createElement('a');a.href=url;a.download='screw-static-gt.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000)};
</script></html>'''

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('image',type=Path);p.add_argument('out',type=Path)
    p.add_argument('--expected-count',type=int,required=True);a=p.parse_args()
    if a.expected_count<1:p.error('positive count required')
    data=a.image.read_bytes();meta=dict(schema=1,source_sha256=hashlib.sha256(data).hexdigest(),expected_count=a.expected_count)
    a.out.write_text(TEMPLATE.replace('__META__',json.dumps(meta)).replace('__IMAGE__',base64.b64encode(data).decode()))

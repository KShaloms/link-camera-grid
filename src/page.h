#pragma once
static const char PAGE[] PROGMEM=R"HTML(<!doctype html><html lang="ko"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>9칸 검사</title>
<style>body{max-width:700px;margin:24px auto;padding:0 12px;font:16px system-ui}canvas{width:100%;height:auto;background:#333;touch-action:manipulation}button{padding:8px;margin:4px}pre{white-space:pre-wrap}</style>
<h1>9칸 블록 검사</h1><p>9칸을 비우고 사진을 갱신하세요. 금속 칸막이 안에 블록을 놓는 3×3 보관판이 검사 영역입니다. 바닥에서 가장 바깥 두 칸막이의 안쪽 면이 만나는 네 꼭짓점을 1번 쪽 → 3번 쪽 → 9번 쪽 → 7번 쪽 순서로 누르세요.</p>
<canvas width="320" height="240" id="view"></canvas><br>
<button id="reset">사진 갱신 · 점 다시 선택</button><button id="save">모서리 저장</button><button id="learn">빈 판 학습</button>
<p id="msg"></p><pre id="status"></pre>
<script>
const canvas=document.querySelector('canvas'),ctx=canvas.getContext('2d'),image=new Image();
let points=[],saved=[];
// 선택 중인 점 또는 저장된 점을 사진 위에 연결하고 물리적인 칸 번호를 표시합니다.
function draw(){
 if(image.naturalWidth)ctx.drawImage(image,0,0);
 const p=points.length?points:saved;
 if(!p.length)return;
 ctx.beginPath();p.forEach(([x,y],i)=>{if(i)ctx.lineTo(x,y);else ctx.moveTo(x,y)});if(p.length===4)ctx.closePath();
 ctx.strokeStyle='lime';ctx.stroke();ctx.fillStyle='lime';p.forEach(([x,y],i)=>ctx.fillText([1,3,9,7][i],x,y));
}
image.onload=draw;image.onerror=()=>msg.textContent='카메라 프레임을 가져오지 못했습니다.';
reset.onclick=()=>{points=[];image.src='/frame?t='+Date.now()};
// 브라우저 화면에서 클릭한 위치를 카메라 원본 크기(320×240)의 좌표로 환산합니다.
canvas.onclick=e=>{if(!image.naturalWidth)return;if(points.length===4)points=[];const r=canvas.getBoundingClientRect();
 points.push([Math.min(319,Math.floor((e.clientX-r.left)*320/r.width)),Math.min(239,Math.floor((e.clientY-r.top)*240/r.height))]);draw()};
async function post(path,body){let r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});msg.textContent=await r.text();return r.ok}
save.onclick=async()=>{if(points.length!==4){msg.textContent='네 점을 모두 선택하세요.';return}if(await post('/grid',new URLSearchParams({p:points.flat().join(',')}))) {saved=points;points=[];draw()}};
learn.onclick=()=>{if(confirm('손과 블록을 모두 치웠나요?'))post('/learn','')};
// 0.8초마다 판정 결과를 읽습니다. 사진은 사진 갱신 버튼을 눌렀을 때 바뀝니다.
async function poll(){try{let j=await(await fetch('/status')).json();saved=j.points;draw();
 document.getElementById('status').textContent=['상태  '+['기준 대기','빈 판 학습 중','판정 준비 중','정상','카메라 오류'][j.state],'1~9번  '+j.cells.join('  '),'변화율(‰)  '+j.scores.join('  '),'프레임 번호  '+j.frame].join('\n')}catch{document.getElementById('status').textContent='접속 대기'}setTimeout(poll,800)}
reset.click();poll();
</script></html>)HTML";

/* Prototein explainer — all visualizations computed from real SAW counts in data.js. */
'use strict';

/* ----------------------------------------------------------------- data prep */
const Wmap = {};                       // Wmap[n] : Map "x,y" -> count
let GMAX = 0;
for (let n = 1; n <= NMAX; n++) {
  const m = new Map();
  for (const [x, y, c] of WDATA[n]) { m.set(x + ',' + y, c); if (c > GMAX) GMAX = c; }
  Wmap[n] = m;
}
const W = (n, x, y) => (Wmap[n] ? (Wmap[n].get(x + ',' + y) || 0) : 0);
const U = (n, x, y) => W(n-1, x-1, y) + W(n-1, x+1, y) + W(n-1, x, y-1) + W(n-1, x, y+1); // free random-walk push
const shellC = (n, x, y) => (n - (Math.abs(x) + Math.abs(y))) / 2;

/* ------------------------------------------------------------------- colours */
const STOPS = [[13,20,33],[34,52,99],[31,110,165],[31,158,125],[150,180,60],[230,200,70],[245,238,180]];
function heat(t){ t=Math.max(0,Math.min(1,t)); const f=t*(STOPS.length-1), i=Math.floor(f), u=f-i;
  const a=STOPS[i], b=STOPS[Math.min(i+1,STOPS.length-1)];
  return `rgb(${a[0]+(b[0]-a[0])*u|0},${a[1]+(b[1]-a[1])*u|0},${a[2]+(b[2]-a[2])*u|0})`; }
const fmt = v => v.toLocaleString('en-US');

/* --------------------------------------------------------------- geometry/hover */
function geo(cv, n, pad){ const R=n+(pad||0); const cs=Math.min(cv.width,cv.height)/(2*R+1);
  return { R, cs, cx:cv.width/2, cy:cv.height/2 }; }
const S = (g,x,y)=>[g.cx + x*g.cs, g.cy - y*g.cs];
function mouseCell(cv,e){ const r=cv.getBoundingClientRect(), sx=cv.width/r.width, sy=cv.height/r.height;
  const g=cv._geo; if(!g) return null; const mx=(e.clientX-r.left)*sx, my=(e.clientY-r.top)*sy;
  const x=Math.round((mx-g.cx)/g.cs), y=Math.round((g.cy-my)/g.cs);
  if(Math.abs(x)+Math.abs(y)>g.R+0.5) return null; return {x,y}; }
const tip=document.getElementById('tip');
function showTip(e,txt){ tip.textContent=txt; tip.style.opacity=1; tip.style.left=(e.clientX+14)+'px'; tip.style.top=(e.clientY+14)+'px'; }
function hideTip(){ tip.style.opacity=0; }

/* ------------------------------------------------------------ generic diamond */
function diamond(cv, n, opt){
  const ctx=cv.getContext('2d'); ctx.clearRect(0,0,cv.width,cv.height);
  const g=geo(cv,n,opt.pad||0); cv._geo=g; const cs=g.cs;
  ctx.font=`${Math.max(8,cs*0.42|0)}px monospace`;
  ctx.textAlign='center'; ctx.textBaseline='middle';
  for(let y=-g.R;y<=g.R;y++) for(let x=-g.R;x<=g.R;x++){
    if((Math.abs(x)+Math.abs(y))>g.R) continue;
    const [sx,sy]=S(g,x,y); const v=opt.val?opt.val(x,y):0;
    const sz=cs*0.92;
    // background dot for the lattice
    const reachable = ((x+y) % 2 + 2) % 2 === n % 2;
    if(opt.cell){ const r=opt.cell(x,y,v); if(r&&r.fill){ ctx.fillStyle=r.fill; ctx.fillRect(sx-sz/2,sy-sz/2,sz,sz); }
      if(r&&r.stroke){ ctx.lineWidth=r.lw||2; ctx.strokeStyle=r.stroke; ctx.strokeRect(sx-sz/2,sy-sz/2,sz,sz); }
      if(r&&r.dot){ ctx.fillStyle=r.dot; ctx.beginPath(); ctx.arc(sx,sy,Math.max(1,cs*0.08),0,7); ctx.fill(); }
    } else {
      if(v>0){ ctx.fillStyle = opt.color?opt.color(v):heat(Math.log(v+1)/Math.log(GMAX+1)); ctx.fillRect(sx-sz/2,sy-sz/2,sz,sz); }
      else if(reachable){ ctx.fillStyle='#1b232d'; ctx.fillRect(sx-sz/2,sy-sz/2,sz,sz); }
    }
    if(opt.label){ const t=opt.label(x,y,v); if(t!=null){ ctx.fillStyle=opt.labelColor?opt.labelColor(x,y,v):'#0c1118'; ctx.fillText(t,sx,sy); } }
  }
  if(opt.path&&opt.path.length){ ctx.lineWidth=Math.max(2,cs*0.16); ctx.strokeStyle=opt.pathColor||'#ffd166'; ctx.lineJoin='round'; ctx.lineCap='round';
    ctx.beginPath(); opt.path.forEach((p,i)=>{ const [sx,sy]=S(g,p[0],p[1]); i?ctx.lineTo(sx,sy):ctx.moveTo(sx,sy); }); ctx.stroke();
    const [hx,hy]=S(g,...opt.path[opt.path.length-1]); ctx.fillStyle=opt.pathColor||'#ffd166'; ctx.beginPath(); ctx.arc(hx,hy,cs*0.22,0,7); ctx.fill();
    const [ox,oy]=S(g,...opt.path[0]); ctx.fillStyle='#fff'; ctx.beginPath(); ctx.arc(ox,oy,cs*0.18,0,7); ctx.fill();
  }
  if(opt.after) opt.after(ctx,g);
}
function bind(cv,fn){ cv.addEventListener('mousemove',e=>{const c=mouseCell(cv,e); fn(c,e);});
  cv.addEventListener('mouseleave',()=>{hideTip(); fn(null,null);}); }

/* ============================================================ 1. FOLD figure */
(function(){
  const cv=document.getElementById('cv-fold'); if(!cv) return; const ctx=cv.getContext('2d');
  // snake fold, 20 beads
  const path=[]; let id=0;
  for(let row=0;row<4;row++){ const xs=row%2? [4,3,2,1,0]:[0,1,2,3,4]; for(const x of xs) path.push([x,row]); }
  const hp="PPPPHHPPHPHHHPPHHHPP".split('');
  // find H-H contacts (grid-adjacent, not sequence-adjacent)
  const pos=new Map(); path.forEach((p,i)=>pos.set(p[0]+','+p[1],i));
  const contacts=[];
  for(let i=0;i<path.length;i++){ if(hp[i]!=='H')continue; const[x,y]=path[i];
    for(const[dx,dy]of[[1,0],[0,1]]){ const j=pos.get((x+dx)+','+(y+dy)); if(j!=null&&hp[j]==='H'&&Math.abs(j-i)>1) contacts.push([i,j]); } }
  function draw(){ ctx.clearRect(0,0,cv.width,cv.height); const cs=70, ox=180, oy=70;
    const SS=(x,y)=>[ox+x*cs, oy+y*cs];
    // contacts
    ctx.strokeStyle='#e8590c'; ctx.lineWidth=4; ctx.setLineDash([2,6]); ctx.lineCap='round';
    for(const[i,j]of contacts){ const[a,b]=SS(...path[i]),[c,d]=SS(...path[j]); ctx.beginPath();ctx.moveTo(a,b);ctx.lineTo(c,d);ctx.stroke(); }
    ctx.setLineDash([]);
    // chain
    ctx.strokeStyle='#46566a'; ctx.lineWidth=5; ctx.beginPath();
    path.forEach((p,i)=>{const[a,b]=SS(...p); i?ctx.lineTo(a,b):ctx.moveTo(a,b);}); ctx.stroke();
    // beads
    path.forEach((p,i)=>{ const[a,b]=SS(...p); const H=hp[i]==='H';
      ctx.beginPath(); ctx.arc(a,b,16,0,7); ctx.fillStyle=H?'#1a2433':'#dfe7ee'; ctx.fill();
      ctx.lineWidth=2; ctx.strokeStyle=H?'#0b1320':'#9fb0c0'; ctx.stroke();
      ctx.fillStyle=H?'#7fd1ff':'#5d6b7a'; ctx.font='bold 13px monospace'; ctx.textAlign='center';ctx.textBaseline='middle';
      ctx.fillText(hp[i],a,b); });
    // legend text
    ctx.textAlign='left'; ctx.fillStyle='#9fb0c0'; ctx.font='13px sans-serif';
    ctx.fillText('● H  hydrophobic     ○ P  polar', ox, oy+4*cs-6);
  }
  draw();
  document.getElementById('cap-fold').innerHTML =
    `This 20-bead fold scores <b style="color:#e8590c">${contacts.length} H–H contacts</b> (dashed orange). The best fold maximises that number; brute force tries them all.`;
})();

/* ====================================================== 2. WALK enumeration */
(function(){
  const cv=document.getElementById('cv-walk'); if(!cv) return;
  let n=+document.getElementById('rng-walk-n').value, playing=false, raf=null;
  let occ, stack, total, completed, lastFull=null;
  function reset(){ occ=new Set(['0,0']); stack=[{x:0,y:0,d:0}]; total=0; completed=0; lastFull=null; }
  reset();
  const DIRS=[[1,0],[0,1],[-1,0],[0,-1]];
  function step(){ // one DFS micro-step
    const top=stack[stack.length-1];
    if(stack.length===n){ // full walk
      total++; completed++; lastFull=stack.map(s=>[s.x,s.y]); stack.pop(); occ.delete(top.x+','+top.y); return; }
    let moved=false;
    while(top.d<4){ const[dx,dy]=DIRS[top.d++]; const nx=top.x+dx, ny=top.y+dy, k=nx+','+ny;
      if(!occ.has(k)){ occ.add(k); stack.push({x:nx,y:ny,d:0}); moved=true; break; } }
    if(!moved){ stack.pop(); occ.delete(top.x+','+top.y); if(stack.length===0){ reset(); } }
  }
  function render(){ const path=stack.map(s=>[s.x,s.y]);
    diamond(cv,n,{ pad:1, cell:(x,y)=>occ.has(x+','+y)?{fill:'#243140'}:null,
      path: lastFull&&stack.length<2?lastFull:path,
      pathColor: stack.length===n||(lastFull&&stack.length<2)?'#1e9e6a':'#ffd166' });
    document.getElementById('walk-count').textContent=`${total} full walks found`;
  }
  function loop(){ for(let i=0;i<2;i++) step(); render(); if(playing) raf=requestAnimationFrame(loop); }
  document.getElementById('btn-walk-play').onclick=function(){ playing=!playing; this.classList.toggle('on',playing); this.textContent=playing?'❚❚ pause':'▶ grow walks'; if(playing) loop(); else cancelAnimationFrame(raf); };
  document.getElementById('btn-walk-step').onclick=()=>{ for(let i=0;i<3;i++) step(); render(); };
  const rn=document.getElementById('rng-walk-n');
  rn.oninput=function(){ n=+this.value; document.getElementById('val-walk-n').textContent=n; reset(); render(); };
  document.getElementById('cap-walk').innerHTML='The pen draws a path, backtracking at dead ends. Every completed full-length path (green) is one distinct fold.';
  render();
  // growth KPIs
  const kg=document.getElementById('kpi-growth'); const ns=[4,8,12,16];
  kg.innerHTML=ns.map(k=>`<div class="k"><div class="n">${fmt(TOTALS[k-1])}</div><div class="l">walks of length ${k}</div></div>`).join('');
})();

/* ========================================================= 3. ENDPOINT DIAMOND */
(function(){
  const cv=document.getElementById('cv-diamond'); if(!cv) return;
  let n=+document.getElementById('rng-d-n').value, logc=true, nums=false;
  function maxN(){ let m=1; for(const[,v] of Wmap[n]) if(v>m)m=v; return m; }
  function render(){
    const mx=maxN();
    diamond(cv,n,{ val:(x,y)=>W(n,x,y),
      color:v=> logc?heat(0.08+0.92*Math.log(v+1)/Math.log(mx+1)):heat(v/mx),
      label: nums&&n<=11?((x,y,v)=>v>0?(v<10000?fmt(v):''):null):null, labelColor:()=>'#06243a' });
    document.getElementById('d-total').textContent=`Σ = ${fmt(TOTALS[n-1])} walks total`;
  }
  bind(cv,(c,e)=>{ if(!c){return;} const v=W(n,c.x,c.y);
    if(v>0||((c.x+c.y)%2+2)%2===n%2){ const cc=shellC(n,c.x,c.y);
      showTip(e,`(${c.x}, ${c.y})\n${fmt(v)} walks\nshell c=${cc}  surplus=${2*cc}`); } else hideTip(); });
  document.getElementById('rng-d-n').oninput=function(){ n=+this.value; document.getElementById('val-d-n').textContent=n; render(); };
  document.getElementById('btn-d-log').onclick=function(){ logc=!logc; this.classList.toggle('on',logc); render(); };
  document.getElementById('btn-d-num').onclick=function(){ nums=!nums; this.classList.toggle('on',nums); render(); };
  render();
})();

/* ============================================================== 4. SYMMETRY */
(function(){
  const cv=document.getElementById('cv-sym'); if(!cv) return;
  let n=+document.getElementById('rng-sym-n').value, folded=false, picked=null;
  const inWedge=(x,y)=> x>=0&&y>=0&&y<=x;
  const images=(x,y)=>{ const a=Math.abs(x),b=Math.abs(y); const s=new Set();
    for(const[p,q] of [[a,b],[b,a]]) for(const sx of[1,-1]) for(const sy of[1,-1]) s.add((sx*p)+','+(sy*q));
    return [...s].map(k=>k.split(',').map(Number)); };
  function render(){
    diamond(cv,n,{ cell:(x,y)=>{ const v=W(n,x,y); if(v<=0&&!(((x+y)%2+2)%2===n%2)) return null;
      const w=inWedge(x,y); let fill = v>0?heat(Math.log(v+1)/Math.log(TOTALS[n-1])):'#1b232d';
      if(folded&&!w) fill='#161c24';
      const r={fill};
      if(w){ r.stroke='#caa64b'; r.lw=1.4; }
      if(picked){ for(const[ix,iy] of images(picked[0],picked[1])) if(ix===x&&iy===y){ r.stroke='#2f81f7'; r.lw=3; } }
      return r; } });
  }
  bind(cv,(c,e)=>{ if(c&&((c.x+c.y)%2+2)%2===n%2){ showTip(e,`(${c.x}, ${c.y}) → 8 images`); } else hideTip(); });
  cv.addEventListener('click',e=>{ const c=mouseCell(cv,e); if(c){ picked=[c.x,c.y]; render(); } });
  document.getElementById('rng-sym-n').oninput=function(){ n=+this.value; document.getElementById('val-sym-n').textContent=n; picked=null; render(); };
  document.getElementById('btn-sym-fold').onclick=function(){ folded=!folded; this.classList.toggle('on',folded); this.textContent=folded?'◂ unfold':'fold into the wedge ▸'; render(); };
  render();
})();

/* ============================================================== 5. PASCAL */
(function(){
  const cv=document.getElementById('cv-pascal'); if(!cv) return;
  let n=+document.getElementById('rng-pas-n').value, trace=null, raf=null;
  const binom=(N,k)=>{ let r=1; for(let i=0;i<k;i++) r=r*(N-i)/(i+1); return Math.round(r); };
  function render(){
    diamond(cv,n,{ pad:0,
      cell:(x,y)=>{ const v=W(n,x,y); const onRim=(Math.abs(x)+Math.abs(y))===n;
        if(onRim) return {fill: '#caa64b'};
        if(v>0) return {fill:'#222d39'}; return ((x+y)%2+2)%2===n%2?{fill:'#1b232d'}:null; },
      label:(x,y)=>{ if((Math.abs(x)+Math.abs(y))!==n) return null;
        // k = number of up-ish steps; on NE edge x+y=n with x=n-k? use position along rim
        const v=W(n,x,y); return n<=12? fmt(v):null; }, labelColor:()=>'#1a1205',
      path: trace, pathColor:'#2f81f7' });
  }
  document.getElementById('rng-pas-n').oninput=function(){ n=+this.value; document.getElementById('val-pas-n').textContent=n; trace=null; render(); };
  document.getElementById('btn-pas-trace').onclick=function(){ cancelAnimationFrame(raf);
    const k=Math.floor(n/2); const full=[]; let x=0,y=0; full.push([0,0]);
    for(let i=0;i<n-k;i++){x++;full.push([x,y]);} for(let i=0;i<k;i++){y++;full.push([x,y]);}
    let t=1; trace=[full[0]]; (function anim(){ trace=full.slice(0,++t); render(); if(t<full.length) raf=requestAnimationFrame(anim); })(); };
  render();
})();

/* ============================================================== 6. BANDS */
(function(){
  const cv=document.getElementById('cv-bands'); if(!cv) return;
  let n=+document.getElementById('rng-band-n').value, cc=+document.getElementById('rng-band-c').value;
  const BC=['#3b6ea5','#2f9e7d','#caa64b','#e8590c','#c9344a','#8e44ad','#16a085','#d35400','#7f8c8d'];
  function render(){
    const maxc=Math.floor(n/2); cc=Math.min(cc,maxc);
    document.getElementById('rng-band-c').max=maxc; document.getElementById('val-band-c').textContent=cc;
    diamond(cv,n,{ cell:(x,y)=>{ const v=W(n,x,y); if(v<=0) return ((x+y)%2+2)%2===n%2?{fill:'#161c24'}:null;
      const c=shellC(n,x,y); const base=BC[c%BC.length];
      if(c===cc) return {fill:base, stroke:'#fff', lw:1.2};
      return {fill: base+'33'}; } });
    const dist=n-2*cc, cells=Wmap[n]? [...Wmap[n].keys()].filter(k=>{const[x,y]=k.split(',').map(Number);return shellC(n,x,y)===cc;}).length:0;
    document.getElementById('cap-bands').innerHTML=`Shell <b style="color:${BC[cc%BC.length]}">c = ${cc}</b> · distance ${dist} from home · <b>surplus = ${2*cc}</b> wasted steps · ${cells} squares. ${cc===0?'The rim — no surplus, no way to collide.':cc===maxc?'The centre — almost all surplus; nothing but collisions.':''}`;
  }
  document.getElementById('rng-band-n').oninput=function(){ n=+this.value; document.getElementById('val-band-n').textContent=n; render(); };
  document.getElementById('rng-band-c').oninput=function(){ cc=+this.value; render(); };
  render();
})();

/* =================================================== 7. DIFF TABLES + HOLO */
(function(){
  const host=document.getElementById('diff-tables'); if(!host) return;
  function onAxis(c){ const s=[]; for(let n=2*c;n<=NMAX;n++){ if(n<1)continue; s.push(W(n,n-2*c,0)); } return s; }
  function diffs(seq,k){ let cur=seq.slice(); for(let i=0;i<k;i++) cur=cur.slice(1).map((v,j)=>v-cur[j]); return cur; }
  let html='';
  for(let c=0;c<=3;c++){ const seq=onAxis(c).slice(0,10); const deg=2*c;
    html+=`<table class="seq"><tr><th>shell c=${c} &nbsp;(degree ${deg})</th>`+seq.map(v=>`<td>${fmt(v)}</td>`).join('')+`</tr>`;
    for(let k=1;k<=deg;k++){ const d=diffs(seq,k); const isLead=(k===deg);
      html+=`<tr><th>Δ<sup>${k}</sup></th>`+d.map(v=>`<td class="${isLead?'lead':''}">${fmt(v)}</td>`).join('')+`</tr>`; }
    html+=`</table>`;
    if(c>0) html+=`<div class="muted" style="font-size:13px;margin:-2px 0 14px">→ ${2*c}ᵗʰ difference is constant <b style="color:#e8590c">${CENTRAL_BINOM[c]}</b> = C(${2*c},${c}). Closed-form polynomial.</div>`;
  }
  host.innerHTML=html;

  // holonomic schematic
  const cv=document.getElementById('cv-holo'); const ctx=cv.getContext('2d');
  ctx.clearRect(0,0,cv.width,cv.height);
  const orders=[1,2,3,4,5,6]; const x0=60,y0=40,bw=92,bh=26,gap=8;
  ctx.font='12px monospace';
  ctx.fillStyle='#9fb0c0'; ctx.textAlign='left';
  ctx.fillText('Catalan numbers (a known closed-form sequence) — control:', x0, 24);
  orders.forEach((r,i)=>{ const yy=y0+0*0; const xx=x0+i*(bw+gap);
    ctx.fillStyle='#1e9e6a'; ctx.fillRect(xx,30,bw,bh); ctx.fillStyle='#06120c'; ctx.textAlign='center';
    ctx.fillText(`order ${r} ✓`, xx+bw/2, 30+bh/2+4); });
  ctx.textAlign='left'; ctx.fillStyle='#9fb0c0';
  ctx.fillText('A225877 = n·(self-avoiding polygons) — the centre of the diamond:', x0, 96);
  orders.forEach((r,i)=>{ const xx=x0+i*(bw+gap);
    ctx.fillStyle='#c9344a'; ctx.fillRect(xx,104,bw,bh); ctx.fillStyle='#1a0508'; ctx.textAlign='center';
    ctx.fillText(`order ${r} ✗`, xx+bw/2, 104+bh/2+4); });
  ctx.textAlign='left'; ctx.fillStyle='#e7eef5'; ctx.font='13px sans-serif';
  ctx.fillText('Every recurrence (order ≤ 6, degree ≤ 7) tested against 65 exact terms → full rank → no fit.', x0, 176);
  ctx.fillStyle='#c9344a'; ctx.font='bold 13px sans-serif';
  ctx.fillText('Non-holonomic — a re-demonstration of the proven non-D-finiteness (Conway–Guttmann; Rechnitzer).', x0, 200);
  document.getElementById('cap-holo').textContent='Green = a recurrence exists (Catalan control passes). Red = none (the hard sequences). This re-sees a known result; it is not a new proof.';
})();

/* ============================================================== 8. PATCH */
(function(){
  const cv=document.getElementById('cv-patch'); if(!cv) return;
  let n=+document.getElementById('rng-patch-n').value, mode='U';
  const btns={U:document.getElementById('btn-patch-U'),W:document.getElementById('btn-patch-W'),C:document.getElementById('btn-patch-C')};
  function maxVal(f){ let m=1; for(let y=-n;y<=n;y++)for(let x=-n;x<=n;x++){ const v=f(x,y); if(v>m)m=v; } return m; }
  function render(){
    let f, cap, col;
    if(mode==='U'){ f=(x,y)=>U(n,x,y); cap='<b>Uₙ</b> — the free guess: each square = sum of its 4 neighbours from length n−1. Pure arithmetic on prior data. (Overcounts.)'; }
    else if(mode==='W'){ f=(x,y)=>W(n,x,y); cap='<b>Wₙ</b> — the truth: actual self-avoiding-walk counts.'; }
    else { f=(x,y)=>U(n,x,y)-W(n,x,y); cap='correction'; }
    const mx=maxVal(f);
    diamond(cv,n,{ val:f, color: mode==='C'? (v=> v>0?heat(0.25+0.75*v/mx):'#161c24')
                                            : (v=> v>0?heat(Math.log(v+1)/Math.log(mx+1)):'#161c24') });
    if(mode==='C'){ let supp=0; for(let y=-n;y<=n;y++)for(let x=-n;x<=n;x++) if(U(n,x,y)-W(n,x,y)>0) supp++;
      cap=`<b>Cₙ = Uₙ − Wₙ</b> — the only thing you must compute. Support = <b style="color:#e8590c">${supp}</b> squares (exactly (n−1)² = ${(n-1)*(n-1)}). Zero on the boundary shell — the rim patches itself.`; }
    document.getElementById('cap-patch').innerHTML=cap;
  }
  for(const k in btns) btns[k].onclick=function(){ mode=k; for(const j in btns) btns[j].classList.toggle('on',j===k); render(); };
  bind(cv,(c,e)=>{ if(!c) return; const u=U(n,c.x,c.y),w=W(n,c.x,c.y);
    if(u||w) showTip(e,`(${c.x}, ${c.y})\nUₙ=${fmt(u)}  Wₙ=${fmt(w)}\ncorrection=${fmt(u-w)}`); else hideTip(); });
  document.getElementById('rng-patch-n').oninput=function(){ n=+this.value; document.getElementById('val-patch-n').textContent=n; render(); };
  render();
})();

/* ============================================ classification (shared 9 & 10) */
function classify(n,x,y){ // for reachable cells only
  const rep = x>=0&&y>=0&&y<=x;             // canonical wedge representative
  if(!rep) return 'sym';
  const c=shellC(n,x,y);
  if(c===0) return 'rim';
  if(c===1) return 'cf';
  if(y===0) return 'axis';                  // on-axis spine of deep shells: extrapolable
  return 'core';                            // near-diagonal interior of c>=2: must enumerate
}
const CATCOL={sym:'#33414f',rim:'#3b6ea5',cf:'#2f9e7d',axis:'#6b7b8c',core:'#e8590c'};

/* ============================================================== 9. CORE */
(function(){
  const cv=document.getElementById('cv-core'); if(!cv) return;
  let n=+document.getElementById('rng-core-n').value;
  function counts(){ let tot=0,core=0,sym=0; for(const k of Wmap[n].keys()){ const[x,y]=k.split(',').map(Number);
    tot++; const cat=classify(n,x,y); if(cat==='core')core++; if(cat==='sym')sym++; } return {tot,core,sym}; }
  function render(){
    diamond(cv,n,{ cell:(x,y)=>{ const v=W(n,x,y); if(v<=0) return ((x+y)%2+2)%2===n%2?{fill:'#11161d'}:null;
      const cat=classify(n,x,y); return {fill:CATCOL[cat], stroke:cat==='core'?'#ffd166':undefined, lw:1}; } });
    const {tot,core}=counts();
    document.getElementById('cap-core').innerHTML=`Length ${n}: of <b>${tot}</b> reachable squares, only <b style="color:#e8590c">${core}</b> carry genuinely new information (no closed-form rule). The rest are symmetry copies, the binomial rim, or extrapolable shells — <em>but each core square is a count as hard as the whole problem.</em>`;
    const pct=(100*core/Math.max(1,tot)).toFixed(0);
    document.getElementById('kpi-core').innerHTML=
      `<div class="k"><div class="n">${tot}</div><div class="l">reachable squares in the diamond</div></div>`+
      `<div class="k"><div class="n" style="color:#e8590c">${core}</div><div class="l">with no closed-form rule (the core)</div></div>`+
      `<div class="k"><div class="n">${pct}%</div><div class="l">of squares — small in count, not in work</div></div>`;
  }
  document.getElementById('rng-core-n').oninput=function(){ n=+this.value; document.getElementById('val-core-n').textContent=n; render(); };
  render();
})();

/* ============================================================== 10. PIPELINE + BUILD */
(function(){
  const steps=[
    ['free','Fold into one wedge','8-fold symmetry — compute ⅛, stamp the rest.'],
    ['free','Write the rim','Outer shell = C(n,k), Pascal\'s triangle. A formula.'],
    ['free','Fill shell c = 1','Closed-form "one-defect" walks.'],
    ['free','Extrapolate near-axis shells','Each is a degree-2c polynomial — extend from prior lengths.'],
    ['free','Push the bulk outward','Uₙ from the length n−1 table — free arithmetic.'],
    ['hard','Enumerate the core','Near-diagonal interior of shells c ≥ 2: the only real work.'],
    ['free','Reflect / rotate ×8','Recover the whole diamond from the wedge.'],
  ];
  const host=document.getElementById('pipeline');
  host.innerHTML=steps.map((s,i)=>`<div class="step ${s[0]}"><div class="ix">${i+1}</div><div class="t"><b>${s[1]}</b><div class="d">${s[2]}</div></div></div>`).join('');

  const cv=document.getElementById('cv-build'); const ctx=cv.getContext('2d');
  function tally(n){ let free=0,core=0; for(const k of Wmap[n].keys()){ const[x,y]=k.split(',').map(Number);
    classify(n,x,y)==='core'?core++:free++; } return {free,core}; }
  const data=[]; for(let n=1;n<=NMAX;n++) data.push(tally(n));
  let shown=0, raf=null;
  function draw(){ ctx.clearRect(0,0,cv.width,cv.height);
    const x0=46,y0=18,W0=cv.width-70,H0=cv.height-70; const bw=W0/NMAX*0.7, gap=W0/NMAX*0.3;
    let mx=0; data.forEach(d=>{mx=Math.max(mx,d.free+d.core);});
    ctx.font='11px monospace'; ctx.textAlign='center';
    for(let i=0;i<shown;i++){ const d=data[i]; const xx=x0+i*(bw+gap);
      const hf=(d.free/mx)*H0, hc=(d.core/mx)*H0;
      ctx.fillStyle='#33414f'; ctx.fillRect(xx,y0+H0-hf,bw,hf);
      ctx.fillStyle='#e8590c'; ctx.fillRect(xx,y0+H0-hf-hc,bw,hc);
      ctx.fillStyle='#7e8ea0'; ctx.fillText(i+1,xx+bw/2,y0+H0+14); }
    ctx.textAlign='left'; ctx.fillStyle='#9fb0c0'; ctx.font='12px sans-serif';
    ctx.fillText('■ rule-fillable (symmetry · formulas · extrapolation · push)',46,cv.height-8);
    ctx.fillStyle='#e8590c'; ctx.fillText('■ core (no closed-form rule)',430,cv.height-8);
    if(shown>0){ const d=data[shown-1]; const pct=(100*d.core/(d.free+d.core)).toFixed(0);
      document.getElementById('build-status').textContent=`length ${shown}: ${d.core} core / ${d.free+d.core} squares (${pct}% have no rule)`; }
  }
  document.getElementById('btn-build').onclick=function(){ cancelAnimationFrame(raf); shown=0;
    (function go(){ shown++; draw(); if(shown<NMAX) raf=setTimeout(()=>requestAnimationFrame(go),260); })(); };
  shown=NMAX; draw();
})();

/* ===================================== 11. FINITE-LATTICE / TRANSFER MATRIX */
(function(){
  const cv=document.getElementById('cv-fl'); if(!cv) return; const ctx=cv.getContext('2d');
  const P=[[0,0],[4,0],[4,1],[1,1],[1,2],[4,2],[4,3],[0,3]]; // a "C"-shaped self-avoiding polygon
  const GW=4, GH=3;
  function densify(P){ const V=[]; const n=P.length;
    for(let i=0;i<n;i++){ let x=P[i][0],y=P[i][1]; const bx=P[(i+1)%n][0],by=P[(i+1)%n][1];
      const dx=Math.sign(bx-x),dy=Math.sign(by-y);
      while(x!==bx||y!==by){ V.push([x,y]); x+=dx; y+=dy; } }
    return V; }
  const V=densify(P);
  let cut=+document.getElementById('rng-fl').value, playing=false, raf=null;
  function compute(cutX){ const cross=[],arcs=[]; const M=V.length;
    for(let i=0;i<M;i++){ const a=V[i],b=V[(i+1)%M];
      if(a[1]===b[1] && (a[0]-cutX)*(b[0]-cutX)<0) cross.push({i,y:a[1]}); }
    const m=cross.length;
    for(let k=0;k<m;k++){ const c1=cross[k],c2=cross[(k+1)%m];
      const mid=V[(c1.i+1)%M]; arcs.push({y1:c1.y,y2:c2.y,side:mid[0]<cutX?'L':'R'}); }
    return {cross,arcs}; }
  function wrap(c,t,x,y,mw,lh){ const w=t.split(' '); let ln='',yy=y;
    for(const o of w){ const tt=ln+o+' '; if(c.measureText(tt).width>mw){ c.fillText(ln,x,yy); ln=o+' '; yy+=lh; } else ln=tt; } c.fillText(ln,x,yy); }
  function draw(){ ctx.clearRect(0,0,cv.width,cv.height);
    const cs=Math.min((cv.width-230)/(GW+1),(cv.height-70)/(GH+1));
    const ox=64, oy=cv.height-34, SX=x=>ox+x*cs, SY=y=>oy-y*cs, cutX=cut+0.5;
    ctx.fillStyle='#172029'; ctx.fillRect(SX(-0.4),SY(GH+0.4),(cutX+0.4)*cs,(GH+0.8)*cs);
    ctx.fillStyle='#2a3642';
    for(let y=0;y<=GH;y++)for(let x=0;x<=GW;x++){ ctx.beginPath(); ctx.arc(SX(x),SY(y),2,0,7); ctx.fill(); }
    ctx.strokeStyle='#5d8fc0'; ctx.lineWidth=4; ctx.lineJoin='round'; ctx.beginPath();
    P.forEach((p,i)=>{ i?ctx.lineTo(SX(p[0]),SY(p[1])):ctx.moveTo(SX(p[0]),SY(p[1])); }); ctx.closePath(); ctx.stroke();
    ctx.strokeStyle='#caa64b'; ctx.lineWidth=2; ctx.setLineDash([5,5]); ctx.beginPath();
    ctx.moveTo(SX(cutX),SY(-0.4)); ctx.lineTo(SX(cutX),SY(GH+0.4)); ctx.stroke(); ctx.setLineDash([]);
    const {cross,arcs}=compute(cutX);
    arcs.forEach(a=>{ if(a.side!=='L')return; const y1=SY(a.y1),y2=SY(a.y2),x=SX(cutX);
      const bulge=14+Math.abs(a.y1-a.y2)*cs*0.55;
      ctx.strokeStyle='#e8590c'; ctx.lineWidth=2.5; ctx.beginPath();
      ctx.moveTo(x,y1); ctx.quadraticCurveTo(x-bulge,(y1+y2)/2,x,y2); ctx.stroke(); });
    cross.forEach(c=>{ ctx.fillStyle='#ffd166'; ctx.beginPath(); ctx.arc(SX(cutX),SY(c.y),5,0,7); ctx.fill(); });
    const px=SX(GW)+34;
    ctx.textAlign='left'; ctx.fillStyle='#cfe0ee'; ctx.font='13px sans-serif';
    ctx.fillText(`cut at x = ${cutX}`, px, 40);
    ctx.fillStyle='#ffd166'; ctx.fillText(`${cross.length} strands cross`, px, 66);
    ctx.fillStyle='#e8590c'; ctx.fillText(`${arcs.filter(a=>a.side==='L').length} arcs stored (left)`, px, 92);
    ctx.fillStyle='#8497a8'; ctx.font='12px sans-serif';
    wrap(ctx,'This pairing is the whole state. The shaded left fill is forgotten.',px,120,cv.width-px-14,16);
    document.getElementById('cap-fl').innerHTML='Sweep the cut (gold). Only the <b style="color:#e8590c">connectivity of the strands behind the line</b> (orange arcs) is stored — every different left-filling with the same pairing collapses to one state.';
  }
  document.getElementById('rng-fl').oninput=function(){ cut=+this.value; document.getElementById('val-fl').textContent=(cut+0.5); draw(); };
  document.getElementById('btn-fl-play').onclick=function(){ playing=!playing; this.classList.toggle('on',playing);
    if(playing){ (function go(){ cut=(cut+1)%GW; const r=document.getElementById('rng-fl'); r.value=cut; document.getElementById('val-fl').textContent=(cut+0.5); draw(); if(playing) raf=setTimeout(()=>requestAnimationFrame(go),900); })(); } else clearTimeout(raf); };
  document.getElementById('val-fl').textContent=(cut+0.5); draw();
})();

/* ===================================== 12. LACE EXPANSION (schematic series) */
(function(){
  const cv=document.getElementById('cv-lace'); if(!cv) return; const ctx=cv.getContext('2d');
  let terms=1;
  function draw(){ ctx.clearRect(0,0,cv.width,cv.height);
    ctx.textAlign='left'; ctx.fillStyle='#9fb0c0'; ctx.font='13px sans-serif';
    ctx.fillText('G  (walks from 0 to x)  =  a random-walk line, decorated with local self-avoidance corrections:', 22, 24);
    const x0=70, x1=cv.width-150, oy=58, rowH=56;
    for(let t=0;t<terms;t++){ const y=oy+t*rowH+18, loops=t;
      ctx.strokeStyle='#46566a'; ctx.lineWidth=3; ctx.beginPath(); ctx.moveTo(x0,y); ctx.lineTo(x1,y); ctx.stroke();
      ctx.fillStyle='#fff'; ctx.beginPath(); ctx.arc(x0,y,5,0,7); ctx.fill(); ctx.beginPath(); ctx.arc(x1,y,5,0,7); ctx.fill();
      ctx.fillStyle='#7e8ea0'; ctx.font='11px monospace'; ctx.textAlign='center'; ctx.fillText('0',x0,y+18); ctx.fillText('x',x1,y+18);
      for(let b=0;b<loops;b++){ const bx=x0+(x1-x0)*(b+1)/(loops+1);
        ctx.strokeStyle='#e8590c'; ctx.lineWidth=2.5; ctx.beginPath(); ctx.arc(bx,y-9,9,0,7); ctx.stroke(); }
      ctx.textAlign='left'; ctx.fillStyle='#8497a8'; ctx.font='12px monospace';
      ctx.fillText(t===0?'bare (= Uₙ)':('+ '+t+' × π'), x1+16, y+4);
    }
    if(terms<5){ ctx.fillStyle='#7e8ea0'; ctx.font='15px sans-serif'; ctx.textAlign='left';
      ctx.fillText('+  …', x0, oy+terms*rowH+18); }
    document.getElementById('cap-lace').innerHTML='Schematic. Each ◯ is an <b style="color:#e8590c">irreducible self-avoidance correction</b> (a lace coefficient π). They are short-range — enumerate the small ones <em>once</em>, then slide them along by convolution, rather than recomputing the whole correction each length.';
  }
  document.getElementById('btn-lace-step').onclick=()=>{ terms=Math.min(terms+1,5); draw(); };
  document.getElementById('btn-lace-reset').onclick=()=>{ terms=1; draw(); };
  draw();
})();

/* ----------------------------------------------------------------- nav active */
(function(){
  const links=[...document.querySelectorAll('nav a')];
  const secs=links.map(a=>document.querySelector(a.getAttribute('href')));
  const obs=new IntersectionObserver(es=>{ es.forEach(e=>{ if(e.isIntersecting){
    const i=secs.indexOf(e.target); links.forEach(l=>l.classList.remove('active')); if(links[i])links[i].classList.add('active'); } }); },
    {rootMargin:'-30% 0px -60% 0px'});
  secs.forEach(s=>s&&obs.observe(s));
})();

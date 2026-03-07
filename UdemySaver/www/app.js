const $ = s => document.querySelector(s);
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

async function getJSON(url){
    try {
        const r = await fetch(url);
        const txt = await r.text();
        try { return JSON.parse(txt) } catch { return { ok:false, raw:txt } }
    } catch(err) { 
        return { ok:false, error:String(err) } 
    }
}

/* ===== i18n ===== */
let I18N = { lang: 'en', dict: {} };

function detectLang(){
    const saved = localStorage.getItem('cf_lang');
    if (saved) return saved;
    const nav = (navigator.language||'en').toLowerCase();
    return nav.startsWith('tr') ? 'tr' : 'en';
}

async function loadLang(lang) {
    try {
        const res = await fetch(`/www/lang/${lang}.json`);
        const text = await res.text();
        I18N.dict = JSON.parse(text);
        I18N.lang = lang;
        localStorage.setItem('cf_lang', lang);
    } catch (err) { 
        I18N.dict = {}; 
    }
    applyI18n();
}

function t(key, vars = {}) {
    let s = I18N.dict[key] || key;
    return s.replace(/\{(\w+)\}/g, (_, k) => vars[k] ?? `{${k}}`);
}

function applyI18n() {
    document.querySelectorAll('[data-i18n]').forEach(el => {
        const key = el.getAttribute('data-i18n'); if(!key) return;
        el.textContent = t(key);
    });
    const q = document.getElementById('q');
    if(q) q.setAttribute('placeholder', t('search.placeholder', {}));
    const sb = document.getElementById('searchBox');
    if (sb) sb.setAttribute('title', t('search.title'));
    const pageInfo = document.getElementById('pageInfo');
    if(pageInfo && window.state){ pageInfo.textContent = t('pager.page_fmt', {page: state.page, total: Math.max(1, Math.ceil((state.total||0)/state.page_size))}); }
    
    const qp = document.getElementById('qualityPill');
    if(qp) {
        const currentQ = preferredQuality();
        qp.textContent = t('quality.label', {q: getTranslatedQuality(currentQ)});
    }
}

/* ===== globals ===== */
const grid = $('#grid'), pager = $('#pager'), pageInfo = $('#pageInfo');
const prevBtn = $('#prevBtn'), nextBtn = $('#nextBtn'), refreshBtn = $('#refreshBtn');
const downloadSelectedBtn = $('#downloadSelectedBtn');
const authWarn = $('#authWarn'), authPill = $('#auth-pill');
const userbox = $('#userbox'), avatar = $('#avatar'), uname = $('#uname'), signOutBtn = $('#signOutBtn');
const countInfo = $('#countInfo'), qInput = $('#q');
const settingsBtn = $('#settingsBtn'), settingsPanel = $('#settingsPanel'), settingsCloseBtn = $('#settingsCloseBtn');
const optSubs = $('#optSubs'), optAssets = $('#optAssets'), optQuality = $('#optQuality');
let userOpts = {subs:false, assets:false};
const selectedCourses = new Map();

function updateSelectedUI(){
    if(downloadSelectedBtn)
        downloadSelectedBtn.style.display = selectedCourses.size > 0 ? '' : 'none';
}

function toggleCourseSelection(course, checked){
    if(checked) selectedCourses.set(course.id, course);
    else selectedCourses.delete(course.id);
    updateSelectedUI();
}

async function downloadSelected(){
    const tasks = [];
    for(const c of selectedCourses.values()){
        tasks.push(queueWholeCourse(c, preferredQuality()));
    }
    await Promise.all(tasks);
    selectedCourses.clear();
    updateSelectedUI();
    renderGrid();
}

function loadOpts(){
    if(optQuality) optQuality.value = preferredQuality();
}

async function saveOpts(){
    userOpts.subs = !!(optSubs?.checked);
    userOpts.assets = !!(optAssets?.checked);
    let quality = preferredQuality();

    if(optQuality){
        quality = optQuality.value;
        localStorage.setItem('cf_quality_pref', optQuality.value);
        toast(t('quality.label', {q: getTranslatedQuality(quality)}));
    }
    
    await fetch('/settings', {
        method:'POST',
        headers:{'Content-Type':'application/json'},
        body: JSON.stringify({
            download_subtitles: userOpts.subs,
            download_assets: userOpts.assets,
            quality
        })
    }).catch(()=>({}));
}

let state = { page:1, page_size:12, total:0, auth:false, items:[], filter:"" };

/* ===== utils ===== */
function imageOrPlaceholder(url){
    const label = t('img.placeholder');
    if(!url) return 'data:image/svg+xml;utf8,'+encodeURIComponent(
    `<svg xmlns="http://www.w3.org/2000/svg" width="480" height="270">
    <rect width="100%" height="100%" fill="#111111"/>
    <text x="50%" y="50%" fill="#333333" dominant-baseline="middle" text-anchor="middle" font-family="Arial" font-size="16" font-weight="bold">${label}</text>
    </svg>`
    );
    return url;
}

function fmtBytes(b) { 
    if (!isFinite(b) || b <= 0) return '0 B'; 
    const KB = 1024, MB = KB * 1024, GB = MB * 1024; 
    if (b >= GB) return (b / GB).toFixed(2) + ' GB'; 
    if (b >= MB) return (b / MB).toFixed(1) + ' MB'; 
    if (b >= KB) return Math.round(b / KB) + ' KB'; 
    return Math.round(b) + ' B';
}
function clamp01(x){ return Math.max(0, Math.min(1, x)); }
const pad3 = n => String(n).padStart(3,'0');
const safe = s => {
    let str = (s || "").replace(/[<>:"/\\|?*]+/g, '-').replace(/[\x00-\x1F\x7F]/g, '');
    return str.trim().replace(/^-+|-+$/g, '').substring(0, 120) || 'item';
};

function sanitizeFilename(name, fallback = 'item'){
    const raw = (name||"").trim();
    if(!raw) return fallback;
    const lastDot = raw.lastIndexOf('.');
    if(lastDot > 0 && lastDot < raw.length-1){
        const base = safe(raw.slice(0,lastDot));
        const ext = safe(raw.slice(lastDot+1));
        if(base && ext) return `${base}.${ext}`;
        if(base) return base;
        if(ext) return `${fallback}.${ext}`;
    }
    return safe(raw) || fallback;
}

function preferredQuality(){ return localStorage.getItem('cf_quality_pref') || '720'; }

function getTranslatedQuality(q) {
    if (q === 'Highest') return t('quality.highest');
    if (q === 'Lowest') return t('quality.lowest');
    return q; 
}

/* ===== token save ===== */
async function saveTokenFromUI(){
    const inp = document.getElementById('tokenInput');
    const msg = document.getElementById('authMsg');
    if(!inp) return;
    const token = (inp.value||"").trim();
    msg.textContent = '';
    if(!token){ msg.textContent = t('auth.token_empty'); return; }
    const r = await fetch('/settings', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({ udemy_access_token: token }) }).catch(e=>({ok:false, statusText:String(e)}));
    if(!r || !r.ok){ msg.textContent = t('auth.save_failed'); return; }
    const j = await r.json().catch(()=>({ok:false}));
    if(j && j.ok){ msg.textContent = t('auth.saved_reload'); setTimeout(()=> location.reload(), 1000); }
    else { msg.textContent = t('auth.error_prefix') + (j && j.error ? j.error : 'unknown'); }
}

/* ===== session ===== */
function toggleAuthUI() {
    const hero = document.querySelector('.hero');
    const queuePanel = document.getElementById('queue');
    const searchBox = document.getElementById('searchBox');
    const gridEl = document.getElementById('grid');
    
    if (state.auth) {
        if (hero) hero.style.display = '';
        if (gridEl) gridEl.style.display = ''; 
        if (queuePanel) queuePanel.style.display = '';
        if (searchBox) searchBox.style.display = '';
    } else {
        if (hero) hero.style.display = 'none';
        if (gridEl) gridEl.style.display = 'none';
        if (queuePanel) queuePanel.style.display = 'none';
        if (searchBox) searchBox.style.display = 'none';
    }
}

async function loadSession(){
    const data = await getJSON('/session');
    state.auth = !!data.auth;
    if (authPill) authPill.style.display = 'inline-flex';
    if(data.opts){
      userOpts.subs = !!data.opts.subs;
      userOpts.assets = !!data.opts.assets;
      if(optSubs) optSubs.checked = userOpts.subs;
      if(optAssets) optAssets.checked = userOpts.assets;
    }
    if(data.auth && data.user){
        if (userbox) userbox.style.display = 'flex';
        if (avatar) avatar.src = data.user.image_50x50 || data.user.image_100x100 || '';
        if (uname) uname.textContent = data.user.display_name || data.user.title || 'User';
        if (authWarn) authWarn.style.display = 'none';
        if (signOutBtn) signOutBtn.style.display = 'inline-flex';
    } else {
        if (userbox) userbox.style.display = 'none';
        if (authWarn) authWarn.style.display = 'block';
        if (signOutBtn) signOutBtn.style.display = 'none';
    }
    toggleAuthUI();
}

async function signOut(){
    const r = await fetch('/settings', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({ udemy_access_token: '' }) }).catch(()=>({ok:false}));
    if(r && r.ok){ location.reload(); }
}

async function setLang(lang){
    await loadLang(lang);
    applyI18n();
    if (countInfo) countInfo.textContent = state.total ? `(${state.total} ${t('library.count_suffix')})` : '';
    renderGrid();     
    renderPager();     
    qTick(true);       
}

/* ===== courses ===== */
async function loadPage(p=1){
    state.page = Math.max(1, p|0);
    const q = state.filter ? encodeURIComponent(state.filter) : '';
    const data = await getJSON(`/courses?page=${state.page}&page_size=${state.page_size}&search=${q}`);

    state.total = data.total || 0;
    state.auth = !!data.auth;
    state.items = Array.isArray(data.courses) ? data.courses : (data.results || []);

    toggleAuthUI();

    if (countInfo) countInfo.textContent = state.total ? `${state.total} ${t('library.count_suffix')}` : '';
    
    if (state.auth) {
        renderGrid(); 
        renderPager();
    }
}

/* ===== courses ===== */
function renderGrid(){
    if(!grid) return; 
    grid.innerHTML = '';
    let items = state.items;
    
    if(items.length === 0){ 
        grid.innerHTML = `<div class="empty">${t('grid.empty')}</div>`; 
        return; 
    }

    const fragment = document.createDocumentFragment();

    for(const c of items){
        const el = document.createElement('div');
        el.className = 'card';
        el.innerHTML = `
        <div class="thumb-wrap">
            <input type="checkbox" class="select-cb" data-id="${c.id}" ${selectedCourses.has(c.id)?'checked':''}>
            <div class="thumb-overlay"></div>
            <img class="thumb" loading="lazy" src="${imageOrPlaceholder(c.image)}" alt="">
            <span class="badge"><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2"></polygon></svg> ${c.id}</span>
        </div>
        <div class="body">
            <h3 class="title" title="${c.title||t('misc.course')}">${c.title||t('misc.course')}</h3>
            <div class="inst">
                <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"></path><circle cx="12" cy="7" r="4"></circle></svg>
                ${c.instructor||'Instructor'}
            </div>
            <div class="row">
                <button class="btn primary dl-btn" data-id="${c.id}">
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path><polyline points="7 10 12 15 17 10"></polyline><line x1="12" y1="15" x2="12" y2="3"></line></svg>
                    ${t('card.download')}
                </button>
                <a class="btn ghost" href="${c.url||'#'}" target="_blank" rel="noopener">
                    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"></path><polyline points="15 3 21 3 21 9"></polyline><line x1="10" y1="14" x2="21" y2="3"></line></svg>
                </a>
            </div>
        </div>`;
        
        fragment.appendChild(el); 
    }
    
    grid.appendChild(fragment);
    updateSelectedUI();
}

if (!grid.dataset.listenerAttached) {
    grid.addEventListener('click', (e) => {
        const dlBtn = e.target.closest('.dl-btn');
        if (dlBtn) {
            const courseId = parseInt(dlBtn.getAttribute('data-id'), 10);
            const course = state.items.find(c => c.id === courseId);
            if (course) queueWholeCourse(course, preferredQuality());
            return;
        }
        
        const cb = e.target.closest('.select-cb');
        if (cb) {
            const courseId = parseInt(cb.getAttribute('data-id'), 10);
            const course = state.items.find(c => c.id === courseId);
            if (course) toggleCourseSelection(course, cb.checked);
        }
    });
    grid.dataset.listenerAttached = 'true';
}

function renderPager(){
    if(!pager||!pageInfo) return;
    const totalPages = Math.max(1, Math.ceil((state.total||0)/state.page_size));
    pager.style.display = totalPages>1 ? 'flex' : 'none';
    pageInfo.textContent = t('pager.page_fmt', {page: state.page, total: totalPages});
    if(prevBtn) prevBtn.disabled = state.page<=1;
    if(nextBtn) nextBtn.disabled = state.page>=totalPages;
}

/* ===== busy pill ===== */
let __busy = 0; const qPill = $('#qPill');
function showBusy(msgKey='queue.collecting'){
    __busy++;
    if(qPill){ qPill.style.display=''; qPill.classList.add('loading'); }
    const e=$('#qRunning');
    if(e) e.innerHTML = `<i class="spin"></i>${t(msgKey)}`;
}
function setBusyTextTextual(text, subtext = ""){
    const e = $('#qRunning');
    if(e){
        if(qPill) qPill.style.display='';
        let html = `<i class="spin"></i> ${text}`;
        if(subtext) {
            if (subtext.length > 35) subtext = subtext.substring(0, 32) + '...';
            html += ` <span style="opacity: 0.7; margin-left: 5px; font-weight: normal;"> - ${subtext}</span>`;
        }
        e.innerHTML = html;
    }
}
function hideBusy(){
    __busy=Math.max(0,__busy-1);
    if(__busy===0){
        if(qPill){ qPill.classList.remove('loading'); qPill.style.display='none'; }
        const e=$('#qRunning');
        if(e) e.textContent=t('queue.waiting');
    }
}

/* ===== pick source ===== */
function pickVideoSource(asset, preference="720"){
    if(!asset) return null;
    let list = (asset.stream_urls && asset.stream_urls.Video) ? [...asset.stream_urls.Video] : [];
    if(asset.download_urls && asset.download_urls.Video){
        for(let v of asset.download_urls.Video){
            if(v.file && !list.find(x => x.file === v.file)) {
                list.push({ type: 'video/mp4', label: v.label || '720', file: v.file });
            }
        }
    }
    const mp4s = list.filter(x=>x && x.type==='video/mp4' && x.file).map(x=>({raw:x, quality:parseInt(x.label,10)||0}));
    mp4s.sort((a,b)=>a.quality-b.quality);
    const lowestMp4 = mp4s.length ? mp4s[0] : null;
    const bestMp4 = mp4s.length ? mp4s[mp4s.length-1] : null;

    const buildHls = ()=>{
        const hlsEntry = list.find(x=>x && x.type==='application/x-mpegURL' && x.file);
        if(hlsEntry) return {type:'hls', label:hlsEntry.label||'Auto', url:hlsEntry.file};
        if(asset.hls_url) return {type:'hls', label:'Auto', url:asset.hls_url};
        if(Array.isArray(asset.media_sources)){
            for(const m of asset.media_sources){
                if(!m) continue;
                const src = m.src || m.file || m.url;
                if(src) return {type:'hls', label:m.label||m.quality||'Auto', url:src};
            }
        }
        return null;
    };

    const hlsSource = buildHls();
    const prefValue = /^\d+$/.test(preference) ? parseInt(preference,10) : null;
    const highestMp4 = bestMp4 ? bestMp4.quality : 0;
    let preferHls = false;
    
    if(hlsSource){
        if(preference==='Highest') preferHls = true;
        else if(prefValue && (prefValue >= 1080 || (highestMp4>0 && prefValue>highestMp4))) preferHls = true;
    }

    if(preferHls && hlsSource) return hlsSource;
    if(mp4s.length){
        let chosen = bestMp4;
        if(preference==='Lowest' && lowestMp4) chosen = lowestMp4;
        else if(prefValue){
            const exact = mp4s.find(x=>x.quality===prefValue) || mp4s.find(x=>String(x.raw.label)===String(preference));
            if(exact) chosen = exact;
            else{
                const higherOrEqual = mp4s.find(x=>x.quality>=prefValue);
                if(higherOrEqual) chosen = higherOrEqual;
            }
        }
        return {type:'mp4', label:chosen.raw.label, url:chosen.raw.file};
    }
    if(hlsSource) return hlsSource;
    return null;
}

/* ===== enqueue ===== */
async function enqueueLecture(course, lecture, idxInCourse, pref="720", opts={}){ 
    const asset = lecture && lecture.asset; 
    if(!asset || asset.asset_type!=='Video') return {skipped:true}; 
    const picked = pickVideoSource(asset, pref); 
    if(!picked) return {skipped:true}; 

    const base = { 
        course_id:course.id, course_title:course.title, 
        section_index:lecture.section_index||0, section_title:lecture.section_title||'', 
        lecture_index:idxInCourse, lecture_title:lecture.title||'', 
        lecture_id: lecture.id 
    }; 

    const videoPayload = {...base, url:picked.url, filename:`${pad3(idxInCourse)} - ${safe(lecture.title)}-${picked.label}.mp4`, is_video: true, quality: pref};
    
    const queueTasks = [];
    
    queueTasks.push(
        fetch('/queue',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(videoPayload)})
            .then(r => r.json()).catch(()=>({ok:false}))
    );

    if(opts.subs && Array.isArray(asset.captions)){ 
        for(const cap of asset.captions){ 
            const url = cap.url || cap.file || cap.src; if(!url) continue; 
            const lang = safe(cap.language || cap.label || 'sub'); 
            const ext = (url.split(/[#?]/)[0].split('.').pop()||'vtt'); 
            const p = {...base, url, filename:`${pad3(idxInCourse)} - ${safe(lecture.title)}.${lang}.${ext}`}; 
            
            queueTasks.push(fetch('/queue',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)}));
        } 
    } 

    if(opts.assets && Array.isArray(lecture.supplementary_assets)){
        for(const a of lecture.supplementary_assets){
            const name = a.filename ? a.filename : `${safe(a.title||'asset')}`;
            let url = '';
            if(a.download_urls){
                for(const k in a.download_urls){
                    const arr = a.download_urls[k];
                    if(Array.isArray(arr) && arr.length){
                        url = arr[0].file || arr[0].url || '';
                        if(url) break;
                    }
                }
            }
            if(!url) continue;
            const p = {...base, filename:`${pad3(idxInCourse)} - ${safe(lecture.title)} - ${sanitizeFilename(name)}`, asset_id:a.id, url};
            
            queueTasks.push(fetch('/queue',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)}));
        }
    }
    
    const results = await Promise.all(queueTasks);
    const videoData = results[0]; 
    
    if(videoData && videoData.skipped){
        if(videoData.reason==='exists') toast(t('toast.exists',{title: lecture.title}));
        else if(videoData.reason==='queued') toast(t('toast.queued',{title: lecture.title}));
    }

    return videoData;
}

async function queueWholeCourse(course, preference="720"){ 
    const opts = {subs: userOpts.subs, assets: userOpts.assets};
    showBusy('queue.collecting'); 
    try{ 
        toast(t('toast.collecting',{title: course.title||'Course'})); 
        let page=1, knownTotal=null, seen=0, added=0, skipped=0, exists=0; 
        let retryCount = 0;

        while(true){ 
            const j = await getJSON(`/lectures?course_id=${course.id}&page=${page}`); 
            if (!j || j.ok === false) {
                if (retryCount < 3) {
                    retryCount++;
                    setBusyTextTextual(`Error, try again -> (${retryCount}/3)...`, "API Rate");
                    await delay(3000 * retryCount); 
                    continue;
                } else {
                    toast("Udemy disconnected. Please wait and try again.");
                    break;
                }
            }
            retryCount = 0;
            if(knownTotal==null) knownTotal = (j && Number.isFinite(j.count)) ? (j.count|0) : null; 
            const chunk = Array.isArray(j.results) ? j.results : []; 
            if(chunk.length===0) break; 
            
            for(const lec of chunk){ 
                seen++; 
                setBusyTextTextual(`${seen}/${knownTotal ?? '…'}`, lec.title || 'Section'); 
                if(!lec || !lec.asset || lec.asset.asset_type!=='Video'){ skipped++; continue; } 
                const res = await enqueueLecture(course, lec, seen, preference, opts); 
                if(res && res.skipped && res.reason==='exists') exists++; 
                else if(res && res.ok) added++; 
                else skipped++; 
                if(seen%5===0) qTick(true); 
                await delay(50);
            } 
            if(j.next) { page++; setBusyTextTextual(`${seen}/${knownTotal ?? '…'}`, "Loading..."); await delay(1500); } else break; 
        } 
        qTick(true); 
        toast(t('toast.added_summary', {added, seen, skipped, exists})); 
        if(added===0 && exists>0 && skipped===0) toast(t('toast.course_done',{title: course.title})); 
        ensureCourseSize(course.id, preference); 
    } catch(err) {
        toast("An unexpected error occurred.");
    } finally { 
        hideBusy(); 
    }
}

/* ===== course total size (lazy) ===== */
const __courseSize = new Map();
async function estimateCourseSize(courseId, quality){ return await getJSON(`/estimate?course_id=${courseId}&quality=${encodeURIComponent(quality||'720')}`); }
async function ensureCourseSize(courseId, quality){ const entry = __courseSize.get(courseId); if(entry && (entry.pending || entry.bytes>0)) return; __courseSize.set(courseId,{bytes:0,pending:true}); const j = await estimateCourseSize(courseId, quality); __courseSize.set(courseId,{bytes:(j && j.total_bytes)||0,pending:false}); }

/* ===== queue render ===== */
async function qTick(force=false){
    if(!force && document.hidden) return;
    const data = await getJSON('/queue');

    const byCourse = new Map();
    if(Array.isArray(data.courses)){
        for(const c of data.courses){
            const done=c.done|0, total=c.total|0;
            const cTitle = c.title && c.title !== 'Course' ? c.title : t('misc.course'); 
            byCourse.set(c.course_id,{ 
                course_id:c.course_id, title:cTitle, done, total, 
                state:(total>0&&done>=total)?'done':'queued', 
                pct: total>0 ? Math.round((done*100)/total) : 0, 
                _r:-1, active_files: [] 
            });
        }
    }
    
    if(Array.isArray(data.items)){
        const rank={downloading:4, failed:3, paused:2, queued:1, done:0};
        for(const it of data.items){
            const row = byCourse.get(it.course_id); if(!row) continue;
            const st=String(it.state||'').toLowerCase(), r=rank[st]??0;
            if(r>row._r){ row.state=st||'queued'; row._r=r; }
            if(st==='downloading'){
                const spd = it.speed_bps || 0; 
                row.speed = (row.speed||0) + spd;
                if(it.filename) row.active_files.push(it.filename);
            }
        }
    }
    
    for(const row of byCourse.values()){
        if(!__courseSize.has(row.course_id)) ensureCourseSize(row.course_id, preferredQuality());
    }

    const qList = $('#qList'); if(!qList) return;
    if(byCourse.size===0){ 
        qList.innerHTML=`<div class="q-empty">${t('queue.empty')}</div>`; 
    } else {
        const rows = [...byCourse.values()].map(row=>{
            const pct = Math.round(clamp01(row.total? row.done/row.total : 0)*100) || row.pct || 0;
            const sz = __courseSize.get(row.course_id);
            const sizeTxt = sz && sz.bytes>0 ? ` • ${fmtBytes(sz.bytes)}` : '';
            const speedTxt = row.speed>0 ? ` • ${t('queue.speed', {speed: fmtBytes(row.speed)})}` : '';
            
            let activeFileHtml = '';
            if (row.active_files.length > 0) {
                let fileName = row.active_files[0];
                if (fileName.length > 40) fileName = fileName.substring(0, 37) + '...';
                let extra = row.active_files.length > 1 ? ` (+${row.active_files.length - 1})` : '';
                activeFileHtml = `<br><span class="active-file-text"><i class="spin" style="margin-right:4px;"></i> ${t('queue.active_file', {file: fileName + extra})}</span>`;
            }
            if (row.state === 'failed') {
                activeFileHtml = `<br><span style="color: var(--danger); font-size: 11px; margin-top: 6px; display: inline-block; font-weight: 700;">⚠️ ${t('queue.failed')}</span>`;
            }

            let finalSub = `${row.done} / ${row.total} ${t('queue.sections')} • %${pct}${sizeTxt}${speedTxt}${activeFileHtml}`;
            if (row.state === 'done') {
                finalSub = `<span style="color: #4ade80; font-weight: bold;">${t('queue.done_file')}</span> • ${row.total} ${t('queue.sections')}${sizeTxt}`;
            }

            const btn = row.state === 'paused'
                ? `<button class="btn sm" data-act="resume" data-cid="${row.course_id}"><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="5 3 19 12 5 21 5 3"></polygon></svg> ${t('actions.resume')}</button>`
                : `<button class="btn sm" data-act="pause" data-cid="${row.course_id}"><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="6" y="4" width="4" height="16"></rect><rect x="14" y="4" width="4" height="16"></rect></svg> ${t('actions.pause')}</button>`;

            return `
            <div class="q-item${row.state==='done' ? ' done' : ''}" data-cid="${row.course_id}">
                <div>
                    <div class="q-title">${row.title}</div>
                    <div class="q-sub">${finalSub}</div>
                    <div class="bar"><i style="width:${pct}%;"></i></div>
                </div>
                <div class="q-ctrl">${btn}</div>
            </div>`;
        }).join('');
        
        qList.innerHTML = rows;

        qList.querySelectorAll('button[data-act]').forEach(btn=>{
            btn.addEventListener('click', async ()=>{
                const cid = btn.getAttribute('data-cid');
                const act = btn.getAttribute('data-act');
                fetch(`/queue/${act}`, { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({course_id:Number(cid)}) }).catch(()=>{});
                qTick(true);
            });
        });
    }

    if(__busy===0){
        const run = $('#qRunning');
        if(run && qPill){
            let statusKey = 'queue.waiting';
            let isDownloading = false;
            let hasFailed = false;
            let allDone = byCourse.size > 0;

            for(const row of byCourse.values()){
                if(row.state !== 'done') allDone = false;
                if(row.state==='downloading'){ statusKey='queue.downloading'; isDownloading=true; break; }
                if(row.state==='failed'){ statusKey='queue.failed'; hasFailed = true; }
                if(row.state==='paused' && !hasFailed){ statusKey='queue.paused'; }
            }

            if (allDone) {
                run.innerHTML = `<span style="color: #4ade80; font-weight: 800;">${t('queue.done_all')}</span>`;
            } else {
                run.innerHTML = isDownloading ? `<i class="spin"></i>${t(statusKey)}` : t(statusKey);
            }
            qPill.style.display='';
        }
    }
}

/* ===== tiny toast ===== */
let toastTimer=null; function toast(msg){ let t=document.getElementById('cf_toast'); if(!t){ t=document.createElement('div'); t.id='cf_toast'; t.style.cssText='position:fixed;left:50%;transform:translateX(-50%);bottom:24px;background:rgba(15,15,20,0.9);backdrop-filter:blur(10px);border:1px solid rgba(255,255,255,.08);box-shadow:0 10px 30px rgba(0,0,0,.5);padding:12px 18px;border-radius:12px;color:#fff;font-size:13px;font-weight:700;z-index:9999;transition:0.3s;'; document.body.appendChild(t); } t.textContent=msg; t.style.opacity='1'; t.style.transform='translateX(-50%) translateY(0)'; clearTimeout(toastTimer); toastTimer=setTimeout(()=>{t.style.opacity='0'; t.style.transform='translateX(-50%) translateY(10px)';},2000); }

/* ===== events & boot ===== */
if(prevBtn) prevBtn.addEventListener('click',()=>loadPage(state.page-1));
if(nextBtn) nextBtn.addEventListener('click',()=>loadPage(state.page+1));
if(refreshBtn) refreshBtn.addEventListener('click',()=>loadPage(state.page));
if(downloadSelectedBtn) downloadSelectedBtn.addEventListener('click', downloadSelected);
let searchTimer = null;
if(qInput) qInput.addEventListener('input', () => { 
    state.filter = (qInput.value || '').trim();
    clearTimeout(searchTimer);
    searchTimer = setTimeout(() => { loadPage(1); }, 500);
});
if(signOutBtn) signOutBtn.addEventListener('click', signOut);
if(settingsBtn && settingsPanel) settingsBtn.addEventListener('click', ()=>{ settingsPanel.style.display = (settingsPanel.style.display==='none' || settingsPanel.style.display==='') ? 'block' : 'none'; });
if(settingsCloseBtn && settingsPanel) settingsCloseBtn.addEventListener('click', ()=>{ settingsPanel.style.display='none'; });
if(optSubs) optSubs.addEventListener('change', saveOpts);
if(optAssets) optAssets.addEventListener('change', saveOpts);
if(optQuality) optQuality.addEventListener('change', saveOpts);

document.addEventListener('visibilitychange', ()=>{ if(!document.hidden) qTick(true); });
window.addEventListener('load', ()=>{ const btn = document.getElementById('saveTokenBtn'); if(btn) btn.addEventListener('click', saveTokenFromUI); });

const langSelect = document.getElementById('langSelect');
if (langSelect) {
  langSelect.addEventListener('change', e => { setLang(e.target.value); });
  window.addEventListener('load', () => { langSelect.value = detectLang(); });
}

window.addEventListener('load', async ()=>{
  await loadLang(detectLang());
  await loadSession();        
  await loadPage(1);   
  loadOpts();
  applyI18n(); 
  qTick(true);
  setInterval(()=>qTick(false),1500);
});

window.queueWholeCourse = queueWholeCourse;
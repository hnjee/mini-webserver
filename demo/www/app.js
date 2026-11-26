// 시연 페이지 동작. 상태 코드·시간·머리·본문은 모두 이 브라우저가 실제로 받은 응답에서 읽는다.
"use strict";

var $ = function (id) { return document.getElementById(id); };
var reqNo = 0;
var actNo = 0;
var LOG_MAX = 20;        // 최근 활동 수(메뉴 불러오기 1회 = 활동 1개)
var activities = [];     // 이 페이지를 연 뒤 서비스 체험 탭에서 일어난 활동, 오래된 것부터
var selectedReq = null;  // 통신 보기에서 고른 요청 기록(객체 자체로 식별)
var lastMenuBy = {};     // 인스턴스별 마지막 메뉴 JSON 문자열
var seen = [];           // 최근 메뉴 응답 인스턴스

function el(tag, cls, text) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
}
function clock(d) { return d.toLocaleTimeString("en-GB"); }
function clockMs(d) { return clock(d) + "." + String(d.getMilliseconds()).padStart(3, "0"); }
function ms(r) { return Math.round(r.end - r.start) + " ms"; }

// 서비스 체험 활동을 기록에 추가한다. 가장 오래된 것부터 버려 LOG_MAX개를 유지한다.
function newActivity(type) {
  actNo += 1;
  var act = { id: "act-" + actNo, type: type, startAt: new Date(), reqs: [] };
  activities.push(act);
  while (activities.length > LOG_MAX) {
    var gone = activities.shift();
    if (selectedReq && gone.reqs.indexOf(selectedReq) >= 0) selectedReq = null;
  }
  return act;
}

// 요청 하나를 보내고 기록한다. sent는 페이지 JS가 직접 넣은 머리만 담는다.
// act가 있으면 그 활동의 기록에 시작 즉시 넣고, 끝나면 같은 객체를 채운다.
function send(kind, url, init, act) {
  init = init || {};
  reqNo += 1;
  var id = "req-" + reqNo;
  var sent = Object.assign({ "X-Demo-Trace": id }, init.headers || {});
  var body = init.body != null ? new TextEncoder().encode(init.body) : null;
  var rec = { id: id, kind: kind, method: init.method || "GET", url: url, sent: sent, sentBytes: body,
              start: performance.now(), startAt: new Date(), pending: true };
  if (act) { act.reqs.push(rec); refreshNetwork(); }
  return fetch(url, { method: rec.method, headers: sent, body: body, cache: "no-store" }).then(function (res) {
    rec.status = res.status;
    rec.statusText = res.statusText;
    rec.headers = Array.from(res.headers.entries());
    return res.arrayBuffer();
  }).then(function (buf) {
    rec.body = new Uint8Array(buf);
    return rec;
  }, function (err) {
    var msg = String(err && err.message || err);
    // 머리를 받기 전 실패는 error, 머리를 받은 뒤 본문을 끝까지 못 받으면 bodyError(상태 코드는 그대로 둔다)
    if (rec.status == null) rec.error = msg;
    else { rec.bodyError = msg; rec.body = null; }
    return rec;
  }).then(function (r) {
    r.end = performance.now();
    r.at = new Date();
    r.pending = false;
    // 같은 URL이 겹칠 수 있으므로 이 요청의 시작 시각 이후에 시작한 가장 이른 항목을 쓴다.
    var t = performance.getEntriesByName(new URL(url, location.href).href)
      .filter(function (e) { return e.startTime >= r.start - 2 && e.startTime <= r.end; })
      .sort(function (a, b) { return a.startTime - b.startTime; })[0];
    r.protocol = t && t.nextHopProtocol || "";
    r.json = null;
    if (r.body) { try { r.json = JSON.parse(new TextDecoder().decode(r.body)); } catch (e) { r.json = null; } }
    return r;
  });
}

// 실행 중에는 버튼을 막고 경과 시간을 버튼 옆 출력에 보인다.
function busy(button, out, label) {
  var t0 = performance.now();
  button.disabled = true;
  button.setAttribute("aria-busy", "true");
  var tick = function () { out.textContent = label + " " + ((performance.now() - t0) / 1000).toFixed(1) + "초"; };
  tick();
  var timer = setInterval(tick, 100);
  return function () { clearInterval(timer); button.disabled = false; button.removeAttribute("aria-busy"); };
}

function statusText(r) {
  if (r.status == null) return "응답 없음 (" + r.error + ")";
  return r.status + " " + r.statusText + (r.bodyError ? " · 본문 수신 실패" : "");
}

/* ---------- 탭 ---------- */
var tabs = Array.from(document.querySelectorAll('[role="tab"]'));
function selectTab(tab, focus) {
  tabs.forEach(function (t) {
    var on = t === tab;
    t.setAttribute("aria-selected", String(on));
    t.tabIndex = on ? 0 : -1;
    $(t.getAttribute("aria-controls")).hidden = !on;
  });
  if (focus) tab.focus();
  window.history.replaceState(null, "", "#" + tab.id.slice(4));
  if (tab.id === "tab-network") {
    if (!selectedReq && activities.length) selectedReq = activities[activities.length - 1].reqs[0];
    renderNetwork();
  }
}
tabs.forEach(function (t, i) {
  t.addEventListener("click", function () { selectTab(t); });
  t.addEventListener("keydown", function (e) {
    var n = { ArrowRight: i + 1, ArrowLeft: i - 1, Home: 0, End: tabs.length - 1 }[e.key];
    if (n == null) return;
    e.preventDefault();
    selectTab(tabs[(n + tabs.length) % tabs.length], true);
  });
});

/* ---------- 1. 서비스 체험 ---------- */
var selectedId = null;

function renderMenu(items) {
  var list = $("menu-list");
  list.replaceChildren();
  items.forEach(function (item) {
    var li = el("li");
    var b = el("button", "item");
    b.type = "button";
    b.setAttribute("aria-pressed", String(item.id === selectedId));
    var img = el("img");
    img.alt = "";
    img.src = imageUrls[item.image] || item.image;
    var txt = el("span", "item-text");
    txt.appendChild(el("b", null, item.name));
    txt.appendChild(el("span", "price", item.price.toLocaleString("ko-KR") + "원"));
    if (item.id === selectedId) txt.appendChild(el("span", "desc", item.desc));
    b.append(img, txt);
    b.addEventListener("click", function () { selectedId = item.id === selectedId ? null : item.id; renderMenu(items); });
    li.appendChild(b);
    list.appendChild(li);
  });
}

var imageUrls = {};
// 메뉴 사진도 fetch로 받아 상태와 머리를 기록한다(정적 파일 경로).
function loadImages(items, act) {
  return Promise.all(items.map(function (item) {
    return send("image", item.image, null, act).then(function (r) {
      var type = (r.headers || []).find(function (h) { return h[0] === "content-type"; });
      if (!failed(r) && r.body) {
        if (imageUrls[item.image]) URL.revokeObjectURL(imageUrls[item.image]);
        imageUrls[item.image] = URL.createObjectURL(new Blob([r.body], { type: type ? type[1] : "" }));
      }
      r.body = null; // 기록에는 이미지 바이트를 남기지 않는다
      refreshNetwork();
      return r;
    });
  })).then(function (rs) {
    var ok = rs.filter(function (r) { return !failed(r); }).length;
    $("static-line").textContent = "사진 " + rs.length + "장은 mini-webserver가 정적 파일로 보냈다 · 정상 수신 " + ok + "/" + rs.length;
  });
}

$("load-menu").addEventListener("click", function () {
  var button = this;
  var empty = $("menu-empty");
  empty.hidden = false;
  var done = busy(button, empty, "메뉴 받는 중…");
  var act = newActivity("menu");
  send("menu", "/api/menu", null, act).then(function (r) {
    var ok = r.status === 200 && r.json && Array.isArray(r.json.menu);
    var inst = ok ? r.json.instance : null;
    document.querySelectorAll(".inst").forEach(function (n) { n.classList.toggle("on", n.dataset.inst === inst); });
    seen.push(inst || String(r.status || "×"));
    if (seen.length > 8) seen.shift();
    var hist = $("history");
    hist.replaceChildren();
    hist.appendChild(el("li", "h-label", "최근 순서"));
    seen.forEach(function (h) { hist.appendChild(el("li", "h-" + h, h)); });

    if (!ok) {
      done();
      $("menu-list").replaceChildren();
      $("src-line").hidden = true;
      $("same-line").textContent = "";   // 이전 성공 결과를 남기지 않는다
      $("static-line").textContent = "";
      empty.textContent = "메뉴를 받지 못했다 · " + statusText(r);
      empty.classList.add("err");
      $("who-line").textContent = statusText(r) + " · " + ms(r) + " · " + clock(r.at);
      refreshNetwork();
      return;
    }
    empty.classList.remove("err");
    var menuText = JSON.stringify(r.json.menu);
    var other = inst === "A" ? "B" : "A";
    lastMenuBy[inst] = menuText;
    $("who-line").innerHTML = "";
    $("who-line").append(el("b", "inst-" + inst, inst + "가 응답했다"), " · " + statusText(r) + " · " + ms(r) + " · " + clock(r.at));
    $("same-line").textContent = lastMenuBy[other] == null
      ? "한 번 더 불러오면 다른 인스턴스와 메뉴를 비교한다."
      : (lastMenuBy[other] === menuText ? "A가 줘도 B가 줘도 메뉴 " + r.json.menu.length + "개가 똑같다." : "A와 B가 준 메뉴가 다르다.");
    button.textContent = "새로고침";
    refreshNetwork();
    return loadImages(r.json.menu, act).then(function () {
      done();
      empty.hidden = true;
      var src = $("src-line");
      src.hidden = false;
      src.replaceChildren("메뉴 정보 ", el("b", "inst-" + inst, "백엔드 " + inst), " · 사진 ", el("b", null, "mini-webserver"));
      renderMenu(r.json.menu);
    });
  });
});

$("send-memo").addEventListener("click", function () {
  var out = $("memo-out");
  var done = busy(this, out, "보내는 중…");
  var act = newActivity("memo");
  send("memo", "/echo", { method: "POST", body: $("memo-in").value, headers: { "Content-Type": "text/plain; charset=utf-8" } }, act).then(function (r) {
    done();
    if (!failed(r) && r.body) {
      r.same = r.sentBytes.length === r.body.length && r.sentBytes.every(function (x, i) { return x === r.body[i]; });
    }
    refreshNetwork();
    if (failed(r) || !r.body) { out.textContent = "실패 · " + statusText(r); return; }
    var a = r.sentBytes, b = r.body, same = r.same;
    out.replaceChildren(el("q", null, new TextDecoder().decode(b)),
      el("span", "fine", " 돌려받음 · 보낸 " + a.length + "바이트와 받은 " + b.length + "바이트가 " + (same ? "모두 같다" : "다르다")));
  });
});

/* ---------- 2. 통신 보기 (관찰 전용: 여기서는 네트워크 요청을 보내지 않는다) ---------- */
function refreshNetwork() { if (!$("panel-network").hidden) renderNetwork(); }

var ROUTE = { menu: "proxy", image: "static", memo: "echo" };
var ACT_NAME = { menu: "메뉴 불러오기", memo: "메모 보내기" };

function failed(r) { return !r.pending && (r.status == null || r.status >= 400 || !!r.bodyError); }
function shortStatus(r) { return r.pending ? "진행 중" : r.status == null ? "응답 없음" : String(r.status); }

function kv(rows, cls) {
  var dl = el("dl", "kv " + (cls || ""));
  rows.forEach(function (row) {
    dl.appendChild(el("dt", null, row[0]));
    // "a; b" 형태 값은 "; " 뒤에서만 줄을 바꾼다(20자 이하 조각은 안에서 꺾지 않는다)
    var dd = el("dd");
    String(row[1]).split("; ").forEach(function (part, i, all) {
      dd.appendChild(el("span", part.length <= 20 ? "nw" : null, part + (i < all.length - 1 ? ";" : "")));
      if (i < all.length - 1) dd.append(" ");
    });
    dl.appendChild(dd);
  });
  return dl;
}
// 목록의 요청 한 줄. 누르면 그 기록을 고른다(네트워크 요청 없음).
function reqRow(r) {
  var b = el("button", "req" + (failed(r) ? " is-bad" : "") + (r.pending ? " is-pending" : ""));
  b.type = "button";
  b.dataset.id = r.id;
  b.setAttribute("aria-pressed", String(r === selectedReq));
  b.appendChild(el("span", "req-status", shortStatus(r)));
  var main = el("span", "req-main");
  main.appendChild(el("code", null, r.method + " " + r.url));
  var meta = clockMs(r.startAt) + (r.pending ? "" : " · " + ms(r)) + (r.bodyError ? " · 본문 수신 실패" : "");
  if (r.kind === "menu" && r.json && r.json.instance) meta += " · 백엔드 " + r.json.instance;
  main.appendChild(el("span", "req-meta", meta));
  b.appendChild(main);
  b.addEventListener("click", function () {
    // 목록은 다시 만들지 않고 선택 표시만 바꾼다(포커스가 이 버튼에 남는다)
    selectedReq = r;
    document.querySelectorAll("#log .req").forEach(function (x) { x.setAttribute("aria-pressed", String(x.dataset.id === r.id)); });
    renderDetail();
    // 한 열로 쌓인 화면에서는 상세가 스트립 아래에 있으니 그쪽으로 옮긴다
    var areas = document.querySelector(".areas");
    if (areas && getComputedStyle(areas).display === "flex") $("exchange").scrollIntoView({ block: "start" });
  });
  return b;
}

function renderLog() {
  var list = $("log");
  // 새 기록이 들어와 목록을 다시 만들 때: 포커스·펼친 사진 묶음·스크롤 위치를 요청/활동 ID로 되살린다
  var active = list.contains(document.activeElement) ? document.activeElement : null;
  var focusId = active && active.classList.contains("req") ? active.dataset.id : null;
  var focusAct = active && active.tagName === "SUMMARY" ? active.parentNode.dataset.act : null;
  var openActs = Array.from(list.querySelectorAll("details.imgs[open]")).map(function (d) { return d.dataset.act; });
  var scroller = list.closest(".log-col"), top = scroller ? scroller.scrollTop : 0;
  list.replaceChildren();
  $("log-empty").hidden = activities.length > 0;
  $("log-count").textContent = activities.length + "건";
  activities.slice().reverse().forEach(function (act) {
    var li = el("li", "act");
    var head = el("p", "act-head");
    head.append(el("b", null, ACT_NAME[act.type]), " · " + clock(act.startAt));
    li.appendChild(head);
    var main = act.reqs.filter(function (r) { return r.kind !== "image"; });
    var imgs = act.reqs.filter(function (r) { return r.kind === "image"; });
    main.forEach(function (r) { li.appendChild(reqRow(r)); });
    if (imgs.length) {
      var d = el("details", "imgs");
      d.dataset.act = act.id;
      // 고른 요청이 사진이거나 이미 펼쳐 둔 묶음이면 펼친 채로 둔다
      d.open = imgs.indexOf(selectedReq) >= 0 || openActs.indexOf(act.id) >= 0;
      var bad = imgs.filter(failed).length, wait = imgs.filter(function (r) { return r.pending; }).length;
      d.appendChild(el("summary", null, "사진 요청 " + imgs.length + "건" +
        (wait ? " · 진행 중 " + wait : bad ? " · 실패 " + bad : " · 모두 " + shortStatus(imgs[0]))));
      imgs.forEach(function (r) { d.appendChild(reqRow(r)); });
      li.appendChild(d);
    }
    list.appendChild(li);
  });
  if (scroller) scroller.scrollTop = top;
  var again = focusId ? list.querySelector('.req[data-id="' + focusId + '"]') :
              focusAct ? list.querySelector('details.imgs[data-act="' + focusAct + '"] > summary') : null;
  if (again) again.focus({ preventScroll: true });
}

var NUM = ["", "①", "②", "③", "④", "⑤", "⑥"];
var SRC = {   // 값의 출처 표시
  js: "관찰 · JS가 직접 넣은 머리",
  fetch: "관찰 · fetch가 읽은 응답 머리",
  backend: "관찰 · 백엔드가 직접 본 값",
  compare: "관찰 값 비교",
  design: "구조 설명",
  guess: "추론"
};

function header(r, key) {
  var h = (r.headers || []).find(function (x) { return x[0] === key; });
  return h ? h[1] : null;
}
// 값 글자: hi = 바뀐 값 강조(파랑 굵게), was = 바뀌기 전(약한 회색), bad = 오류(빨강), 없으면 보통 글자
function v(text, tone) {
  // 긴 코드 값(긴 URL 등)은 줄바꿈을 허용해 카드 밖으로 나가지 않게 한다
  var wrap = tone && tone.indexOf("code") >= 0 && String(text).length > 24 ? " wrap" : "";
  return el("span", "v" + (tone ? " " + tone : "") + wrap, text);
}
// 비교 한 행: 이름 | 이전 → 이후 (설명)
function change(name, before, after, note) {
  var row = el("div", "chg");
  row.appendChild(el("span", "chg-name", name));
  var line = el("span", "chg-line");
  line.append(before, el("span", "chg-arrow", " → "), after);
  if (note) line.appendChild(el("span", "chg-note", note));
  row.appendChild(line);
  return row;
}
// 백엔드가 공간 부족으로 뺀 값은 null로 온다
function shown(x) { return x == null ? "(생략됨)" : x; }
// 백엔드 관측 값의 범위: 숨긴 머리 수, 생략·이어 붙임·대체 표시(구버전 백엔드에는 없는 필드는 건너뛴다)
function recvNote(recv) {
  var t = ["정해 둔 머리만 돌려받고 나머지 " + recv.other_header_count + "개는 숨긴다."];
  if (recv.omitted && recv.omitted.length) t.push("너무 길어 생략: " + recv.omitted.join(", ") + ".");
  if (recv.repeated && recv.repeated.length) t.push("여러 줄로 온 " + recv.repeated.join(", ") + "는 \", \"로 이어 보인다.");
  if (recv.replaced_bytes) t.push("ASCII 밖 바이트 " + recv.replaced_bytes + "개는 �로 바꿔 보인다.");
  return t.join(" ");
}
// 요약 한 줄: 조각을 " · "로 잇는다
function lead() {
  var p = el("p", "lead");
  Array.prototype.forEach.call(arguments, function (n, i) { if (i) p.append(" · "); p.append(n); });
  return p;
}

// 고른 기록 하나를 번호 단계로 만든다. area: client(왼쪽) · server(오른쪽 위) · target(오른쪽 아래)
function buildSteps(r) {
  var route = ROUTE[r.kind];
  var recv = r.kind === "menu" && r.json && r.json.received;
  var inst = recv ? r.json.instance : null;
  var steps = [], no = 0;
  function add(st) { st.no = ++no; steps.push(st); return st; }

  var reqRows = Object.keys(r.sent).map(function (k) { return [k, r.sent[k]]; });
  if (r.sentBytes) reqRows.push(["본문", r.sentBytes.length + "바이트"]);
  add({ area: "client", title: r.sentBytes ? "본문을 보냄" : "요청을 보냄", src: SRC.js,
        lead: lead(v(r.method + " " + r.url, "code")), body: [kv(reqRows)],
        note: "브라우저가 자동으로 붙인 머리(Host, User-Agent 등)는 페이지에서 읽을 수 없어 보이지 않는다." });

  function response() {
    if (r.pending) { add({ area: "client", title: "응답 기다리는 중", lead: lead(v("아직 끝나지 않음", "was")) }); return; }
    var parts = [v(statusText(r), failed(r) ? "bad" : "strong")];
    if (r.kind === "memo" && !failed(r) && r.body) {
      parts.push(el("span", null, "보낸 " + r.sentBytes.length + "바이트 = 받은 " + r.body.length + "바이트 "));
      parts[1].appendChild(v(r.same ? "일치" : "불일치", r.same ? "hi" : "bad"));
    }
    add({ area: "client", title: "응답을 받음", src: r.status == null ? null : SRC.fetch, bad: failed(r),
          lead: lead.apply(null, parts), body: r.headers ? [kv(r.headers)] : [],
          note: r.status == null ? "응답을 받지 못했다: " + r.error :
                r.bodyError ? "상태 줄과 머리는 받았지만 본문을 끝까지 받지 못했다: " + r.bodyError :
                ms(r) + " 걸림 · 응답 머리는 fetch가 읽을 수 있는 것 전부다." });
  }

  if (r.pending || r.status == null) { response(); return steps; }

  if (route === "proxy" && recv) {
    var trace = r.sent["X-Demo-Trace"], got = recv.headers["X-Demo-Trace"];
    add({ area: "server", title: "경로·머리를 바꿔 전달", src: SRC.compare,
          body: [
            change("경로", v(r.url, "was code"), v(shown(recv.path), "hi code")),
            change("버전", v(r.protocol || "미확인", "was code"), v(shown(recv.version), "hi code"), "왼쪽: 브라우저 연결(Resource Timing)"),
            change("목적지", v("페이지 주소 " + location.host, "was"), v("Host: " + (recv.headers.Host || "없음"), "hi code"), "브라우저가 보낸 Host 머리는 볼 수 없다"),
            change("Connection", v("브라우저 값 볼 수 없음", "was"), v(recv.headers.Connection || "없음", "hi code")),
            change("X-Demo-Trace", v(trace, "code"), v(got || "없음", got === trace ? "code" : "bad"), got === trace ? "그대로" : "다름")
          ],
          note: "① 보낸 요청과 ③ 백엔드가 본 값을 비교했다. 서버 안 처리 과정은 캡처하지 않았다." });
    add({ area: "target", title: "백엔드 " + inst + "가 받은 요청", src: SRC.backend,
          lead: (function () { var p = el("p", "lead"); p.append(v(recv.method + " ", "code"), v(shown(recv.path), "hi code"), v(" " + shown(recv.version), "code")); return p; })(),
          body: [kv(Object.keys(recv.headers).map(function (k) { return [k, recv.headers[k]]; }))],
          note: recvNote(recv) });
    var back = header(r, "x-demo-backend");
    add({ area: "server", title: "백엔드 응답이 서버를 거쳐 돌아감", src: SRC.design,
          lead: lead(back ? v("X-Demo-Backend: " + back, "code") : v("X-Demo-Backend 없음", "was")),
          note: "중계 과정은 구조 설명이다. 근거는 ⑤에서 fetch가 읽은 X-Demo-Backend 머리뿐이다." });
    response();
  } else if (route === "proxy") {
    add({ area: "server", title: "백엔드로 전달 시도", src: SRC.design,
          lead: lead(v(r.url, "code"), v("백엔드 A·B 중 하나", "was")),
          note: "/api/ 규칙에 맞아 백엔드로 보낸다. 어느 백엔드였는지는 이 페이지에서 알 수 없다." });
    add({ area: "target", title: "도착 확인 불가", stop: true,
          lead: lead(v(r.bodyError ? "도착 기록을 읽지 못함" : "백엔드 도착 기록 없음", "bad")),
          note: r.bodyError ? "응답 본문이 중간에 끊겨 백엔드 도착 기록을 읽을 수 없다." : "응답에 백엔드가 남긴 도착 기록이 없다. 백엔드에 무엇이 도착했는지 알 수 없다." });
    var back2 = header(r, "x-demo-backend");
    add({ area: "server", title: r.bodyError ? "응답이 중간에 끊김" : "서버가 실패를 알림", src: back2 ? SRC.fetch : SRC.guess, bad: true,
          lead: lead(v(statusText(r), "bad"), back2 ? v("X-Demo-Backend: " + back2, "code") : v("X-Demo-Backend 없음", "was")),
          note: back2 ? "" : "데모 백엔드는 이 머리를 항상 붙이므로 백엔드가 만든 응답이 아닌 것으로 본다." });
    response();
  } else if (route === "static") {
    add({ area: "server", title: "정적 파일로 처리", src: SRC.design,
          body: [change("경로", v(r.url, "was code"), v("demo/www" + r.url, "hi code"))],
          note: "/api/·/echo가 아닌 경로는 루트 폴더의 파일로 찾는다." });
    var len = header(r, "content-length"), type = header(r, "content-type");
    add({ area: "target", title: failed(r) ? "파일을 주지 못함" : "파일을 보냄", src: SRC.fetch, stop: failed(r), bad: failed(r),
          lead: failed(r) ? lead(v(statusText(r), "bad")) : lead(v((type || "종류 없음").split(";")[0], "hi code"), v((len || "?") + "바이트")),
          note: "폴더 읽기는 캡처하지 않았다. 종류·크기는 ④의 응답 머리 값이다." });
    response();
  } else {
    add({ area: "server", title: "/echo로 나눔", src: SRC.design,
          body: [change("경로", v("POST " + r.url, "was code"), v("서버 내부 echo", "hi"))],
          note: "POST /echo는 백엔드로 보내지 않고 서버가 직접 처리한다." });
    add({ area: "target", title: failed(r) ? "되돌리지 못함" : "받은 본문을 그대로 되돌림", src: SRC.compare, stop: failed(r), bad: failed(r),
          lead: failed(r) ? lead(v(statusText(r), "bad")) : lead(el("span", null, "되돌린 본문 "), v(r.body.length + "바이트", "hi")),
          note: "바이트 수는 ④에서 브라우저가 받은 본문으로 확인했다." });
    response();
  }
  return steps;
}

function card(st, steps, names) {
  var c = el("section", "card" + (st.stop ? " stop" : "") + (st.bad ? " bad" : ""));
  c.dataset.area = st.area;
  c.style.order = String(st.no);
  c.appendChild(el("span", "area-tag", names[st.area]));
  var h = el("h3");
  h.append(el("span", "no", NUM[st.no]), st.title);
  if (st.src) h.appendChild(el("span", "src" + (st.src === SRC.design || st.src === SRC.guess ? " soft" : ""), st.src));
  c.appendChild(h);
  if (st.lead) c.appendChild(st.lead);
  (st.body || []).forEach(function (n) { c.appendChild(n); });
  if (st.note) c.appendChild(el("p", "fine prov", st.note));
  // 다음 단계 안내: 클라이언트 ↔ 오른쪽 열은 좌우 화살표, 같은 열은 ↓ (한 줄 화면에서는 모두 ↓)
  var next = steps[st.no];
  if (st.stop) {
    c.appendChild(el("p", "next stop", "✕ 여기서 멈춤" + (next ? " · 이어서 " + NUM[next.no] : "")));
  } else if (next) {
    // 데스크톱 배치: 클라이언트는 왼쪽, 오른쪽 열은 mini-webserver(위)·처리 대상(아래)
    var where = names[next.area];
    var wide = next.area === st.area ? "다음 " + NUM[next.no] + " ↓" :
      st.area === "client" ? "다음 " + NUM[next.no] + " → " + where :
      next.area === "client" ? "← 다음 " + NUM[next.no] + " " + where :
      next.area === "target" ? "다음 " + NUM[next.no] + " ↓ " + where : "다음 " + NUM[next.no] + " ↑ " + where;
    var n = el("p", "next");
    n.append(el("span", "wide", wide), el("span", "narrow", "↓ 다음 " + NUM[next.no]));
    c.appendChild(n);
  }
  return c;
}

function highlightFlow(r) {
  var flow = $("flow");
  flow.querySelectorAll(".pair i").forEach(function (i) { i.classList.remove("on"); });
  if (!r) {
    flow.dataset.route = "none";
    flow.dataset.fail = "false";
    $("edge-in").textContent = "요청";
    $("edge-out").textContent = "—";
    return;
  }
  var route = ROUTE[r.kind];
  var recv = r.kind === "menu" && r.json && r.json.received;
  flow.dataset.route = route;
  flow.dataset.fail = String(failed(r));
  $("edge-in").replaceChildren(r.method + " " + r.url,
    el("small", failed(r) ? "back bad" : "back", r.pending ? "진행 중" : "← " + statusText(r)));
  if (route === "proxy") {
    $("edge-out").textContent = recv ? recv.method + " " + recv.path : (r.pending ? "진행 중" : "도착 확인 불가");
    var inst = recv ? r.json.instance : null;
    flow.querySelectorAll(".pair i").forEach(function (i) { i.classList.toggle("on", i.dataset.inst === inst); });
  } else {
    $("edge-out").textContent = route === "static" ? "파일 읽기" : "본문 되돌림";
  }
}

function renderNetwork() {
  renderLog();
  renderDetail();
}

function renderDetail() {
  var r = selectedReq;
  highlightFlow(r);
  var out = $("exchange");
  out.replaceChildren();
  if (!r) {
    out.appendChild(el("p", "detail-empty", activities.length ? "왼쪽 기록에서 요청 한 건을 고른다." : "기록이 생기면 여기에 고른 요청의 흐름이 나온다."));
    return;
  }
  var title = el("div", "detail-head");
  title.appendChild(el("h2", null, r.method + " " + r.url));
  title.appendChild(el("p", "fine", "요청 ID " + r.id + " · 시작 " + clockMs(r.startAt) +
    (r.pending ? " · 아직 끝나지 않음" : " · 완료 " + clockMs(r.at) + " · " + ms(r))));
  out.appendChild(title);

  var steps = buildSteps(r);
  var route = ROUTE[r.kind];
  var names = {
    client: "클라이언트",
    server: "mini-webserver",
    target: route === "proxy" ? (r.json && r.json.received ? "백엔드 " + r.json.instance : "백엔드 A·B") : route === "static" ? "정적 파일 폴더" : "서버 내부 echo"
  };
  var guide = el("ol", "guide");
  steps.forEach(function (st) {
    var li = el("li", (st.stop ? "stop" : "") + (st.bad ? " bad" : ""));
    li.append(el("b", null, NUM[st.no]), " " + st.title);
    guide.appendChild(li);
  });
  out.appendChild(guide);

  // 세 블록: 왼쪽 클라이언트, 오른쪽 위 mini-webserver, 오른쪽 아래 처리 대상
  var block = {}, sub = {
    client: "이 브라우저",
    server: ":8080",
    target: route === "proxy" ? "별도 프로그램" : route === "static" ? "demo/www" : "서버 안 · 백엔드 아님"
  };
  ["client", "server", "target"].forEach(function (k) {
    block[k] = el("div", "area area-" + k);
    var h = el("p", "area-head");
    h.append(el("b", null, names[k]), el("span", null, sub[k]));
    block[k].appendChild(h);
  });
  steps.forEach(function (st) { block[st.area].appendChild(card(st, steps, names)); });
  var areas = el("div", "areas");
  var right = el("div", "area-col");
  right.append(block.server, block.target);
  areas.append(block.client, right);
  out.appendChild(areas);

  var cmd = el("p", "fine");
  cmd.append("와이어 원문 전체는 터미널에서: ");
  cmd.appendChild(el("code", null, "curl -v " + (r.method === "POST" ? "--data-binary 'hi' " : "") + new URL(r.url, location.href).href));
  out.appendChild(cmd);
}

$("go-service").addEventListener("click", function () { selectTab($("tab-service"), true); });

/* ---------- 3. 장애와 동시성 ---------- */
function setLane(name, startMs, endMs, scale, label) {
  var lane = document.querySelector('.lane[data-lane="' + name + '"]');
  var bar = lane.querySelector(".bar");
  bar.style.marginLeft = (startMs / scale * 100) + "%";
  bar.style.width = Math.max(0.6, (endMs - startMs) / scale * 100) + "%";
  lane.querySelector(".lane-time").textContent = label;
}
function drawAxis(scale) {
  var axis = $("axis");
  axis.replaceChildren();
  for (var s = 0; s * 1000 <= scale; s++) {
    var t = el("span", null, s + "초");
    t.style.left = (s * 1000 / scale * 100) + "%";
    axis.appendChild(t);
  }
}

$("run-race").addEventListener("click", function () {
  var button = this;
  var verdict = $("race-verdict");
  var done = busy(button, verdict, "진행 중…");
  var t0 = performance.now();
  var res = {};
  var live = setInterval(function () {
    var now = performance.now() - t0, scale = Math.max(3500, now);
    drawAxis(scale);
    setLane("slow", 0, res.slow ? res.slow.end - t0 : now, scale, res.slow ? "" : "기다리는 중");
    if (now > 200) setLane("fast", 200, res.fast ? res.fast.end - t0 : now, scale, res.fast ? "" : "기다리는 중");
  }, 50);
  var slow = send("slow", "/api/slow?ms=3000").then(function (r) { res.slow = r; return r; });
  var fast = new Promise(function (ok) { setTimeout(ok, 200); })
    .then(function () { return send("image", "/img/latte.png"); }).then(function (r) { res.fast = r; return r; });
  Promise.all([slow, fast]).then(function (rs) {
    clearInterval(live);
    done();
    var s = rs[0], f = rs[1];
    var scale = Math.max(s.end, f.end) - t0;
    drawAxis(scale);
    setLane("slow", s.start - t0, s.end - t0, scale, statusText(s) + " · " + ((s.end - t0) / 1000).toFixed(2) + "초에 끝남");
    setLane("fast", f.start - t0, f.end - t0, scale, statusText(f) + " · " + ((f.end - t0) / 1000).toFixed(2) + "초에 끝남");
    verdict.textContent = f.end < s.end
      ? "사진 요청이 느린 요청보다 " + ((s.end - f.end) / 1000).toFixed(1) + "초 먼저 끝났다."
      : "이번에는 느린 요청이 먼저 끝났다.";
  });
});

function failButton(id, outId, url, label) {
  $(id).addEventListener("click", function () {
    var out = $(outId);
    var done = busy(this, out, label);
    send("fail", url).then(function (r) {
      done();
      out.replaceChildren(el("b", r.status >= 500 || r.status == null ? "bad" : "good", statusText(r)), " · " + ms(r) + " · ", el("code", null, "GET " + url));
    });
  });
}
failButton("run-502", "out-502", "/down/menu", "요청 중…");
failButton("run-504", "out-504", "/api/slow?ms=7000", "백엔드 기다리는 중…");

// 주소의 #service·#network·#stress로 시작 탭을 고른다(모든 정의가 끝난 뒤).
var initial = tabs.find(function (t) { return "#" + t.id.slice(4) === location.hash; });
if (initial) selectTab(initial);

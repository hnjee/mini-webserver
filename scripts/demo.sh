#!/bin/bash
# 1분 시연: 명령 하나로 정적 파일, 리버스 프록시 라운드 로빈, 워커 스레드 수에 따른 부하를 차례로 보여 줘요.
#   bash scripts/demo.sh           (장면 사이 3초 쉼)
#   PAUSE=2 bash scripts/demo.sh   (쉬는 시간 바꾸기)
PORT="${PORT:-8080}"
PAUSE="${PAUSE:-3}"
D=/tmp/ws_demo
URL="http://127.0.0.1:$PORT"
cd "$(dirname "$0")/.." || exit 1

B=$'\033[1m'; C=$'\033[36m'; G=$'\033[32m'; N=$'\033[0m'
title() { printf '\n%s━━ %s ━━%s\n' "$B$C" "$1" "$N"; sleep 1; }
run()   { printf '%s$ %s%s\n' "$G" "$1" "$N"; eval "$1"; sleep "$PAUSE"; }

cleanup() {
    [ -n "$SRV" ] && kill -INT "$SRV" 2>/dev/null
    [ -n "$B1" ] && kill "$B1" 2>/dev/null
    [ -n "$B2" ] && kill "$B2" 2>/dev/null
}
trap cleanup EXIT

# ---- 준비 ----
[ -x ./webserver ] || make >/dev/null || exit 1
mkdir -p $D/www $D/b1/api $D/b2/api
cp www/index.html www/cat.txt $D/www/        # 리눅스 폴더로 복사 (윈도우 폴더는 파일 읽기가 느려요)
echo "백엔드 8001이 응답했어요" > $D/b1/api/whoami.txt
echo "백엔드 8002가 응답했어요" > $D/b2/api/whoami.txt
printf '/api/ 127.0.0.1:8001 127.0.0.1:8002\n' > $D/routes.conf
(cd $D/b1 && exec python3 -m http.server 8001 >/dev/null 2>&1) & B1=$!
(cd $D/b2 && exec python3 -m http.server 8002 >/dev/null 2>&1) & B2=$!
up() { curl -s -o /dev/null --max-time 1 "$1"; }
# 서버를 워커 $1개로 띄워요 (0이면 기본값: 코어 수 x 2). 떠 있던 서버는 먼저 꺼요
start_server() {
    if [ -n "$SRV" ]; then kill -INT "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; fi
    if [ "$1" = 0 ]; then
        ./webserver -p "$PORT" -r $D/www -c $D/routes.conf > $D/access.log 2> $D/server.err & SRV=$!
    else
        ./webserver -p "$PORT" -r $D/www -c $D/routes.conf -t "$1" > $D/access.log 2> $D/server.err & SRV=$!
    fi
    for i in $(seq 50); do up "$URL/index.html" && break; sleep 0.1; done
}
start_server 0
for i in $(seq 50); do               # 백엔드가 뜰 때까지 최대 10초
    up http://127.0.0.1:8001/ && up http://127.0.0.1:8002/ && break
    sleep 0.2
done
if ! kill -0 "$SRV" 2>/dev/null; then
    echo "서버를 못 띄웠어요:"; cat $D/server.err; exit 1
fi
if ! up http://127.0.0.1:8001/ || ! up http://127.0.0.1:8002/; then
    echo "백엔드(8001, 8002)를 못 띄웠어요. 포트를 다른 프로그램이 쓰는지 확인해 주세요"; exit 1
fi
clear 2>/dev/null
printf '%sC 웹 서버 시연%s  (포트 %s, 워커 스레드마다 epoll)\n' "$B" "$N" "$PORT"

title "1. 정적 파일 서빙: www/cat.txt를 그대로 보내요"
run "curl -s $URL/cat.txt"

title "2. 리버스 프록시: /api/ 요청을 백엔드 두 대에 번갈아 보내요 (라운드 로빈)"
run "for i in 1 2 3 4; do curl -s $URL/api/whoami.txt; sleep 0.4; done"

title "3. 워커 스레드: 같은 부하를 워커 1개와 4개로 비교해요"
if command -v wrk >/dev/null; then
    LOAD="wrk -t4 -c100 -d4s $URL/index.html | grep -E 'requests in|Requests/sec|Socket errors'"
elif command -v ab >/dev/null; then
    LOAD="ab -q -n 20000 -c 200 $URL/index.html | grep -E 'Failed requests|Requests per second'"
else                                 # wrk도 ab도 없으면 curl 2000번을 50개씩 동시에 보내고 걸린 시간을 봐요
    LOAD="time (seq 2000 | xargs -P 50 -I{} curl -s -o /dev/null -w '%{http_code}\\n' $URL/index.html | sort | uniq -c)"
fi
for t in 1 4; do
    start_server "$t"
    printf '%s$ ./webserver -t %s   # 스레드 %s개 (메인 1 + 워커 %s)%s\n' "$G" "$t" "$(ls /proc/$SRV/task | wc -l)" "$t" "$N"
    run "$LOAD"
done
printf '%s끝%s\n' "$B" "$N"

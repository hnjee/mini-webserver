#!/usr/bin/env bash
# 시연 데모를 한 명령으로 띄운다: 비root Linux 컨테이너 안에서 서버와 백엔드 A/B를 최신 소스로 빌드·실행한다.
#
#   ./demo/run.sh [start|status|stop] [--name 이름] [--port 호스트포트]
#   기본: start, 이름 mws-demo, 포트 8080 (127.0.0.1에만 공개)
#
# 이 스크립트가 만든 컨테이너에는 라벨 mini-webserver.demo=run.sh 를 붙이고,
# status·stop·재시작은 이 라벨이 있는 컨테이너만 다룬다. 라벨 없는 같은 이름 컨테이너나 사용 중인 포트는 건드리지 않고 멈춘다.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=mini-webserver-dev
LABEL=mini-webserver.demo=run.sh
NAME=mws-demo
PORT=8080
CMD=start
READY_TIMEOUT=60

usage() { sed -n '4,6p' "$0" | sed 's/^# \{0,1\}//'; }
die() { echo "run.sh: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        start|status|stop) CMD=$1 ;;
        --name) [ $# -ge 2 ] || die "--name 뒤에 이름이 필요하다"; NAME=$2; shift ;;
        --port) [ $# -ge 2 ] || die "--port 뒤에 포트가 필요하다"; PORT=$2; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "알 수 없는 인자: $1" ;;
    esac
    shift
done
[[ $NAME =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*$ ]] || die "컨테이너 이름이 올바르지 않다: $NAME"
[[ $PORT =~ ^[0-9]+$ ]] && [ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || die "포트는 1~65535 정수여야 한다: $PORT"
command -v docker >/dev/null || die "docker 명령이 없다. Docker를 설치하고 실행한 뒤 다시 시도하라."
docker info >/dev/null 2>&1 || die "Docker 데몬에 연결할 수 없다. Docker Desktop(또는 dockerd)을 실행하라."

URL="http://127.0.0.1:$PORT"

# 같은 이름 컨테이너의 ID와 소유 여부. 없으면 빈 문자열.
container_id() { docker ps -aq --filter "name=^/${NAME}\$"; }
owned() { [ "$(docker inspect -f '{{index .Config.Labels "mini-webserver.demo"}}' "$1" 2>/dev/null)" = "run.sh" ]; }
running() { [ "$(docker inspect -f '{{.State.Running}}' "$1" 2>/dev/null)" = "true" ]; }
port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; }

# 페이지와 /api/menu가 모두 정상인지 확인한다.
ready() {
    local body
    command -v curl >/dev/null || return 1
    curl -fsS -o /dev/null --max-time 3 "$URL/" 2>/dev/null || return 1
    body=$(curl -fsS --max-time 3 "$URL/api/menu" 2>/dev/null) || return 1
    [[ $body == *'"menu":['* ]]
}

# 컨테이너 안에서 실행할 스크립트(bash, PID 1은 --init의 tini).
# 저장소는 읽기 전용으로 붙이고, 빌드 산출물은 /tmp에만 만든다.
INNER='
set -u
echo "[demo] 빌드 시작: $(uname -sm)"
mkdir -p /tmp/build && cp -r /workspace/src /workspace/Makefile /tmp/build/ || exit 1
make -s -C /tmp/build webserver || { echo "[demo] 서버 빌드 실패"; exit 1; }
cc -std=c11 -Wall -Wextra -Wpedantic -O2 -o /tmp/demo-backend /workspace/demo/backend/backend.c ||
    { echo "[demo] 백엔드 빌드 실패"; exit 1; }
echo "[demo] 빌드 완료"
/tmp/demo-backend 8001 A & a=$!
/tmp/demo-backend 8002 B & b=$!
/tmp/build/webserver -p 8080 -r /workspace/demo/www -c /workspace/demo/routes.conf & s=$!
# docker stop(SIGTERM): 서버는 SIGINT로 정상 종료, 백엔드는 TERM. 모두 끝난 뒤 0으로 나간다.
trap "kill -INT $s 2>/dev/null; kill $a $b 2>/dev/null; wait; echo \"[demo] 정지\"; exit 0" TERM INT
# 백엔드 하나를 일부러 끄는 시연(502)이 있으므로 서버가 끝날 때만 컨테이너를 끝낸다.
wait $s
rc=$?
echo "[demo] 서버가 끝났다(rc=$rc). 백엔드를 정리하고 종료한다."
kill -INT $s 2>/dev/null; kill $a $b 2>/dev/null; wait
exit 1
'

do_start() {
    local id
    id=$(container_id)
    if [ -n "$id" ]; then
        if ! owned "$id"; then
            cat >&2 <<EOF
run.sh: 이름이 '$NAME'인 컨테이너가 이미 있고, run.sh가 만든 것이 아니다(라벨 $LABEL 없음).
  그 컨테이너는 건드리지 않는다. 다음 중 하나를 고른다.
  - 다른 이름·포트로 띄우기: ./demo/run.sh --name mws-demo-2 --port 18080
  - 그 컨테이너가 더 필요 없다면 직접 정리한 뒤 다시 실행: docker stop $NAME
EOF
            exit 1
        fi
        if running "$id"; then
            if ready; then
                echo "이미 실행 중이다: $NAME → $URL/"
                exit 0
            fi
            die "'$NAME'이 실행 중이지만 $URL 에서 응답하지 않는다. ./demo/run.sh stop --name $NAME 뒤 다시 시작하라."
        fi
        echo "멈춘 이전 컨테이너($NAME, run.sh 소유)를 지우고 새로 시작한다."
        docker rm "$id" >/dev/null
    fi
    if port_busy; then
        die "127.0.0.1:$PORT 를 이미 다른 프로그램이 쓰고 있다. 다른 포트로 띄워라: ./demo/run.sh --port 18080"
    fi
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "이미지 $IMAGE 가 없어 만든다(처음 한 번, 몇 분 걸릴 수 있다)."
        docker build -t "$IMAGE" "$ROOT" || die "이미지 빌드 실패"
    fi

    id=$(docker run -d --init --name "$NAME" --label "$LABEL" \
        -p "127.0.0.1:$PORT:8080" -v "$ROOT:/workspace:ro" \
        "$IMAGE" bash -c "$INNER") || die "컨테이너를 시작하지 못했다(위 docker 메시지 참고)."

    # 실패하면 이번에 만든 컨테이너만 로그를 보여 주고 지운다.
    fail() {
        echo "run.sh: $1" >&2
        echo "----- 컨테이너 로그 ($NAME) -----" >&2
        docker logs "$id" >&2 2>&1 || true
        echo "--------------------------------" >&2
        docker rm -f "$id" >/dev/null 2>&1 || true
        exit 1
    }
    trap 'fail "중단됨"' INT TERM

    echo "빌드·기동 중… (최대 ${READY_TIMEOUT}초)"
    local t=0
    until ready; do
        running "$id" || fail "컨테이너가 준비 전에 끝났다."
        [ "$t" -lt "$READY_TIMEOUT" ] || fail "${READY_TIMEOUT}초 안에 $URL/ 와 /api/menu 가 준비되지 않았다."
        sleep 1
        t=$((t + 1))
    done
    trap - INT TERM
    echo "준비 완료: $URL/   (통신 보기: $URL/#network)"
    echo "상태: ./demo/run.sh status$( [ "$NAME" = mws-demo ] || printf ' --name %s' "$NAME")$( [ "$PORT" = 8080 ] || printf ' --port %s' "$PORT")"
    echo "정지: ./demo/run.sh stop$( [ "$NAME" = mws-demo ] || printf ' --name %s' "$NAME")"
}

do_status() {
    local id
    id=$(container_id)
    [ -n "$id" ] || { echo "$NAME: 없음"; exit 3; }
    owned "$id" || { echo "$NAME: 있지만 run.sh가 만든 것이 아니다(관리하지 않음)"; exit 2; }
    echo "$NAME: $(docker inspect -f '{{.State.Status}} (시작 {{.State.StartedAt}})' "$id")"
    docker port "$id" 8080 2>/dev/null | sed 's/^/  포트: /' || true
    if running "$id"; then
        if ready; then echo "  응답: $URL/ 와 /api/menu 정상"; else echo "  응답: $URL 에서 확인 실패(포트가 다르면 --port 지정)"; exit 1; fi
    else
        exit 1
    fi
}

do_stop() {
    local id
    id=$(container_id)
    [ -n "$id" ] || { echo "$NAME: 없음(할 일 없음)"; exit 0; }
    owned "$id" || die "'$NAME'은 run.sh가 만든 컨테이너가 아니라 멈추지 않는다."
    docker stop -t 15 "$id" >/dev/null
    docker rm "$id" >/dev/null
    echo "$NAME: 정지·삭제했다."
}

"do_$CMD"

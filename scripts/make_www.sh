#!/bin/sh
# DESIGN.md Step 0의 공통 테스트 파일 중, 커밋하지 않는 두 개를 만든다.
#   large.bin  : 수 MB 바이너리 (큰 파일 전송 확인)
#   secret.txt : 읽기 권한 없는 파일 (403 확인)
set -e
ROOT="${1:-./www}"

dd if=/dev/urandom of="$ROOT/large.bin" bs=1M count=5 status=none
echo "large.bin: $(wc -c < "$ROOT/large.bin") bytes"

echo "이 파일이 보이면 403 처리가 안 된 것" > "$ROOT/secret.txt"
chmod 000 "$ROOT/secret.txt"
if [ -r "$ROOT/secret.txt" ]; then
    echo "경고: secret.txt가 아직 읽힘. 윈도우 폴더 연결 때문에 chmod가 안 먹었을 수 있음."
    echo "      /tmp/www 같은 리눅스 폴더에서 다시 실행: ./scripts/make_www.sh /tmp/www"
else
    echo "secret.txt: 권한 000 확인"
fi

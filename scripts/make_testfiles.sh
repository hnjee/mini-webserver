#!/bin/sh
# git에 올리지 않는 테스트 파일을 만든다.
#   large.bin  : 5MB 랜덤 바이너리 (큰 파일 전송 테스트, git에 올리기엔 큼)
#   secret.txt : 권한 000 파일 (403 테스트, git은 권한 000을 저장하지 못함)
set -eu

WWW_DIR="${1:-www}"
mkdir -p "$WWW_DIR"

dd if=/dev/urandom of="$WWW_DIR/large.bin" bs=1048576 count=5 2>/dev/null
echo "생성: $WWW_DIR/large.bin (5MB)"

# 권한 000 파일은 덮어쓸 수 없으니 지우고 새로 만든다.
# 지우기는 파일이 아니라 폴더 권한만 필요해서 Docker 공유 폴더에서도 된다.
rm -f "$WWW_DIR/secret.txt"
printf 'this file must not be served\n' > "$WWW_DIR/secret.txt"
chmod 000 "$WWW_DIR/secret.txt"
echo "생성: $WWW_DIR/secret.txt (권한 000)"

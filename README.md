# C Web Server (+ Reverse Proxy)

POSIX 소켓만으로 만드는 HTTP 서버 과제.

## 폴더
- `src/` 소스, `include/` 헤더, `tests/` 단위 테스트(`test_*.c`)
- `www/` 정적 파일 루트, `conf/` 라우팅 설정

## 명령
- `make` 빌드 → `./webserver`
- `make run` 8080 포트로 실행
- `make debug` ASan·UBSan 빌드
- `make www` 테스트 파일(large.bin, secret.txt) 만들기
- `make test` 단위 테스트 전체 실행

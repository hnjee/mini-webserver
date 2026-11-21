# Step 0 테스트

이 문서를 읽으면 Step 0을 어떤 순서로 테스트하고, 결과를 어디에 기록하는지 알 수 있어요. 요구사항은 [Step 0 요구사항](../requirements/step0.md), 설계는 [Step 0 설계](../design/step0.md)에 있어요.

## 1. 테스트 범위

| 종류 | 확인하는 것 | 요구사항 |
| --- | --- | --- |
| 빌드 테스트 | `make`, `make debug`, `make clean`이 규칙대로 동작하는지 | S0-B01~B05 |
| 단위 테스트 | `config_parse()`가 입력마다 맞는 결과 코드를 돌려주는지 | S0-A01~A06, S0-A08 |
| 실행 테스트 | `./webserver`를 실제로 실행했을 때 출력과 종료 코드 | S0-A01~A08 |
| 테스트 파일 확인 | `www/` 파일과 `make www` 결과, `.gitignore` | S0-W01~W03 |

Step 0에는 소켓이 없어서 `curl`이나 `nc`로 하는 기능 테스트와 부하 테스트는 하지 않아요.

## 2. 준비

- Dev Container 터미널에서 저장소 루트로 이동해요.
- `secret.txt` 권한 확인(S0-W02)은 윈도우 폴더에서는 `chmod`가 적용되지 않아요. 아래처럼 리눅스 폴더에 따로 만들어 확인해요.

```bash
mkdir -p /tmp/www && cp www/* /tmp/www/
./scripts/make_www.sh /tmp/www
```

## 3. 빌드 테스트

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| BT-01 | S0-B01 | `make && ls webserver` | `webserver`가 출력된다 |
| BT-02 | S0-B02 | `grep -rhoE '#include <[^>]+>' src include \| sort -u` | 표준 C·POSIX 헤더만 나온다 |
| BT-03 | S0-B03 | `make clean && make 2>&1 \| grep -c warning` | `0`이 출력된다 |
| BT-04 | S0-B04 | `make clean && make debug && nm webserver \| grep -c __asan_init` | 1 이상이 출력된다 |
| BT-05 | S0-B05 | `make clean && ls webserver build` | 두 경로 모두 `No such file or directory`가 나온다 |

BT-04 다음에는 `make clean && make`로 일반 빌드로 되돌린 뒤 다음 테스트를 해요.

## 4. 단위 테스트

`tests/test_config.c`에 아래 경우를 모두 넣고 `make test`로 실행해요. 함수는 출력이나 `exit()`를 하지 않으므로 결과 코드만 확인해요.

| 번호 | 입력 (`argv`) | 기대 결과 | 요구사항 |
| --- | --- | --- | --- |
| UT-01 | `-p 8080 -r ./www` | `CONFIG_OK`, `port == 8080`, `root`가 `"./www"` | S0-A01 |
| UT-02 | `-p 8080 -r ./www -c routes.conf` | `CONFIG_OK`, `config_path`가 `"routes.conf"` | S0-A02 |
| UT-03 | `-p 8080 -r ./www` (`-c` 없음) | `config_path == NULL` | S0-A02 |
| UT-04 | `-p 8080` | `CONFIG_ERR_USAGE` | S0-A03 |
| UT-05 | `-r ./www` | `CONFIG_ERR_USAGE` | S0-A03 |
| UT-06 | `-p abc -r ./www` | `CONFIG_ERR_PORT` | S0-A04 |
| UT-07 | `-p 80a -r ./www` | `CONFIG_ERR_PORT` | S0-A04 |
| UT-08 | `-p 0 -r ./www` | `CONFIG_ERR_PORT` | S0-A04 |
| UT-09 | `-p 65536 -r ./www` | `CONFIG_ERR_PORT` | S0-A04 |
| UT-10 | `-p 65535 -r ./www` | `CONFIG_OK` (경곗값) | S0-A04 |
| UT-11 | `-p 8080 -r ./없음` | `CONFIG_ERR_ROOT` | S0-A05 |
| UT-12 | `-p 8080 -r ./www/index.html` | `CONFIG_ERR_ROOT` | S0-A05 |
| UT-13 | `-x` | `CONFIG_ERR_USAGE` | S0-A06 |
| UT-14 | UT-01 다음에 UT-04를 이어서 호출 | 두 번째도 `CONFIG_ERR_USAGE` (`optind` 초기화 확인) | 설계 7장 |
| UT-15 | `-p 8080 -p 9090 -r ./www` | `CONFIG_ERR_USAGE` | S0-A08 |
| UT-16 | `-p 8080 -r ./www -r ./www` | `CONFIG_ERR_USAGE` | S0-A08 |
| UT-17 | `-p 8080 -r ./www -c a.conf -c b.conf` | `CONFIG_ERR_USAGE` | S0-A08 |

**통과 기준**: `make test`가 `test_config`에서 실패 없이 끝나고, `make clean && make test-debug`로 ASan·UBSan을 켜고 돌려도 오류가 없다.

## 5. 실행 테스트

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| RT-01 | S0-A01, S0-A07 | `./webserver -p 8080 -r ./www; echo $?` | `port: 8080`, `root: ./www`가 나오고 `0` |
| RT-02 | S0-A02 | `./webserver -p 8080 -r ./www -c routes.conf; echo $?` | 출력에 `config: routes.conf`가 있고 `0` |
| RT-03 | S0-A03 | `./webserver -p 8080; echo $?` | `usage:`로 시작하는 줄과 `1` |
| RT-04 | S0-A03 | `./webserver -r ./www; echo $?` | `usage:`로 시작하는 줄과 `1` |
| RT-05 | S0-A04 | `./webserver -p abc -r ./www; echo $?` | 포트 에러와 `1` |
| RT-06 | S0-A04 | `./webserver -p 0 -r ./www; echo $?` | 포트 에러와 `1` |
| RT-07 | S0-A04 | `./webserver -p 70000 -r ./www; echo $?` | 포트 에러와 `1` |
| RT-08 | S0-A05 | `./webserver -p 8080 -r ./없음; echo $?` | 루트 에러와 `1` |
| RT-09 | S0-A05 | `./webserver -p 8080 -r ./www/index.html; echo $?` | 루트 에러와 `1` |
| RT-10 | S0-A06 | `./webserver -x; echo $?` | `usage:`로 시작하는 줄과 `1` |
| RT-11 | 설계 7장 | `./webserver -p 8080 2>/dev/null` | 아무것도 출력되지 않는다 (에러는 표준 에러로만) |
| RT-12 | S0-A08 | `./webserver -p 8080 -p 9090 -r ./www; echo $?` | `usage:`로 시작하는 줄과 `1` |

## 6. 테스트 파일 확인

| 번호 | 요구사항 | 명령 | 기대 결과 |
| --- | --- | --- | --- |
| WT-01 | S0-W01 | `ls www` | `index.html`, `style.css`, `app.js`, `image.png`, `photo.jpg`가 모두 나온다 |
| WT-02 | S0-W02 | `ls -l /tmp/www/large.bin /tmp/www/secret.txt` | `large.bin`은 1MB 이상, `secret.txt` 권한은 `----------` |
| WT-03 | S0-W03 | `git check-ignore www/large.bin www/secret.txt` | 두 경로가 모두 출력된다 |

## 7. 결과 기록

테스트를 돌린 뒤 이 표를 채워요. 실패하면 비고에 무엇이 나왔는지 적어요.

| 날짜 | 커밋 | 빌드 (BT) | 단위 (UT) | 실행 (RT) | 파일 (WT) | 비고 |
| --- | --- | --- | --- | --- | --- | --- |
| | | / 5 | / 17 | / 12 | / 3 | |

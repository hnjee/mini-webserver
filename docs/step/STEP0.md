# Step 0. 공통 준비

## 목표

서버 기능을 만들기 전에 빌드 방법, 실행 방법, 테스트 재료를 준비해요. 요구사항 ID는 T-01이에요.

- `make`로 빌드돼요.
- `./webserver -p 8080 -r ./www`처럼 포트와 루트를 실행 인자로 받아요.
- `www/`에 Step 3부터 쓸 공통 테스트 파일이 있어요.
- 빌드·실행·테스트는 Docker의 Ubuntu 컨테이너에서 해요. 편집은 Mac에서 해요.

이 단계의 서버는 아직 연결을 받지 않아요. 인자를 읽어 출력하고 바로 끝나요.

## 핵심 과제

1. **빌드**: 컴파일 명령을 `make` 한 번으로 줄이고, 메모리 오류를 잡는 ASan 디버그 빌드를 따로 둬요.
2. **실행 인자**: `argv`에서 포트·루트·설정 파일을 꺼내고, 잘못된 값은 서버를 켜기 전에 막아요.
3. **테스트 파일**: 모든 팀원이 같은 파일로 Step 3 완료 기준을 확인할 수 있게 해요.
4. **개발 환경**: macOS 대신 리눅스 컨테이너에서 빌드하고 테스트해서, 리눅스 전용 기능과 도구를 그대로 써요.

## 질문과 정리

**Q. `./webserver -p 8080 -r ./www`에서 `argc`와 `argv`는?**

`argc`는 5예요. 프로그램 이름도 하나로 세요.

```
argv[0] = "./webserver"
argv[1] = "-p"
argv[2] = "8080"
argv[3] = "-r"
argv[4] = "./www"
argv[5] = NULL
```

**Q. `./webserver -r ./www -p 8080`처럼 순서를 바꿔도 돼야 하나?**

돼야 해요. `-p`, `-r` 같은 이름표를 붙이는 이유가 순서에 기대지 않기 위해서예요. `curl -i -X POST`와 `curl -X POST -i`가 같은 것과 같아요. `argv`를 앞에서부터 보다가 `"-p"`를 만나면 바로 다음 칸 `argv[i + 1]`이 포트 값이에요.

**Q. 포트 문자열 `"8080"`을 정수로 바꿀 때 `atoi()`면 충분한가?**

부족해요. `atoi("abc")`도 `atoi("0")`도 0을 돌려줘서, 잘못된 입력을 구별할 수 없어요. `atoi("80abc")`는 에러 없이 80이 돼요. `strtol()`은 숫자가 끝난 위치(`end`)와 범위 초과(`errno`)를 알려줘서 실패를 알아챌 수 있어요.

**Q. 인자 찾기를 직접 `for`문으로 짤까, `getopt()`를 쓸까?**

`getopt()`를 써요. POSIX 표준 함수라 과제 규칙에서 허용되고, 순서 무관 처리와 "값이 빠진 옵션", "모르는 옵션" 검사를 대신 해줘요.

**Q. Mac에서 바로 개발할까, Docker로 리눅스 환경에서 개발할까?**

Docker로 리눅스 환경에서 개발해요. macOS에서는 아래 문제가 생겨요.

| 시점 | macOS에서 생기는 문제 |
| --- | --- |
| Step 5 동시성 | `epoll`은 리눅스 전용이에요. macOS는 `kqueue`로 따로 짜야 해요 |
| Step 7 메모리 검사 | valgrind가 최신 macOS(특히 Apple Silicon)에서 거의 돌아가지 않아요 |
| Step 8 최종 선택 | 같은 테스트로 비교해야 하는데, OS가 다르면 결과가 달라질 수 있어요 |
| 테스트 명령 | `nc` 옵션이 OS마다 달라요 |

편집은 Mac의 VS Code에서 하고, 프로젝트 폴더를 컨테이너에 마운트해서 빌드와 실행만 컨테이너에서 해요. 지금까지 만든 코드는 POSIX만 써서 리눅스에서도 그대로 빌드돼요.

## 이 단계에서 정한 것

| #&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; | 결정 | 이유 |
| - | ---- | ---- |
| 0-1 | 실행 인자는 `-p` 포트, `-r` 루트, `-c` 라우팅 설정 파일로 받는다. | 팀원 모두 같은 명령어로 완료 기준을 확인하기 위해서예요. (DESIGN.md 제안과 같음) |
| 0-2 | 인자 파싱은 `getopt()`로 한다. | POSIX 표준이라 과제에서 허용되고, 순서 무관 처리와 기본 에러 검사를 해줘요. |
| 0-3 | 포트는 `strtol()`로 바꾸고 1~65535 정수만 받는다. | `atoi()`는 `"abc"`나 `"80abc"` 같은 잘못된 입력을 알아챌 수 없어요. |
| 0-4 | `-p`와 `-r`은 필수, `-c`는 선택으로 한다. 루트는 디렉터리인지, `-c`는 파일인지 시작할 때 검사한다. | `-c`는 Step 6부터 써요. 잘못된 경로는 요청을 받기 전에 알려주는 게 원인을 찾기 쉬워요. |
| 0-5 | 잘못된 인자는 에러와 사용법을 `stderr`에 출력하고 종료 코드 2로 끝낸다. `-h`는 사용법을 `stdout`에 출력하고 0으로 끝낸다. | 종료 코드 2는 "사용법 오류"를 뜻하는 관례예요. |
| 0-6 | `make`는 `webserver`, `make debug`는 ASan을 넣은 `webserver-debug`를 만든다. 오브젝트 파일은 `build/release/`와 `build/debug/`로 나눈다. | 디버그 빌드에 ASan을 넣는 건 DESIGN.md 제안과 같아요. 실행 파일과 오브젝트를 나누면 `make clean` 없이 두 빌드를 오갈 수 있어요. |
| 0-7 | 컴파일 옵션은 `-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic`로 한다. | 표준 C와 POSIX만 쓰는지 컴파일러가 확인해줘요. `getopt()`, `stat()` 같은 POSIX 함수를 쓰려면 `_POSIX_C_SOURCE`가 필요해요. |
| 0-8 | 소스는 `src/`에 두고, 설정 읽기는 `config.c`/`config.h`로 분리한다. | `main.c`는 흐름만 담고, Step마다 기능을 모듈로 붙이기 위해서예요. |
| 0-9 | `www/`에 `index.html`, `style.css`, `app.js`, `image.png`, `photo.jpg`, `large.bin`, `secret.txt`를 둔다. | 정적 파일, MIME, 큰 파일, 403 테스트에 같은 재료를 쓰기 위해서예요. (DESIGN.md 제안과 같음) |
| 0-10 | `large.bin`과 `secret.txt`는 `make www`로 만들고 git에 올리지 않는다. | `large.bin`은 5MB라 저장소에 넣기엔 커요. git은 권한 000을 저장하지 못해서 `secret.txt`는 받은 뒤 직접 만들어야 해요. |
| 0-11 | 빌드·실행·테스트는 Docker의 Ubuntu 24.04 컨테이너에서 한다. 편집은 Mac에서 한다. | `epoll`과 valgrind를 쓸 수 있고, 팀원과 같은 환경에서 결과를 비교할 수 있어요. |
| 0-12 | `compose.yaml`로 프로젝트 폴더를 컨테이너의 `/app`에 마운트하고, 8080 포트를 Mac에 연다. | Mac에서 고친 파일이 바로 컨테이너에 보여요. Step 3에서 Mac 브라우저로 확인할 수 있어요. |
| 0-13 | 컨테이너는 root가 아닌 일반 사용자 `dev`(UID 1000)로 실행한다. | root는 권한 000 파일도 읽을 수 있어서 Step 3의 403 테스트가 통과하지 않아요. |
| 0-14 | 컨테이너에 `SYS_PTRACE` 권한을 준다. | ASan의 누수 검사(LeakSanitizer)와 `gdb`가 `ptrace`를 써요. 권한이 없으면 컨테이너 안에서 실패해요. |
| 0-15 | `make`와 `make www`는 컨테이너 안에서만 한다. Mac에서 빌드한 적이 있으면 컨테이너에서 `make clean`부터 한다. Mac에서 만든 `www/secret.txt`가 있으면 Mac에서 지운다. | `build/`와 실행 파일이 Mac과 공유돼요. Mac에서 만든 오브젝트 파일은 리눅스에서 링크할 수 없어요. Mac에서 만든 권한 000 파일은 컨테이너에서 지우지도, `ls`로 보지도 못해요. |
| 0-16 | 이미지에 `build-essential`, `netcat-openbsd`, `curl`, `iproute2`, `apache2-utils`, `python3`, `valgrind`, `gdb`, `wrk`를 넣는다. | Step 1~8 완료 기준에 쓰는 도구예요. 처음에 다 넣어 두면 Step마다 이미지를 다시 만들지 않아도 돼요. |

## 만드는 파일과 함수

| 파일 | 내용 |
| ---- | ---- |
| `Makefile` | `all`, `debug`, `www`, `clean` 타깃 |
| `src/main.c` | `config_parse()` 결과에 따라 설정을 출력하거나 사용법을 출력하고 끝내요 |
| `src/config.h` | `struct server_config`, `enum config_result`, 함수 선언 |
| `src/config.c` | 인자 파싱과 검사 |
| `scripts/make_testfiles.sh` | `large.bin`(5MB 랜덤)과 `secret.txt`(권한 000) 생성 |
| `www/` | `index.html`, `style.css`, `app.js`, `image.png`, `photo.jpg` |
| `.gitignore` | 빌드 결과, `www/large.bin`, `www/secret.txt` 추가 |
| `Dockerfile` | Ubuntu 24.04에 개발 도구를 설치하고, 일반 사용자 `dev`로 실행해요 |
| `compose.yaml` | 폴더 마운트(`/app`), 8080 포트 연결, `SYS_PTRACE` 권한 |
| `.dockerignore` | 이미지에 소스를 넣지 않아요 (마운트로 쓰니까요) |

`config.c`의 함수예요.

| 함수 | 하는 일 |
| ---- | ------- |
| `config_parse(argc, argv, cfg)` | `getopt()`로 옵션을 읽고 검사해요. `CONFIG_OK`, `CONFIG_HELP`, `CONFIG_ERROR` 중 하나를 돌려줘요 |
| `config_print_usage(out, prog)` | 사용법을 `out`(`stdout` 또는 `stderr`)에 출력해요 |
| `parse_port(s, out)` | `strtol()`로 문자열을 포트로 바꿔요. 숫자가 없거나, 뒤에 글자가 붙거나, 범위를 벗어나면 실패해요 |
| `is_directory(path)`, `is_regular_file(path)` | `stat()`으로 경로 종류를 확인해요 |

`struct server_config`의 문자열은 `argv`를 그대로 가리켜요. 따로 할당하지 않으니 해제할 것도 없어요.

## 완료 테스트

아래 명령은 별도 표시가 없으면 모두 **컨테이너 안**에서 실행해요.

**컨테이너 준비** (Mac 터미널, 프로젝트 폴더에서)

```sh
docker compose up -d --build        # 이미지를 만들고 컨테이너를 백그라운드로 켜요 (처음 한 번은 오래 걸려요)
docker compose exec dev bash        # 컨테이너 셸로 들어가요. 터미널을 더 열 때도 이 명령을 써요
```

컨테이너 셸에서 아래를 확인해요.

```sh
whoami          # → dev (root가 아니어야 해요)
pwd             # → /app
uname -s        # → Linux
```

다 쓰면 Mac 터미널에서 `docker compose down`으로 컨테이너를 꺼요.

> **실행 전에 프롬프트를 확인해요.** `dev@xxxx:/app$`면 컨테이너, `lhj@...MacBookAir`면 Mac이에요. Mac에서 `./webserver`를 실행하면 `zsh: exec format error`가 나요. 리눅스용 실행 파일이라서예요. Mac에서 `make`를 돌렸다면 컨테이너에서 `make clean && make`로 다시 빌드해요.

**빌드**

```sh
make clean      # Mac에서 빌드한 결과가 남아 있을 수 있어서 한 번 지워요
make            # → ./webserver 생성, 경고 없음
make debug      # → ./webserver-debug 생성
make www        # → www/large.bin, www/secret.txt 생성
ls -l www       # secret.txt 권한이 ---------- 인지 확인
```

**정상 입력** (종료 코드 0, `echo $?`로 확인)

```sh
./webserver -p 8080 -r ./www          # port: 8080 / root: ./www / routes: (없음)
./webserver -r ./www -p 8080          # 순서를 바꿔도 같은 결과
./webserver -p 8080 -r ./www -c Makefile   # routes: Makefile (파일이 있기만 하면 통과)
./webserver -h                        # 사용법 출력
```

**잘못된 입력** (에러와 사용법이 나오고 종료 코드 2)

| 명령 | 기대 에러 |
| ---- | --------- |
| `./webserver -p abc -r ./www` | 포트는 1~65535 정수여야 해요 |
| `./webserver -p 80abc -r ./www` | 포트는 1~65535 정수여야 해요 |
| `./webserver -p 0 -r ./www` | 포트는 1~65535 정수여야 해요 |
| `./webserver -p 70000 -r ./www` | 포트는 1~65535 정수여야 해요 |
| `./webserver -p 8080 -r ./nope` | 루트 디렉터리가 없어요 |
| `./webserver -r ./www` | -p (포트)가 필요해요 |
| `./webserver -p 8080` | -r (루트 디렉터리)가 필요해요 |
| `./webserver -p` | -p 옵션에는 값이 필요해요 |
| `./webserver -x` | 알 수 없는 옵션이에요 |
| `./webserver -p 8080 -r ./www extra` | 옵션이 아닌 인자가 있어요 |
| `./webserver -p 8080 -r ./www -c nope.conf` | 라우팅 설정 파일이 없어요 |

**권한 000 확인**

```sh
cat www/secret.txt      # → Permission denied 가 나와야 해요
```

`whoami`가 `root`면 이 테스트가 통과하지 않아요. 그래서 컨테이너를 일반 사용자 `dev`로 실행해요.

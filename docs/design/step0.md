# Step 0 설계: 실행 인자와 빌드

이 문서를 읽으면 Step 0을 어떤 파일과 함수로 나눠 구현하는지, 인자가 잘못됐을 때 어떤 순서로 검사하고 끝내는지 알 수 있어요. 요구사항은 [Step 0 요구사항](../requirements/step0.md)에 있어요.

## 1. 구동 방법

VS Code에서 저장소 폴더를 열고 **Reopen in Container**로 개발 컨테이너에 들어간 뒤, 저장소 루트에서 실행해요.

```bash
make                                   # 빌드 → ./webserver
./webserver -p 8080 -r ./www           # 실행: 받은 포트와 루트를 출력하고 끝남
./webserver -p 8080 -r ./www -c routes.conf
make test                              # 단위 테스트 (test_config 21개)
```

### 실행 인자

| 인자 | 의미 | 필수 | 허용 형식 | 예시 |
| --- | --- | --- | --- | --- |
| `-p` | 서버가 열 포트 번호 | 필수 | 1~65535 정수 | `-p 8080` |
| `-r` | 정적 파일을 찾을 루트 디렉터리 | 필수 | 존재하는 디렉터리 경로 | `-r ./www` |
| `-c` | 리버스 프록시 라우팅 설정 파일. 내용은 Step 6에서 읽는다 | 선택 | 파일 경로 | `-c routes.conf` |

- 각 인자는 한 번만 쓴다. 같은 인자가 두 번 오면 사용법 오류다.
- 이 셋 말고 다른 인자(`-x`, `-h` 등)는 모르는 인자로 보고 사용법 오류다.

### 자주 쓰는 명령

| 하고 싶은 것 | 명령 |
| --- | --- |
| 메모리 검사를 켜고 실행 | `make clean && make debug && ./webserver -p 8080 -r ./www` |
| 메모리 검사를 켜고 단위 테스트 | `make clean && make test-debug` |
| 테스트 파일(`large.bin`, `secret.txt`) 만들기 | `make www` (권한 확인은 `./scripts/make_www.sh /tmp/www`) |
| 일반 빌드로 되돌리기 | `make clean && make` |

인자가 잘못되면 사용법이나 에러를 표준 에러에 출력하고 종료 코드 1로 끝나요. Step 0에는 소켓이 없어서 실행하면 바로 끝나요. 손님을 기다리는 건 Step 1부터예요.

## 2. 내 Step 0 규칙 한눈에 보기

이 구현이 지키는 빌드 규칙이에요. 모두 `Makefile`에 적혀 있어서 `make`를 치면 자동으로 지켜져요.

### 컴파일·링크 옵션

| 옵션 | 뜻 | 왜 |
| --- | --- | --- |
| `-std=c11` | C11 표준 문법만 쓴다 | 과제 규칙 (언어: C) |
| `-D_POSIX_C_SOURCE=200809L` | POSIX 함수(`getopt`, `stat` 등)까지만 연다 | 리눅스 전용 기능을 실수로 쓰지 않게 |
| `-Wall -Wextra -Werror` | 쓰지 않는 변수, 초기화 안 한 변수, 부호 다른 수 비교 같은 경고를 켜고(`-Wall -Wextra`), 경고가 하나라도 나오면 빌드를 실패시킨다(`-Werror`) | 버그가 될 만한 코드를 빌드 단계에서 막기 |
| `-g` | 디버깅 정보를 넣는다 | gdb·ASan이 몇 번째 줄인지 알려 주게 |
| `-Iinclude` | 헤더는 `include/`에서 찾는다 | 폴더 규칙 |
| `-pthread` (링크) | 스레드 함수를 쓸 수 있게 한다 | Step 5에서 스레드 풀을 고를 때 대비 |
| `-fsanitize=address,undefined` (`make debug`만) | 실행 중 메모리 실수와 정의되지 않은 동작을 잡는다 | 메모리 검사 (Step 7) |

### 폴더 규칙

- 소스는 `src/`, 헤더는 `include/`, 단위 테스트는 `tests/test_*.c`에 둔다.
- 중간 파일(`.o`)은 `build/`에 모으고, 실행 파일은 저장소 루트의 `webserver`로 만든다.
- `main.c`를 뺀 모듈은 단위 테스트와 같이 링크된다. 그래서 테스트할 기능은 `main.c` 밖의 모듈로 나눈다.

### 명령 규칙

| 명령 | 하는 일 |
| --- | --- |
| `make` | 일반 빌드 |
| `make debug` | ASan·UBSan을 켠 빌드 |
| `make test` | 단위 테스트 빌드와 실행 |
| `make test-debug` | ASan·UBSan을 켜고 단위 테스트 빌드와 실행 |
| `make www` | 테스트 파일(`large.bin`, `secret.txt`) 만들기 |
| `make clean` | 빌드 산출물 지우기 |

- `make`·`make test`와 `make debug`·`make test-debug`를 바꿔 빌드할 때는 `make clean`을 먼저 한다. 두 결과물이 섞이지 않게 하려는 거예요.

## 3. 파일 구성

| 파일 | 하는 일 | 관련 요구사항 |
| --- | --- | --- |
| `include/config.h` | 설정 구조체와 함수 선언 | S0-A01~A07 |
| `src/config.c` | 실행 인자 파싱과 검사 | S0-A01~A06 |
| `src/main.c` | `config_parse()`를 부르고 결과를 출력 | S0-A07 |
| `tests/test_config.c` | `config.c` 단위 테스트 | S0-A01~A06 |
| `Makefile` | 빌드, 디버그 빌드, 테스트, 정리 | S0-B01~B05 |
| `scripts/make_www.sh` | `large.bin`, `secret.txt` 만들기 | S0-W02 |

인자 처리를 `main.c`가 아니라 `config.c`에 따로 둔다. `main()` 없이 함수만 불러서 단위 테스트할 수 있게 하려는 거예요.

## 4. 설정 구조체

```c
struct server_config {
    int         port;         /* 1~65535 */
    const char *root;         /* 정적 파일 루트, argv를 그대로 가리킴 */
    const char *config_path;  /* -c 값, 없으면 NULL */
};
```

| 필드 | 의미 | 허용 형식 | 예시 |
| --- | --- | --- | --- |
| `port` | 서버가 열 포트 번호 | 1~65535 정수 | `8080` |
| `root` | 정적 파일을 찾을 디렉터리 | 존재하는 디렉터리 경로 | `"./www"` |
| `config_path` | 라우팅 설정 파일 경로. Step 6에서 읽는다 | 문자열 또는 `NULL` | `"routes.conf"` |

문자열은 복사하지 않고 `argv`를 가리키게 한다. `argv`는 프로그램이 끝날 때까지 살아 있어서 `malloc`과 `free`가 필요 없어요.

## 5. 함수

```c
/* 성공하면 0, 실패하면 아래 오류 코드 중 하나를 돌려준다 */
enum config_result config_parse(int argc, char *argv[], struct server_config *out);
void config_print_usage(const char *prog);
```

| 반환값 | 뜻 | 관련 요구사항 |
| --- | --- | --- |
| `CONFIG_OK` (0) | 모두 올바름 | S0-A07 |
| `CONFIG_ERR_USAGE` | 필수 인자 빠짐, 모르는 인자, 같은 인자 두 번 | S0-A03, S0-A06, S0-A08 |
| `CONFIG_ERR_PORT` | 포트가 정수가 아니거나 범위 밖 | S0-A04 |
| `CONFIG_ERR_ROOT` | 루트가 없거나 디렉터리가 아님 | S0-A05 |

`config_parse()`는 출력하거나 `exit()`하지 않는다. 결과만 돌려주고, 메시지 출력과 종료 코드는 `main()`이 정한다. 그래야 단위 테스트에서 프로그램이 끝나지 않아요.

## 6. 검사 순서

```mermaid
flowchart TD
    A["getopt로 -p, -r, -c 읽기"] --> B{"모르는 인자나<br/>같은 인자 두 번?"}
    B -- 예 --> U["CONFIG_ERR_USAGE"]
    B -- 아니요 --> C{"-p와 -r이 다 있나?"}
    C -- 아니요 --> U
    C -- 예 --> D{"포트가 1~65535 정수?"}
    D -- 아니요 --> P["CONFIG_ERR_PORT"]
    D -- 예 --> E{"루트가 존재하는 디렉터리?"}
    E -- 아니요 --> R["CONFIG_ERR_ROOT"]
    E -- 예 --> OK["CONFIG_OK"]
```

사용법 오류를 먼저 검사한다. `-p 70000`만 준 경우처럼 여러 오류가 겹치면 사용법 오류로 끝나요.

## 7. 세부 결정

- **포트 변환**: `atoi()` 대신 `strtol()`을 쓴다. `atoi("abc")`는 0을 돌려줘서 오류인지 0인지 구분할 수 없어요. `strtol()`은 끝 포인터로 숫자 뒤에 글자가 남았는지(`"80a"`), `errno`로 범위를 넘었는지 알 수 있어요.
- **루트 확인**: `stat()`으로 존재를 확인하고 `S_ISDIR()`로 디렉터리인지 본다. 읽기 권한까지는 Step 3에서 파일마다 확인해요.
- **같은 인자 두 번**: `-p 8080 -p 9090`처럼 같은 인자가 다시 오면 마지막 값으로 덮어쓰지 않고 사용법 오류로 본다. 서버는 포트 하나만 열고, 두 번 쓰는 건 대부분 실수라서 일찍 알려 주려는 거예요. 포트가 둘 필요하면 서버를 두 번 실행한다.
- **남는 인자**: `./webserver -p 8080 -r ./www extra`처럼 옵션이 아닌 인자가 남으면 사용법 오류로 본다.
- **`getopt()`의 자체 출력 끄기**: `opterr = 0`으로 `getopt()`가 직접 에러를 찍지 않게 한다. 출력은 `main()`이 맡는다.
- **`getopt()` 다시 쓰기**: 단위 테스트에서 `config_parse()`를 여러 번 부르므로, 함수 시작에서 `optind = 1`로 되돌린다.
- **출력과 종료 코드**: 성공은 표준 출력에 `port:`, `root:`, `config:`를 쓰고 0으로 끝낸다. 오류는 표준 에러에 쓰고 1로 끝낸다.

## 8. 빌드 설계

| 명령 | 하는 일 | 관련 요구사항 |
| --- | --- | --- |
| `make` | `build/*.o`를 만들고 `webserver`로 링크 | S0-B01, S0-B03 |
| `make debug` | ASan·UBSan 옵션을 붙여 빌드 | S0-B04 |
| `make test` | `tests/test_*.c`를 `main.c`를 뺀 모듈과 링크해 실행 | 단위 테스트 |
| `make www` | `scripts/make_www.sh` 실행 | S0-W02 |
| `make clean` | `build/`와 `webserver` 삭제 | S0-B05 |

`make`와 `make debug`의 결과물이 섞이지 않게, 바꿔 빌드할 때는 `make clean`을 먼저 한다.

## 9. 단위 테스트 목록

| 입력 | 기대 반환값 |
| --- | --- |
| `-p 8080 -r ./www` | `CONFIG_OK`, `port == 8080` |
| `-p 8080 -r ./www -c routes.conf` | `CONFIG_OK`, `config_path`가 `"routes.conf"` |
| `-p 8080` | `CONFIG_ERR_USAGE` |
| `-r ./www` | `CONFIG_ERR_USAGE` |
| `-x` | `CONFIG_ERR_USAGE` |
| `-p abc -r ./www` | `CONFIG_ERR_PORT` |
| `-p 80a -r ./www` | `CONFIG_ERR_PORT` |
| `-p 0 -r ./www` | `CONFIG_ERR_PORT` |
| `-p 70000 -r ./www` | `CONFIG_ERR_PORT` |
| `-p 8080 -r ./없음` | `CONFIG_ERR_ROOT` |
| `-p 8080 -r ./www/index.html` | `CONFIG_ERR_ROOT` |

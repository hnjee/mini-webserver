# Step 0 요구사항: 공통 준비

이 문서를 읽으면 Step 0에서 무엇이 되어야 끝인지, 각 항목을 어떤 명령어로 확인하고 어떤 결과가 나와야 통과인지 알 수 있어요. 상위 요구사항은 [DESIGN.md의 T-01](../../DESIGN.md#3-요구사항-id)이에요.

요구사항 유형은 팀 EARS 형식(항상, 이벤트, 상태, 오류, 선택)을 따라요. 확인 명령어는 저장소 루트에서 실행해요.

## 빌드

| ID | 유형 | 요구사항 | 확인 | 기대 결과 |
| --- | --- | --- | --- | --- |
| S0-B01 | 항상 | 시스템은 `make` 한 번으로 실행 파일 `webserver`를 만들어야 한다. | `make && ls webserver` | `webserver`가 출력된다 |
| S0-B02 | 항상 | 시스템은 C11 표준과 POSIX API만 사용해야 한다. | `grep -rhoE '#include <[^>]+>' src include \| sort -u` | 표준 C·POSIX 헤더만 나온다 |
| S0-B03 | 항상 | 시스템은 `-Wall -Wextra -Werror` 옵션으로 경고 없이 빌드되어야 한다. | `make clean && make` | 종료 코드 0, `warning`이 출력되지 않는다 |
| S0-B04 | 항상 | 시스템은 `make debug`로 ASan·UBSan을 켠 실행 파일을 만들어야 한다. | `make clean && make debug && nm webserver \| grep -c __asan_init` | 1 이상이 출력된다 |
| S0-B05 | 항상 | 시스템은 `make clean`으로 빌드 산출물을 모두 지워야 한다. | `make clean && ls webserver build` | 두 경로 모두 `No such file or directory`가 나온다 |

## 실행 인자

| ID | 유형 | 요구사항 | 확인 | 기대 결과 |
| --- | --- | --- | --- | --- |
| S0-A01 | 항상 | 시스템은 `-p`로 포트 번호를, `-r`로 정적 파일 루트를 받아야 한다. | `./webserver -p 8080 -r ./www` | S0-A07의 출력이 나온다 |
| S0-A02 | 선택 | `-c`가 주어지면, 시스템은 그 값을 라우팅 설정 파일 경로로 저장해야 한다. 파일 내용은 [Step 6](../../DESIGN.md#step-6-리버스-프록시-동적-요청-처리-포함)에서 읽는다. | `./webserver -p 8080 -r ./www -c routes.conf` | 출력에 `config: routes.conf`가 있다 |
| S0-A03 | 오류 | `-p`나 `-r`이 빠지면, 시스템은 사용법을 출력하고 종료 코드 1로 끝나야 한다. | `./webserver -p 8080; echo $?`<br>`./webserver -r ./www; echo $?` | 두 경우 모두 `usage:`로 시작하는 줄이 나오고 `1`이 출력된다 |
| S0-A04 | 오류 | 포트가 1~65535 정수가 아니면, 시스템은 에러를 출력하고 종료 코드 1로 끝나야 한다. | `./webserver -p abc -r ./www; echo $?`<br>`./webserver -p 0 -r ./www; echo $?`<br>`./webserver -p 70000 -r ./www; echo $?` | 세 경우 모두 포트 에러가 나오고 `1`이 출력된다 |
| S0-A05 | 오류 | 루트가 없거나 디렉터리가 아니면, 시스템은 에러를 출력하고 종료 코드 1로 끝나야 한다. | `./webserver -p 8080 -r ./없음; echo $?`<br>`./webserver -p 8080 -r ./www/index.html; echo $?` | 두 경우 모두 루트 에러가 나오고 `1`이 출력된다 |
| S0-A06 | 오류 | 모르는 인자가 오면, 시스템은 사용법을 출력하고 종료 코드 1로 끝나야 한다. | `./webserver -x; echo $?` | `usage:`로 시작하는 줄이 나오고 `1`이 출력된다 |
| S0-A07 | 항상 | 인자가 모두 올바르면, 시스템은 받은 포트와 정적 파일 루트를 출력하고 종료 코드 0으로 끝나야 한다. 소켓은 [Step 1](../../DESIGN.md#step-1-tcp-에코-서버)에서 연다. | `./webserver -p 8080 -r ./www; echo $?` | `port: 8080`, `root: ./www`가 나오고 `0`이 출력된다 |

출력 문구(`usage:`, `port:`, `root:`, `config:`)는 확인을 쉽게 하려고 정한 예시예요. 각자 구현에서 바꿨다면 기대 결과 열도 같이 고쳐요.

## 테스트 파일

| ID | 유형 | 요구사항 | 확인 | 기대 결과 |
| --- | --- | --- | --- | --- |
| S0-W01 | 항상 | `www/`에는 `index.html`, `style.css`, `app.js`, `image.png`, `photo.jpg`가 있어야 한다. | `ls www` | 다섯 파일이 모두 나온다 |
| S0-W02 | 항상 | `make www`는 `large.bin`(수 MB)과 읽기 권한이 없는 `secret.txt`를 만들어야 한다. | `make www && ls -l www/large.bin www/secret.txt` | `large.bin`은 1MB 이상이고, `secret.txt`의 권한은 `----------`이다 |
| S0-W03 | 항상 | 저장소는 `www/large.bin`과 `www/secret.txt`를 `.gitignore`로 제외해야 한다. | `git check-ignore www/large.bin www/secret.txt` | 두 경로가 모두 출력된다 |

S0-W02에서 `secret.txt`가 읽히면, 윈도우 폴더를 그대로 연결한 환경이라 `chmod`가 적용되지 않은 거예요. 이때는 `./scripts/make_www.sh /tmp/www`로 리눅스 폴더에 만들어 확인해요.

## 완료 기준

- 위 표의 확인 명령어를 모두 실행했을 때 기대 결과 열과 같다.
- 특히 S0-A07처럼 올바른 인자로 실행하면 포트와 루트를 출력하고 종료 코드 0으로 끝난다.

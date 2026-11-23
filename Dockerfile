# mini-webserver 개발 환경 (Ubuntu 24.04)
# 소스는 이미지에 넣지 않고 compose.yaml에서 /app으로 마운트한다.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# build-essential : gcc, make            (Step 0~)
# netcat-openbsd  : nc 에코 테스트         (Step 1~)
# curl            : HTTP 테스트           (Step 2~)
# iproute2        : ss로 TIME_WAIT 확인    (Step 1)
# apache2-utils   : ab 부하 테스트         (Step 5)
# python3         : 백엔드 서버            (Step 6)
# valgrind, gdb   : 메모리 검사, 디버깅      (Step 7)
# wrk             : 부하 테스트            (Step 8)
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        netcat-openbsd \
        curl \
        ca-certificates \
        iproute2 \
        apache2-utils \
        python3 \
        valgrind \
        gdb \
        wrk \
    && rm -rf /var/lib/apt/lists/*

# root는 권한 000 파일도 읽을 수 있어서 403 테스트가 안 된다. 일반 사용자로 실행한다.
# ubuntu:24.04에는 UID 1000인 ubuntu 사용자가 있어서, 그 사용자의 이름을 dev로 바꿔 쓴다.
RUN usermod -l dev -d /home/dev -m ubuntu && groupmod -n dev ubuntu
USER dev

WORKDIR /app
CMD ["bash"]

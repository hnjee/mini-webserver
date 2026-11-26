FROM ubuntu:24.04

ENV TZ=Asia/Seoul LANG=ko_KR.UTF-8 LANGUAGE=ko_KR.UTF-8

# 빌드/디버깅 + 네트워크 확인 + 부하 테스트(ab, wrk) + 누수 검사(valgrind)
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    locales tzdata build-essential sudo gdb vim curl git wget \
    python3 make gcc clang valgrind \
    telnet netcat-openbsd net-tools iproute2 lsof strace \
    apache2-utils wrk \
    && locale-gen ko_KR.UTF-8 && update-locale LANG=ko_KR.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

# ubuntu:24.04 기본 계정(ubuntu, uid 1000) 제거 후 jungle 생성
RUN userdel -r ubuntu 2>/dev/null || true
RUN useradd -m -s /bin/bash jungle && usermod -aG sudo jungle
RUN echo "jungle ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/jungle

USER jungle
WORKDIR /home/jungle

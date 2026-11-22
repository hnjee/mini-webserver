# 빌드·실행·검증 환경: Ubuntu 24.04(glibc). 저장소는 /workspace에 bind mount한다.
#   docker build -t mini-webserver-dev .
#   docker run --rm -it -v "$PWD":/workspace mini-webserver-dev
#   (컨테이너 안) make && dir=$(make -s fixtures) && ./webserver -p 8080 -r "$dir"
# 테스트용 www는 make fixtures가 컨테이너 안 /home/jungle/mini-webserver-www에 준비한다(같은 컨테이너에서는 재사용, 새 컨테이너에서는 다시 만든다).
FROM ubuntu:24.04

ENV LANG=C.UTF-8

RUN export DEBIAN_FRONTEND=noninteractive \
    && apt-get update \
    && apt-get install -y --no-install-recommends build-essential gdb \
    && rm -rf /var/lib/apt/lists/*

# Pintos 환경처럼 이미지에 고정된 비root 계정으로 작업한다.
RUN useradd -m -s /bin/bash jungle

USER jungle
WORKDIR /workspace
CMD ["bash"]

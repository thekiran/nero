FROM ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y gcc clang make cmake python3 libcurl4-openssl-dev libcjson-dev libldns-dev libncurses-dev strace iproute2 lsof && rm -rf /var/lib/apt/lists/*

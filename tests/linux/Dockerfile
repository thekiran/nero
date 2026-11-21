FROM ubuntu:26.04@sha256:d66b366dac61b575bee2aef07e9c26e8070096fbe84a3d4d3eb0da069b5d76db
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y gcc clang make cmake python3 libcurl4-openssl-dev libcjson-dev libldns-dev libncurses-dev strace iproute2 lsof && rm -rf /var/lib/apt/lists/*

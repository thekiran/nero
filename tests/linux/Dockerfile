FROM ubuntu:26.04@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y gcc clang make cmake python3 libcurl4-openssl-dev libcjson-dev libldns-dev libncurses-dev strace iproute2 lsof && rm -rf /var/lib/apt/lists/*

# Dockerfile
FROM registry.cn-hangzhou.aliyuncs.com/acs/ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    apt-get update && \
    apt-get install -y build-essential cmake g++ && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /work
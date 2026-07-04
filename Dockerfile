FROM ubuntu:26.04

# Aliyun mirrors (中国大陆加速)
RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|http://mirrors.aliyun.com/ubuntu/|g' /etc/apt/sources.list.d/ubuntu.sources \
 && sed -i 's|http://security.ubuntu.com/ubuntu/|http://mirrors.aliyun.com/ubuntu/|g' /etc/apt/sources.list.d/ubuntu.sources

RUN apt-get update && apt-get install -y \
    # 编译工具链
    build-essential cmake ninja-build meson \
    bison flex autoconf automake libtool \
    pkgconf zip git file python3 python3-pip \
    # wayland-scanner 原生构建 (生成 Wayland 协议代码)
    libexpat1-dev libxml2-dev libffi-dev \
    # sfnt2fon 字体工具 (Wine .fon 生成)
    libfreetype-dev \
    # Wine OHOS 交叉 PE 编译 (mingw gcc + g++ for C++17 DLLs like icu.dll)
    gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \
    # HAP 签名
    default-jdk \
    # 开发工具 (持久化容器用)
    vim less gdb strace htop tree openssh-server rsync sudo \
 && apt-get clean && rm -rf /var/lib/apt/lists/*

# Python 包 (virglrenderer + Mesa guest_gfx 构建) — 使用清华 PyPI 镜像
RUN pip3 install --break-system-packages \
    -i https://pypi.tuna.tsinghua.edu.cn/simple \
    pyyaml mako markupsafe \
 && rm -rf /root/.cache/pip

# libxml2.so.2 兼容性修复
# OHOS SDK 的 ld.lld 链接器依赖 libxml2.so.2，Ubuntu 26.04 提供的是 libxml2.so.16
RUN ln -sf /usr/lib/x86_64-linux-gnu/libxml2.so.16 /usr/lib/x86_64-linux-gnu/libxml2.so.2 \
 && ldconfig

# ── 开发用户 (避免 root 写入 bind mount 导致 Host 权限问题) ──
ARG USERNAME=developer
RUN useradd -m -s /bin/bash $USERNAME \
 && echo "$USERNAME ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/$USERNAME \
 && chmod 0440 /etc/sudoers.d/$USERNAME

# ── SSH 服务 (VS Code Remote-SSH 可选) ──
RUN mkdir -p /var/run/sshd \
 && ssh-keygen -A \
 && sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin no/' /etc/ssh/sshd_config \
 && sed -i 's/#PasswordAuthentication yes/PasswordAuthentication no/' /etc/ssh/sshd_config \
 && echo 'AcceptEnv LANG LC_*' >> /etc/ssh/sshd_config
EXPOSE 22

# ── 默认环境变量 (减少 docker run/exec 时的 -e 参数) ──
ENV OHOS_SDK=/apps/harmony/sdk/default/openharmony \
    TOOL_HOME=/apps/harmony \
    NATIVE_ARCH=arm64-v8a \
    DEVICE_TYPE=pad \
    PATH=/apps/harmony/bin:/apps/harmony/tool/node/bin:$PATH

WORKDIR /data/src/winehua

# ── 使用说明 ──
#
# 开发容器 (持久化):
#   docker build -t winehua-dev .
#   bash scripts/docker_wsl_build.sh dev-start
#   bash scripts/docker_wsl_build.sh dev-exec
#
# 一次性构建 (CI):
#   docker run --rm \
#     -v /mnt/f/WineHua:/data/src/winehua \
#     -v /mnt/f/command-line-tools:/apps/harmony:ro \
#     -w /data/src/winehua \
#     winehua-dev make NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad
#
# 路径约定:
#   F:\WineHua                → /data/src/winehua   (项目源码, bind mount)
#   F:\command-line-tools     → /apps/harmony       (OHOS SDK + hvigor, bind mount ro)

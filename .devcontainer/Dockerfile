#FROM gcr.io/envoy-ci/envoy-build:f94a38f62220a2b017878b790b6ea98a0f6c5f9c@sha256:7adc40c09508f957624c4d2e0f5aeecb73a59207ee6ded53b107eac828c091b2
FROM envoyproxy/envoy-build-ubuntu:b08a261def325942b5c3bbdbc65a95197fe9746f@sha256:36aa85ccf67c3ca060113477bb391db623bf826bf4cd4184acd4814b1905701d

ARG USERNAME=vscode
ARG USER_UID=501
ARG USER_GID=$USER_UID

ENV BUILD_DIR=/build
ENV ENVOY_STDLIB=libc++

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get -y update \
  && apt-get -y install --no-install-recommends libpython2.7 net-tools psmisc vim 2>&1 \
  # Create a non-root user to use if preferred - see https://aka.ms/vscode-remote/containers/non-root-user.
  && groupadd --gid $USER_GID $USERNAME \
  && useradd -s /bin/bash --uid $USER_UID --gid $USER_GID -m $USERNAME -G pcap -d /build \
  # [Optional] Add sudo support for non-root user
  && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME \
  && chmod 0440 /etc/sudoers.d/$USERNAME

ENV DEBIAN_FRONTEND=
ENV PATH=/opt/llvm/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

ENV CLANG_FORMAT=/opt/llvm/bin/clang-format

RUN apt-get -y install --no-install-recommends pkg-config libtool autoconf-archive 2>&1 
RUN apt-get -y install --no-install-recommends openjdk-11-jdk zlib1g-dev zlib1g nodejs 2>&1 

# for compiling sed
RUN apt-get -y install --no-install-recommends autopoint texinfo 2>&1 

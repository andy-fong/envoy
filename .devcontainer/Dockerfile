FROM gcr.io/envoy-ci/envoy-build:637e381b3d59e52d30b186b50f2b26f924d52067@sha256:a41f69c56ba636856576949a22cc2ec450c48a7a37487554d8a31827e0d54c03

ARG USERNAME=vscode
ARG USER_UID=501
ARG USER_GID=$USER_UID

ENV BUILD_DIR=/build
ENV ENVOY_STDLIB=libc++

ENV DEBIAN_FRONTEND=noninteractive

# RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null \
#    && echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ focal main' | tee /etc/apt/sources.list.d/kitware.list >/dev/null
# RUN wget -O - https://download.opensuse.org/repositories/devel:kubic:libcontainers:stable/xUbuntu_20.04/Release.key 2>/dev/null | gpg --dearmor | tee /usr/share/keyrings/devel_kubic_libcontainers_stable.gpg > /dev/null \
#    && echo 'deb [signed-by=/usr/share/keyrings/devel_kubic_libcontainers_stable.gpg] http://download.opensuse.org/repositories/devel:/kubic:/libcontainers:/stable/xUbuntu_20.04/ /' | sudo tee /etc/apt/sources.list.d/devel:kubic:libcontainers:stable.list >/dev/null

RUN apt-get -y update \
  && apt-get -y install --no-install-recommends libpython2.7 net-tools psmisc vim 2>&1 \
  # Create a non-root user to use if preferred - see https://aka.ms/vscode-remote/containers/non-root-user.
  && groupadd --gid $USER_GID $USERNAME \
  && useradd -s /bin/bash --uid $USER_UID --gid $USER_GID -m $USERNAME -G pcap -d /build \
  # [Optional] Add sudo support for non-root user
  && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME \
  && chmod 0440 /etc/sudoers.d/$USERNAME

RUN apt-get -y install default-jre autoconf-archive zlib1g-dev 2>&1

ENV DEBIAN_FRONTEND=
ENV PATH=/opt/llvm/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

ENV CLANG_FORMAT=/opt/llvm/bin/clang-format

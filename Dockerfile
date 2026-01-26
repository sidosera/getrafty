# Base image
FROM ubuntu:24.04


# Set environment variables
ENV TZ=Europe/London \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    CXX="/usr/bin/clang++-19" \
    CC="/usr/bin/clang-19" 


# Dependency
ARG DEPS=" \
    ssh \
    make \
    cmake \
    build-essential \
    ninja-build \
    git \
    linux-tools-common \
    linux-tools-generic \
    clang-19 \
    clang-format-19 \
    clang-tidy-19 \
    libclang-rt-19-dev \
    libc++-19-dev \
    libc++abi-19-dev \
    clangd-19 \
    lldb-19 \
    gdb \
    binutils-dev \
    libdwarf-dev \
    libdw-dev \
    ca-certificates \
    openssh-server \
    vim \
    autoconf \
    curl \
    unzip \
    python3 \
    python3-pip \
    python3-venv \
    libgflags-dev \
    libgoogle-glog-dev \
    libfast-float-dev \
    valgrind \
    yq \
    "

RUN apt-get update -q && \
    apt-get install -y sudo && \
    echo "ubuntu ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers && \
    passwd -d ubuntu


# Install dependencies
RUN apt-get update -q && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y tzdata software-properties-common wget rsync && \
    add-apt-repository -y ppa:ubuntu-toolchain-r/test && \
    add-apt-repository universe && \
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor | tee /etc/apt/trusted.gpg.d/llvm-snapshot.gpg > /dev/null && \
    add-apt-repository -y "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-19 main" && \
    wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor | tee /etc/apt/trusted.gpg.d/kitware-archive.gpg > /dev/null && \
    add-apt-repository -y 'deb https://apt.kitware.com/ubuntu/ noble main' && \
    apt-get update -q && \
    apt-get install -y $DEPS


# Configure SSH daemon for no authentication
RUN sed -i 's/#PermitEmptyPasswords no/PermitEmptyPasswords yes/' /etc/ssh/sshd_config && \
    sed -i 's/#PasswordAuthentication yes/PasswordAuthentication yes/' /etc/ssh/sshd_config && \
    sed -i 's/#ChallengeResponseAuthentication yes/ChallengeResponseAuthentication no/' /etc/ssh/sshd_config && \
    echo "PermitRootLogin yes" >> /etc/ssh/sshd_config && \
    echo "PermitUserEnvironment yes" >> /etc/ssh/sshd_config

RUN echo "export PATH=/lib/llvm-19/bin/:$PATH" >> /home/ubuntu/.bashrc

# Configure SSH
RUN echo "StrictHostKeyChecking=no" >> /etc/ssh/ssh_config && mkdir /var/run/sshd

# Clean up
RUN rm -rf /var/lib/apt/lists/*

EXPOSE 22 3000 8000

# Start SSH service and static file server
CMD ["/bin/bash", "-lc", "SERVE_DIR=\"${WORKSPACE_DIR:-/}\"; if [ ! -d \"$SERVE_DIR\" ]; then SERVE_DIR=\"/home/ubuntu\"; fi; echo \"Starting HTTP file server on port ${HTTP_PORT:-8000}, serving $SERVE_DIR\"; python3 -m http.server \"${HTTP_PORT:-8000}\" --bind 0.0.0.0 --directory \"$SERVE_DIR\" & exec /usr/sbin/sshd -D"]

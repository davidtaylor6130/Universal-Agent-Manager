FROM node:22-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
RUN cmake -S . -B Builds/runner -DCMAKE_BUILD_TYPE=Release -DUAM_RUNNER_ONLY=ON \
    && cmake --build Builds/runner --parallel 2

FROM node:22-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends openssh-server git ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupmod --new-name uam node \
    && usermod --login uam --home /home/uam --move-home --shell /bin/bash node \
    && passwd -d uam \
    && mkdir -p /run/sshd /var/lib/uam-host-keys
COPY --from=build /source/Builds/runner/uam-runner /usr/local/bin/uam-runner
COPY deploy/processing-node/sshd_config /etc/ssh/sshd_config
COPY deploy/processing-node/start.sh /usr/local/bin/uam-node-start
ENV NPM_CONFIG_PREFIX=/home/uam/.local
ENV PATH=/home/uam/.local/bin:${PATH}
ENTRYPOINT ["/bin/sh", "/usr/local/bin/uam-node-start"]

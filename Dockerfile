# syntax=docker/dockerfile:1
#
# Helios-DAG — polyglot image.
# Stage 1 compiles the C++ binaries; stage 2 carries the shared libs plus the
# end-user runtimes AND their package managers (pip/venv, npm).

############################
# Stage 1: build
############################
FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libzmq3-dev \
        nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel \
    && test -x build/master_node && test -x build/worker_node

############################
# Stage 2: runtime  (UPDATED — polyglot dependency resolution)
############################
FROM ubuntu:22.04 AS runtime

# Non-interactive apt + a default timezone so transitive packages (e.g. tzdata
# pulled by some toolchains) never block the build on a prompt.
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
        libzmq5 \
        python3 \
        python3-venv \
        python3-pip \
        ca-certificates \
        ffmpeg \
    && rm -rf /var/lib/apt/lists/*


WORKDIR /app
COPY --from=build /src/build/master_node /usr/local/bin/master_node
COPY --from=build /src/build/worker_node /usr/local/bin/worker_node

# Overridden by docker-compose (worker runs worker_node).
CMD ["master_node", "/app/tasks.json"]
# syntax=docker/dockerfile:1
#
# Helios-DAG — polyglot image.
# Stage 1 compiles the C++ binaries; stage 2 is a slim runtime that carries
# only the shared libs plus the end-user runtimes (python3, nodejs).

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

# Bring in the build definition and sources. `include/` is expected to hold the
# project headers plus the vendored header-only deps (httplib.h, and zmq.hpp if
# you don't rely on the cppzmq-dev package above).
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel \
    && test -x build/master_node && test -x build/worker_node

############################
# Stage 2: runtime
############################
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libzmq5 \
        python3 \
        nodejs \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/master_node /usr/local/bin/master_node
COPY --from=build /src/build/worker_node /usr/local/bin/worker_node

# The artifact server resolves scripts relative to the working dir (./scripts).
# docker-compose mounts the host scripts/ and tasks.json into /app.
CMD ["master_node", "/app/tasks.json"]
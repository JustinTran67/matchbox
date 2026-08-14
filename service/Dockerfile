# syntax=docker/dockerfile:1

# librdkafka comes from the distro rather than being fetched and built: it pulls in
# OpenSSL, zlib, zstd and lz4, and rebuilding that chain on every image build would be slow
# and brittle for no benefit.
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates librdkafka-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Only the pieces the service actually needs. Tests and benchmarks are switched off, which
# also keeps Catch2 out of the image build entirely.
COPY CMakeLists.txt ./
COPY src ./src
COPY sim ./sim
COPY service ./service

RUN cmake -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DMATCHBOX_BUILD_TESTS=OFF \
      -DMATCHBOX_BUILD_BENCH=OFF \
      -DMATCHBOX_ENABLE_ASSERTS=OFF \
    && cmake --build build -j "$(nproc)" --target matchbox_engine matchbox_e2e

FROM debian:bookworm-slim AS runtime

# librdkafka++1, not just librdkafka1: the C++ bindings this service links against ship as
# their own package, and librdkafka1 alone leaves the binary unable to start.
RUN apt-get update && apt-get install -y --no-install-recommends \
      librdkafka1 librdkafka++1 libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 matchbox

COPY --from=build /src/build/service/matchbox_engine /usr/local/bin/matchbox_engine
COPY --from=build /src/build/service/matchbox_e2e /usr/local/bin/matchbox_e2e

USER matchbox
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/matchbox_engine"]

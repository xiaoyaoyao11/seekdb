# MiniVectorDB submission image.
# Build stage installs the only build dependency (g++) while networking is
# still allowed; the runtime image contains a fully static ELF binary, so no
# shared libstdc++/libgcc and no network access are needed during evaluation.
# Base image is pinned to the 24.04 tag used by the official sample.

FROM ubuntu:24.04 AS build

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends g++ \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY src/minivectordb.cpp /src/minivectordb.cpp
# Compile from source, then smoke-test the freshly built Linux ELF before it is
# allowed into the runtime image.  This catches toolchain/linking problems at
# image-build time instead of during the single official submission.
RUN mkdir -p /out /tmp/mvdb-check \
 && g++ -std=c++17 -O3 -static -static-libgcc -static-libstdc++ \
        -o /out/minivectordb /src/minivectordb.cpp \
 && test -x /out/minivectordb \
 && printf '%s\n' '{"id":1,"op":"shutdown"}' \
      | /out/minivectordb --data-dir /tmp/mvdb-check > /tmp/mvdb-check/out.jsonl \
 && grep -Fq '{"type":"ready"}' /tmp/mvdb-check/out.jsonl \
 && grep -Fq '{"id":1,"ok":true}' /tmp/mvdb-check/out.jsonl

FROM ubuntu:24.04

RUN mkdir -p /data /app
COPY --from=build /out/minivectordb /app/minivectordb
WORKDIR /app

# The judge runs `docker run -i`; stdin/stdout is the only channel.
# No EXPOSE, no network listener, no background service.
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]

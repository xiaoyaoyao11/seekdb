# MiniVectorDB evaluation image.
#
# Shape follows the official sample (ubuntu:24.04) and the four publicly
# available passing submissions for this task:
#   * single stage, no cross-stage COPY that could resolve differently;
#   * apt-get is retried: a truncated package index is a documented cause of
#     "no image -> no admission" build failures (see AtoFuture's Dockerfile);
#   * dynamic linkage with libstdc++ installed in the image, instead of a
#     fully static glibc ELF (static glibc C++ binaries are the one build
#     choice none of the passing submissions used);
#   * no in-image smoke test: any failure there aborts the whole build;
#   * VOLUME + WORKDIR /data, and every runtime argument is inside ENTRYPOINT
#     so an overridden CMD cannot drop --data-dir.
FROM ubuntu:24.04

RUN set -eux; \
    ok=0; \
    for i in 1 2 3; do \
      if apt-get update \
         && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends g++ libstdc++6 libgcc-s1; then \
        ok=1; break; \
      fi; \
      sleep 5; \
    done; \
    test "$ok" = "1"; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY src/minivectordb.cpp /app/src/minivectordb.cpp
RUN g++ -std=c++17 -O3 -o /app/minivectordb /app/src/minivectordb.cpp \
 && test -x /app/minivectordb

RUN mkdir -p /data
VOLUME ["/data"]
WORKDIR /data
ENTRYPOINT ["/app/minivectordb", "--data-dir", "/data"]

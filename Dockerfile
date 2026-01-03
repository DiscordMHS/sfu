FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    nlohmann-json3-dev \
    pkg-config

COPY src/external/jwt-cpp /tmp/jwt-cpp
WORKDIR /tmp/jwt-cpp
RUN cmake -DCMAKE_BUILD_TYPE=Release -DJWT_BUILD_EXAMPLES=OFF -DJWT_BUILD_TESTS=OFF . && \
    cmake --build . && \
    cmake --install .

COPY src/external/libdatachannel /tmp/libdatachannel
WORKDIR /tmp/libdatachannel
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DNO_EXAMPLES=ON -DNO_TESTS=ON && \
    cmake --build build -j$(nproc) && \
    cmake --install build

WORKDIR /tmp/libdatachannel/deps/json
RUN cmake -DCMAKE_BUILD_TYPE=Release . && cmake --install .

WORKDIR /app

COPY . .

WORKDIR /app/build
RUN cmake -DCMAKE_BUILD_TYPE=Release .. -DBUILD_SHARED_LIBS=OFF && \
    make -j$(nproc)

WORKDIR /app

RUN cp -r /app/build/sfu_server .

EXPOSE 8000

EXPOSE 50000-50005/udp

ENTRYPOINT ["./sfu_server"]

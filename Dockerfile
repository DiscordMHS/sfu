# ==========================================
# Stage 1: Build
# ==========================================
FROM debian:bookworm-slim AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    nlohmann-json3-dev \
    pkg-config

WORKDIR /app

# Copy the source code (including src/external/jwt-cpp)
COPY . .

# --- START: Build and Install jwt-cpp ---
# Check if the folder exists, configure, build, and install globally
WORKDIR /app/src/external/jwt-cpp
RUN cmake -DCMAKE_BUILD_TYPE=Release -DJWT_BUILD_EXAMPLES=OFF -DJWT_BUILD_TESTS=OFF . && \
    cmake --build . && \
    cmake --install .
# --- END: Build and Install jwt-cpp ---

# Return to main app directory to build the SFU
WORKDIR /app/build

# Configure and Build main application
# Since jwt-cpp was installed (to /usr/local), find_package(jwt-cpp) will now work
RUN cmake -DCMAKE_BUILD_TYPE=Release .. -DBUILD_SHARED_LIBS=OFF && \
    make -j$(nproc)

# ==========================================
# Stage 2: Runtime
# ==========================================
FROM debian:bookworm-slim AS runner

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    ca-certificates \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the binary from the builder stage
COPY --from=builder /app/build/sfu_server .

COPY --from=builder /app/data/ /app/data/

RUN ls

# Expose ports
EXPOSE 8000

# Run the server
ENTRYPOINT ["./sfu_server"]

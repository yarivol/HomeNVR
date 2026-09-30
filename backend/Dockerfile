# --- Сборка ---
FROM debian:13 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config \
    libasio-dev \
    nlohmann-json3-dev \
    libspdlog-dev \
    libpqxx-dev libpq-dev \
    libargon2-dev \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# --- Запуск ---
FROM debian:13

# -dev пакеты подтягивают нужные runtime-библиотеки;
# curl нужен для healthcheck в docker-compose
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpqxx-dev libspdlog-dev libargon2-dev \
    ffmpeg \
    curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/homenvr-backend /usr/local/bin/homenvr-backend

EXPOSE 8080
CMD ["homenvr-backend"]

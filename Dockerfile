ARG ALPINE_VERSION=latest

FROM alpine:${ALPINE_VERSION} AS build

ENV CXXFLAGS=""
WORKDIR /usr/src/telegram-bot-api

RUN apk add --no-cache \
      alpine-sdk \
      linux-headers \
      git \
      zlib-dev \
      openssl-dev \
      gperf \
      cmake

COPY . .

ARG BUILD_JOBS=4
RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX:PATH=/opt/telegram-bot-api \
 && cmake --build build --target install -j "${BUILD_JOBS}" \
 && strip --strip-unneeded /opt/telegram-bot-api/bin/telegram-bot-api

FROM alpine:${ALPINE_VERSION}

LABEL org.opencontainers.image.title="Telegram Bot API" \
      org.opencontainers.image.description="Telegram Bot API 0.1 self-hosted server for linux/amd64 and OpenWrt x86_64 Docker hosts" \
      org.opencontainers.image.version="0.1"

ENV TELEGRAM_WORK_DIR="/opt/telegram-bot-api-data" \
    TELEGRAM_TEMP_DIR="/tmp/telegram-bot-api"

RUN apk add --no-cache \
      ca-certificates \
      openssl \
      libstdc++ \
      zlib \
 && addgroup -g 10001 -S telegram \
 && adduser -S -D -H -u 10001 -h /nonexistent -s /sbin/nologin -G telegram telegram \
 && mkdir -p "${TELEGRAM_WORK_DIR}" "${TELEGRAM_TEMP_DIR}" \
 && chown -R telegram:telegram "${TELEGRAM_WORK_DIR}" "${TELEGRAM_TEMP_DIR}"

COPY --from=build --chown=telegram:telegram /opt/telegram-bot-api/bin/telegram-bot-api /usr/local/bin/telegram-bot-api

USER telegram

EXPOSE 8081/tcp 8082/tcp
VOLUME ["/opt/telegram-bot-api-data"]

ENTRYPOINT ["/usr/local/bin/telegram-bot-api"]

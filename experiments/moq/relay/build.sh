#!/usr/bin/env bash
# Builds the MOQT relay on the host: upstream ngtcp2 v1.25.0 (vendor/ngtcp2-1.25.0, git tag
# v1.25.0) with the OpenSSL 3.5+ backend, and the portable draft-21 core of the firmware. The
# ngtcp2 copy of the firmware carries ESP-IDF diagnostics and is not used on the host.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
SSL=${SSL:-$HOME/miniforge3}
NG=../vendor/ngtcp2-1.25.0
[ -d $NG ] || git clone -q --depth 1 --branch v1.25.0 https://github.com/ngtcp2/ngtcp2 $NG
T=../../../src/components/transport
if [ ! -f build-ngtcp2/lib/libngtcp2.a ]; then
  cmake -S $NG -B build-ngtcp2 -DENABLE_OPENSSL=ON -DENABLE_LIB_ONLY=ON -DENABLE_STATIC_LIB=ON \
    -DENABLE_SHARED_LIB=OFF -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=$SSL > build-ngtcp2.log
  cmake --build build-ngtcp2 -j8 >> build-ngtcp2.log
fi
gcc -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
  -DMOQT_CORE_CACHE=600 -DMOQT_CORE_FETCHES=8 -DMOQT_CORE_TRACKS=5 \
  -I$T -I$NG/lib/includes -Ibuild-ngtcp2/lib/includes -I$NG/crypto/includes -I$SSL/include \
  relay.c quic.c $T/moqt21.c $T/moqt_core.c $T/moqt_scheduler.c \
  build-ngtcp2/crypto/ossl/libngtcp2_crypto_ossl.a build-ngtcp2/lib/libngtcp2.a \
  -L$SSL/lib -Wl,-rpath,$SSL/lib -lssl -lcrypto -o ${OUT:-moq_relay}.new && mv -f ${OUT:-moq_relay}.new ${OUT:-moq_relay}
echo built ${OUT:-moq_relay}

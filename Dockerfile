FROM ubuntu:20.04 AS builder

RUN apt-get update \
 && apt-get upgrade -y \
 && apt-get install -y --no-install-recommends \
    gcc-10 g++-10 make pkg-config ca-certificates curl git wget zip unzip \
 && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 30 \
 && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 30 \
 && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc 30 \
 && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++ 30

RUN wget -q https://github.com/Kitware/CMake/releases/download/v3.19.6/cmake-3.19.6-Linux-x86_64.sh \
 && sh cmake-3.19.6-Linux-x86_64.sh --skip-license --prefix=/usr/local \
 && rm cmake-3.19.6-Linux-x86_64.sh

RUN git clone https://github.com/microsoft/vcpkg \
 && /vcpkg/bootstrap-vcpkg.sh -disableMetrics

COPY x64-linux-static.cmake /vcpkg/triplets/community/

COPY ares.hpp \
     client.cpp \
     CMakeLists.txt \
     Dockerfile.final \
     go_rng_seed_recovery.cpp \
     go_rng_seed_recovery.hpp \
     hardcode.cpp \
     hardcode.hpp \
     serialization.hpp \
     types.hpp \
     vcpkg.json \
     x64-linux-static.cmake \
     /src/

WORKDIR /build

RUN cmake -S /src -DCMAKE_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET=x64-linux-static \
      -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--whole-archive -lpthread -Wl,--no-whole-archive" \
      -DCMAKE_BUILD_TYPE=Release \
 && make

FROM scratch

WORKDIR /

COPY --from=builder /build/goldrush-client /app
COPY --from=builder /src /src

ENTRYPOINT [ "/app" ]
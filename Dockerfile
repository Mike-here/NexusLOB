FROM gcc:14-bookworm

RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake python3 python3-numpy \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/nexuslob
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure

EXPOSE 9000/tcp
CMD ["./build/nexuslob_server", "--port", "9000", "--cpu", "0"]

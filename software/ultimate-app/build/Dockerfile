FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Base dependencies
RUN apt-get update && apt-get install -y \
    build-essential git wget curl \
    python3 python3-pip python3-venv \
    cmake ninja-build ccache \
    libusb-1.0-0-dev pkg-config \
    flex bison gperf libffi-dev libssl-dev \
    dfu-util unzip xz-utils \
    && rm -rf /var/lib/apt/lists/*

# RISC-V toolchain (xPack riscv-none-elf-gcc 11.3.0)
RUN mkdir -p /opt/riscv && \
    curl -L https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v11.3.0-1/xpack-riscv-none-elf-gcc-11.3.0-1-linux-x64.tar.gz \
    | tar -xzC /opt/riscv --strip-components=1 && \
    for file in /opt/riscv/bin/riscv-none-elf-*; do \
        ln -s "$file" "/usr/local/bin/riscv32-unknown-elf-${file##*riscv-none-elf-}"; \
    done

# ESP-IDF (for WiFi chip firmware)
RUN mkdir -p /opt/esp && cd /opt/esp && \
    git clone --depth 1 -b v5.4 --recursive https://github.com/espressif/esp-idf.git && \
    cd esp-idf && ./install.sh esp32,esp32c3 && \
    rm -rf /opt/esp/esp-idf/.git

# Quartus paths (mounted at runtime from host)
ENV QUARTUS_ROOTDIR=/opt/altera_lite/18.1/quartus
ENV QSYS_ROOTDIR=/opt/altera_lite/18.1/quartus/sopc_builder/bin
ENV ALTERAOCLSDKROOT=/opt/altera_lite/18.1/hld
ENV PATH="${PATH}:/opt/altera_lite/18.1/quartus/bin"
ENV PATH="${PATH}:/opt/altera_lite/18.1/nios2eds/bin"
ENV PATH="${PATH}:/opt/altera_lite/18.1/nios2eds/sdk2/bin"
ENV PATH="${PATH}:/opt/altera_lite/18.1/nios2eds/bin/gnu/H-x86_64-pc-linux-gnu/bin"
ENV PATH="${PATH}:/opt/altera_lite/18.1/quartus/sopc_builder/bin"
ENV PATH="${PATH}:/opt/riscv/bin"

WORKDIR /project

# Source ESP-IDF on shell entry
RUN echo '. /opt/esp/esp-idf/export.sh' >> /root/.bashrc

ENTRYPOINT ["/bin/bash", "-c", ". /opt/esp/esp-idf/export.sh && exec \"$@\"", "--"]
CMD ["/bin/bash"]

FROM ubuntu:26.04

RUN apt-get update && apt-get install -y --no-install-recommends \
	bash curl ca-certificates git less openssh-client ripgrep tzdata make \
	meson ninja-build gcc pkgconf \
	libwayland-dev wayland-protocols \
	libdrm-dev libxkbcommon-dev libpixman-1-dev \
	libegl-dev libgles-dev libgbm-dev \
	libvulkan-dev glslang-tools \
	libinput-dev libudev-dev libseat-dev \
	hwdata libdisplay-info-dev libliftoff-dev \
	liblcms2-dev \
	libxcb1-dev libxcb-util-dev libxcb-render-util0-dev libxcb-icccm4-dev \
	libxcb-dri3-dev libxcb-composite0-dev libxcb-present-dev libxcb-xinput-dev \
	libxcb-res0-dev libxcb-ewmh-dev libxcb-errors-dev \
	libcairo2-dev cmake \
	xwayland \
	python3-pip python3-numpy \
	&& rm -rf /var/lib/apt/lists/*

RUN ln -sf /usr/share/zoneinfo/hostlocaltime /etc/localtime

RUN mkdir -p /home/ubuntu/persistent-config && chown ubuntu:ubuntu /home/ubuntu/persistent-config

USER ubuntu
WORKDIR /home/ubuntu

RUN SNIPPET="export PROMPT_COMMAND='history -a' && export HISTFILE=/home/ubuntu/persistent-config/.bash_history" \
  && echo "$SNIPPET" >> "/home/ubuntu/.bashrc"

WORKDIR /workspaces/wlroots

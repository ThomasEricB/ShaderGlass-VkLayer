# ShaderGlassVk: ShaderGlass on a Vulkan layer
# Copyright (C) 2026 Thomas Eric, bmitch87
# GNU General Public License v3.0
# Derived in structure from DLSS5VKLayer's packaging (relicensed to GPL-3.0, see RELICENSE.md).
#
# Built by packaging/make-dist.sh rpm, from the tree it staged: the payload is prebuilt, so this spec
# only places it. Building from source is make-dist.sh's job, because the catalogue step has to run
# tools/build-catalogue.sh -- which applies the shader patches -- and that is the same on every
# distribution.

%global debug_package %{nil}
%{!?sg_version: %global sg_version 0.1.0}
%{!?sg_release: %global sg_release 1}

Name:           shaderglass-vk
Version:        %{sg_version}
Release:        %{sg_release}%{?dist}
Summary:        RetroArch shader presets applied to games, as a Vulkan layer
License:        GPL-3.0-only
URL:            https://github.com/mausimus/ShaderGlass
Source0:        %{name}-%{version}-%{sg_release}-linux-x86_64.tar.gz
BuildArch:      x86_64

Requires:       vulkan-loader
Requires:       qt6-qtbase
Recommends:     gamescope
# Only shaderglass-gen links glslang, and only someone compiling their own presets runs it.
Recommends:     glslang
# The 32-bit layer is in this package, for 32-bit games; it needs only the 32-bit C library, which any
# system running 32-bit games already has.
Suggests:       glibc(x86-32)

# The binaries are built with the project's own toolchain and static libstdc++; nothing here is a
# system library for other packages to link against.
AutoReqProv:    no

%description
ShaderGlassVk applies libretro/RetroArch slang shader presets -- CRT, scanline, handheld and
upscaling effects, about 3300 of them -- to a game's own frames. It is a Vulkan implicit layer,
loaded into the game's process, so there is no window capture and no overlay. A game is switched on
with SHADERGLASS=1 in its launch options; shaderglass-gui chooses and tunes the effect while it
runs. OpenGL games reach it through Zink, or by running them in gamescope.

%prep
%setup -q -n %{name}-%{version}-%{sg_release}-linux-x86_64

%build
# Prebuilt payload.

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a root/usr %{buildroot}/usr

%files
%license %{_datadir}/licenses/%{name}/LICENSE
%doc %{_datadir}/doc/%{name}
/usr/lib/shaderglass
%{_bindir}/shaderglass-gui
%{_bindir}/shaderglass-ctl
%{_bindir}/shaderglass-run
%{_bindir}/shaderglass-gen
%{_datadir}/vulkan/implicit_layer.d/VK_LAYER_SHADERGLASS.x86_64.json
%{_datadir}/vulkan/implicit_layer.d/VK_LAYER_SHADERGLASS_32.i686.json
/usr/lib/environment.d/zz-shaderglass.conf
%{_datadir}/applications/shaderglass.desktop
%{_datadir}/icons/hicolor/128x128/apps/shaderglass.png

%changelog
* Fri Sep 25 2026 Thomas Eric <thombelcar@gmail.com> - 0.1.0-1
- First release: the layer, the preset catalogue, shaderglass-gui, shaderglass-ctl,
  shaderglass-run and shaderglass-gen.

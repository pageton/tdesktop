{
  description = "Telegram Desktop (tdesktop) native development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            name = "tdesktop";

            # Reuse the exact toolchain and library set nixpkgs builds
            # tdesktop with when building against system packages
            # (telegram-desktop.unwrapped): cmake, ninja, python3, pkg-config,
            # qtshadertools, gobject-introspection, Qt, tg_owt, tde2e tdlib,
            # ffmpeg, openal, hunspell, etc.
            inputsFrom = [ pkgs.telegram-desktop.unwrapped ];

            # Qt Quick support: optional in tdesktop's CMake, but present in
            # the official builds. qtimageformats is needed so CMake can find
            # the WebP codec plugin (Qt6::QWebpPlugin) for the qt.conf +
            # qt-plugins tree generated next to the binary. pango provides
            # the pangocairo pkg-config module required by upstream's
            # cmake/external/pango (MicroTeX text rendering).
            packages = [
              pkgs.ccache
              pkgs.pango
              pkgs.qt6.qtdeclarative
              pkgs.qt6.qtimageformats
            ];

            shellHook =
              let
                # The WebP codec lives in qtimageformats, outside qtbase's
                # plugin dir; an unwrapped binary can't find it and crashes
                # on emoji loading. The build now generates an app-local
                # qt.conf + qt-plugins tree for this; the env var is a
                # belt-and-suspenders fallback for incremental builds that
                # haven't been reconfigured yet.
                imagePlugins = "${pkgs.qt6.qtimageformats}/lib/qt-6/plugins";

                # WebApps/Mini Apps: the Linux build dlopen()s WebKitGTK at
                # run time (no link-time dependency), so the dynamic loader
                # must be able to find libwebkit2gtk-4.1.so.0 (the ABI this
                # source resolves to under X11-embedded Qt; it skips
                # webkitgtk-6.0 for that case). Mirror the runtime
                # environment nixpkgs gives its own telegram-desktop wrapper:
                # webkitgtk_4_1 + geoclue2 on the loader path, plus the
                # wrapGAppsHook3 pieces WebKit needs at run time
                # (glib-networking TLS/proxy GIO modules and GSettings
                # schemas). The -webviewhelper child process inherits this
                # environment from the app.
                webviewLibs = pkgs.lib.makeLibraryPath [
                  pkgs.webkitgtk_4_1
                  pkgs.geoclue2
                ];
                webviewDataDirs = pkgs.lib.makeSearchPath "share" [
                  pkgs.glib-networking
                  pkgs.gsettings-desktop-schemas
                  pkgs.gtk3
                  pkgs.shared-mime-info
                  pkgs.webkitgtk_4_1
                ];
              in
              ''
                export QT_PLUGIN_PATH="${imagePlugins}''${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
                # CMake discovers Qt packages via PATH prefixes; qtimageformats
                # ships no binaries, so put its store path on the prefix path.
                export CMAKE_PREFIX_PATH="${pkgs.qt6.qtimageformats}''${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
                export LD_LIBRARY_PATH="${webviewLibs}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
                export XDG_DATA_DIRS="${webviewDataDirs}''${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
                export GIO_EXTRA_MODULES="${pkgs.glib-networking}/lib/gio/modules''${GIO_EXTRA_MODULES:+:$GIO_EXTRA_MODULES}"
                # This host lacks the /etc wiring NixOS' NVIDIA module normally
                # creates: the NVIDIA EGL loader only searches
                # /etc/egl and /usr/share/egl for egl_external_platform.d
                # (libnvidia-egl-wayland etc.) and glvnd only searches its
                # /etc and /usr/share glvnd dirs for the vendor JSONs, and
                # none of those exist here — so the configs sit unread in
                # /run/opengl-driver/share and every EGL platform (GBM,
                # Wayland, X11) fell back to llvmpipe. Both loaders honor
                # env overrides; verified via eglinfo that all platforms then
                # report the NVIDIA GPU instead of llvmpipe.
                export __EGL_VENDOR_LIBRARY_DIRS="/run/opengl-driver/share/glvnd/egl_vendor.d''${__EGL_VENDOR_LIBRARY_DIRS:+:$__EGL_VENDOR_LIBRARY_DIRS}"
                export __EGL_EXTERNAL_PLATFORM_CONFIG_DIRS="/run/opengl-driver/share/egl/egl_external_platform.d''${__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS:+:$__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS}"
                # The webview helper pins GDK itself via
                # gdk_set_allowed_backends() to the platform the Qt app
                # resolved (wayland for the embedded compositor mode, x11 for
                # X11 embedding). GTK3 gives a GDK_BACKEND env var priority
                # over that call and filters the pinned list against it, so a
                # mismatched value (e.g. a session-wide GDK_BACKEND=wayland
                # while the app runs on xcb) leaves the helper with no usable
                # backend (silent gtk_init_check failure). Drop the env pin
                # and let the helper decide per platform.
                unset GDK_BACKEND
                # WebKit's DMABUF renderer fails to allocate GBM buffers on
                # the proprietary NVIDIA driver (gbm_bo_create returns
                # EINVAL: "Failed to create GBM buffer", Mini Apps render
                # empty). The WebProcess-side skip
                # (WEBKIT_DMABUF_RENDERER_DISABLE_GBM) did not stop the
                # WebProcess from still creating its GBM display, so disable
                # the DMABUF renderer from the UI process instead; buffers
                # then travel over shared memory. If a driver revision still
                # renders empty, escalate to WEBKIT_DISABLE_COMPOSITING_MODE=1
                # (software path, same fallback the webview helper itself uses
                # on Wayland/Raster).
                export WEBKIT_DISABLE_DMABUF_RENDERER=1
                if ! test -f cmake/external/qt/package.cmake; then
                  echo "tdesktop: git submodules are missing, run:" >&2
                  echo "    git submodule update --init --recursive" >&2
                fi

                # Public API credentials shared by distro builds; get your own
                # for anything user-facing (docs/api_credentials.md).
                export TDESKTOP_API_ID=611335
                export TDESKTOP_API_HASH=d524b414d21f4d37f08684c1df41ac9c

                # Compiler cache: when a build is interrupted, Ninja's
                # .ninja_log loses its tail and the next run re-plans the
                # whole graph, re-running every compile. Ninja still decides
                # WHAT to rebuild (dependency tracking is untouched); ccache
                # only makes those re-runs cheap (~1s per TU instead of
                # ~1min). The PCH sloppiness is required for CMake
                # precompiled-header builds to hit the cache at all.
                export CCACHE_DIR="$HOME/.cache/tdesktop-ccache"
                export CCACHE_MAXSIZE=15G
                export CCACHE_SLOPPINESS=pch_defines,time_macros

                echo "Configure and build a Debug version:"
                echo "    ./Telegram/configure.sh debug -DTDESKTOP_API_ID=\$TDESKTOP_API_ID -DTDESKTOP_API_HASH=\$TDESKTOP_API_HASH"
                echo "    cmake --build out/Debug"
                echo "Run it from this shell so WebApps find WebKitGTK:"
                echo "    out/Debug/Telegram"
              '';
          };
        }
      );

      formatter = forAllSystems (system: nixpkgs.legacyPackages.${system}.nixfmt-tree);
    };
}

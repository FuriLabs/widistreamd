project('widistreamd', 'c',
  version: '1.0.0',
  meson_version: '>= 0.59.0',
)

gnome = import('gnome')

config_h = configuration_data()
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())

configure_file(
  output: 'widi-stream-config.h',
  configuration: config_h,
)

src_inc = include_directories('src')

add_project_arguments([
  '-I' + meson.current_build_dir(),
], language: 'c')

subdir('src/wfd')
subdir('src/cc')

enum_headers = files('src/nd-sink.h')

widi_stream_sources = [
  'src/main.c',
  'src/nd-sink-list-model.c',
  'src/nd-mtk-wifi-manager.c',
  'src/nd-sink.c',
  'src/nd-provider.c',
  'src/nd-meta-sink.c',
  'src/nd-meta-provider.c',
  'src/nd-wfd-mice-sink.c',
  'src/nd-wfd-mice-provider.c',
  'src/nd-cc-sink.c',
  'src/nd-cc-provider.c',
  'src/nd-wfd-p2p-sink.c',
  'src/nd-wfd-p2p-provider.c',
  'src/nd-nm-device-registry.c',
  'src/nd-pulseaudio.c',
  'src/nd-uri-helpers.c',
]

widi_stream_sources += gnome.mkenums_simple(
  'widi-stream-enum-types',
  sources: enum_headers,
)

widi_stream_deps = [
  dependency('avahi-client'),
  dependency('avahi-gobject'),
  dependency('gio-2.0', version: '>= 2.50'),
  dependency('gio-unix-2.0', version: '>= 2.50'),
  dependency('gstreamer-1.0', version: '>= 1.14'),
  dependency('gstreamer-app-1.0', version: '>= 1.14'),
  dependency('gstreamer-pbutils-1.0', version: '>= 1.14'),
  dependency('gstreamer-plugins-base-1.0'),
  dependency('gstreamer-rtsp-server-1.0'),
  dependency('libnm', version: '>= 1.15'),
  dependency('libpulse-mainloop-glib'),
]

widi_stream_deps += wfd_server_deps
widi_stream_deps += cc_deps

executable('widistreamd',
  widi_stream_sources,
  include_directories: src_inc,
  dependencies: widi_stream_deps,
  install: true,
  link_with: [wfd_server, cc_cast_channel],
)

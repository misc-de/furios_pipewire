project('spa-droid', 'c',
  version : '0.1',
  default_options : ['c_std=gnu11', 'warning_level=1'])

droid_src = '/home/furios/furios-audio/src/pulseaudio-modules-droid-modern/src'

cc = meson.get_compiler('c')
expat_dep = dependency('expat')
# TODO: pa_channel_map_*/pa_sample_spec_*/pa_proplist_* sind reine Utilities
# aus libpulse (Client-Lib, nicht der Daemon). Fuer das fertige Plugin durch
# SPA-Aequivalente ersetzen, dann faellt diese Abhaengigkeit weg.
libpulse_dep = dependency('libpulse')
hybris_dep = cc.find_library('hybris-common', required : false)
hw_dep = cc.find_library('hardware', required : false)

android_inc = include_directories('/usr/include/android')

# Android-Version des Geraets (Halium 11)
add_project_arguments(
  ['-DANDROID_VERSION_MAJOR=11', '-DANDROID_VERSION_MINOR=0', '-DANDROID_VERSION_PATCH=0',
   '-DHAVE_CONFIG_H', '-Wno-attributes', '-Wno-int-conversion', '-Wno-unused-parameter'],
  language : 'c')

compat_inc = include_directories('compat')
droid_inc  = include_directories(droid_src / 'common', droid_src / 'common/include')

# droid-util.c: PA-Graph-Funktionen werden reproduzierbar ausgeschlossen
droid_util_spa = custom_target('droid-util-spa.c',
  input : droid_src / 'common/droid-util.c',
  output : 'droid-util-spa.c',
  command : [find_program('tools/port-droid-util.py'), '@INPUT@', '@OUTPUT@'])

# Der ohne PulseAudio portierte Teil von common/
droid_common = static_library('droid-common',
  [ droid_util_spa,
    'compat/pa-compat.c',
    'compat/pa-containers.c',
    droid_src / 'common/sllist.c',
    droid_src / 'common/utils.c',
    droid_src / 'common/conversion.c',
    droid_src / 'common/config-parser-xml.c',
    droid_src / 'common/droid-config.c' ],
  include_directories : [compat_inc, droid_inc, android_inc],
  dependencies : [expat_dep])

executable('dump-droid-config', 'tools/dump-droid-config.c',
  include_directories : [compat_inc, droid_inc, android_inc],
  link_with : droid_common)

# --- SPA-Plugin ---
spa_dep = dependency('libspa-0.2')

shared_module('spa-droid',
  ['src/droid-device.c', 'src/droid-pcm.c'],
  name_prefix : 'lib',
  include_directories : [compat_inc, droid_inc, android_inc],
  dependencies : [spa_dep, libpulse_dep, hybris_dep, hw_dep,
                  cc.find_library('pthread', required : false)],
  link_with : droid_common,
  install : false)

executable('probe-droid-device', 'tools/probe-droid-device.c',
  dependencies : [spa_dep, cc.find_library('dl', required : false)])

executable('probe-droid-hal', 'tools/probe-droid-hal.c',
  include_directories : [compat_inc, droid_inc, android_inc],
  link_with : droid_common,
  dependencies : [libpulse_dep, hybris_dep, hw_dep,
                  cc.find_library('pthread', required : false)])

executable('probe-droid-node', 'tools/probe-droid-node.c',
  dependencies : [spa_dep, cc.find_library('dl', required : false)])

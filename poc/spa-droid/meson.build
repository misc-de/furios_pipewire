# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
project('spa-droid', 'c',
  version : '0.1',
  meson_version : '>=1.1',
  default_options : ['c_std=gnu11', 'warning_level=1'])

# The upstream droid sources are not versioned in this repo; they are cloned
# next to it. The default is derived from the project directory, so the work
# tree can live anywhere - clone it wherever you like. Via fs.parent rather
# than '..', because include_directories() does not accept paths with '..'.
#
# A clone somewhere else entirely:
#   meson setup build . -Ddroid_src=/path/to/pulseaudio-modules-droid-modern
fs = import('fs')
repo_root = fs.parent(fs.parent(meson.project_source_root()))
droid_root = get_option('droid_src')
if droid_root == ''
  droid_root = repo_root / 'src/pulseaudio-modules-droid-modern'
endif
droid_src = droid_root / 'src'

if not fs.is_dir(droid_src / 'common')
  error('upstream droid sources not found at ' + droid_src + '''/common.

  Clone them next to this repo:
    mkdir -p ''' + repo_root + '''/src
    git clone https://github.com/FuriLabs/pulseaudio-modules-droid-modern \
        "''' + repo_root + '''/src/pulseaudio-modules-droid-modern"

  Or point at an existing clone:
    meson setup <builddir> <srcdir> -Ddroid_src=/path/to/pulseaudio-modules-droid-modern
''')
endif

cc = meson.get_compiler('c')
expat_dep = dependency('expat')
# No more libpulse: the 14 small things it was needed for (channel maps,
# sample formats, a tiny property list) now live in compat/pa-audio.c. The
# libpulse headers stay in use - the types have to match - only the code is
# ours now.
libpulse_dep = dependency('libpulse', required : false)
pulse_inc = include_directories()
hybris_dep = cc.find_library('hybris-common', required : false)
hw_dep = cc.find_library('hardware', required : false)

android_inc = include_directories('/usr/include/android')

# Android version of the device (Halium 11)
add_project_arguments(
  ['-DANDROID_VERSION_MAJOR=11', '-DANDROID_VERSION_MINOR=0', '-DANDROID_VERSION_PATCH=0',
   '-DHAVE_CONFIG_H', '-Wno-attributes', '-Wno-int-conversion', '-Wno-unused-parameter'],
  language : 'c')

compat_inc = include_directories('compat')
droid_inc  = include_directories(droid_src / 'common', droid_src / 'common/include')

# droid-util.c: PA graph functions are excluded reproducibly
droid_util_spa = custom_target('droid-util-spa.c',
  input : droid_src / 'common/droid-util.c',
  output : 'droid-util-spa.c',
  command : [find_program('tools/port-droid-util.py'), '@INPUT@', '@OUTPUT@'])

# The part of common/ ported without PulseAudio
droid_common = static_library('droid-common',
  [ droid_util_spa,
    'compat/pa-compat.c',
    'compat/pa-containers.c',
    'compat/pa-audio.c',
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

# -z nodelete: the plugin must never be unloaded.
#
# PipeWire drops an SPA plugin once nothing uses it any more - which happens
# whenever WirePlumber restarts and the nodes go away. That takes libhybris'
# Android linker state with it, and creating the nodes again re-initialises it
# inside a process that has meanwhile filled its address space (Bluetooth
# codecs, for one). The linker does not get the region it wants and faults, and
# the whole daemon goes down. Keeping the library resident keeps that state
# alive for the life of the process, which is what libhybris expects.
shared_module('spa-droid',
  ['src/droid-device.c', 'src/droid-pcm.c'],
  name_prefix : 'lib',
  include_directories : [compat_inc, droid_inc, android_inc],
  dependencies : [spa_dep, hybris_dep, hw_dep,
                  cc.find_library('pthread', required : false)],
  link_args : ['-Wl,-z,nodelete'],
  link_with : droid_common,
  install : false)

# --- tests ---
#
# The test includes droid-device.c, so it needs the same surroundings as the
# plugin. It opens no HAL: what it exercises are the decisions, not the device.
droid_device_test = executable('test-droid-device',
  'tests/test-droid-device.c',
  c_args : ['-DTEST_FIXTURE="' + meson.current_source_dir() / 'tests/audio-policy-fixture.xml' + '"',
            '-DTEST_FIXTURE_NO_PRIMARY="' + meson.current_source_dir() / 'tests/audio-policy-no-primary.xml' + '"',
            '-DTEST_FIXTURE_TOO_MANY="' + meson.current_source_dir() / 'tests/audio-policy-too-many.xml' + '"'],
  include_directories : [compat_inc, droid_inc, android_inc],
  dependencies : [spa_dep, hybris_dep, hw_dep,
                  cc.find_library('m', required : false),
                  cc.find_library('pthread', required : false)],
  link_with : droid_common,
  build_by_default : false)
test('droid-device', droid_device_test)

# The node's test links the configuration parser and our own compat code, but
# NOT droid-util - the HAL half is replaced by tests/hal-stub.c. Linking both
# would be two definitions of every pa_droid_* function.
droid_config_only = static_library('droid-config-only',
  [ 'compat/pa-compat.c',
    'compat/pa-containers.c',
    'compat/pa-audio.c',
    droid_src / 'common/sllist.c',
    droid_src / 'common/utils.c',
    droid_src / 'common/conversion.c',
    droid_src / 'common/config-parser-xml.c',
    droid_src / 'common/droid-config.c' ],
  include_directories : [compat_inc, droid_inc, android_inc],
  dependencies : [expat_dep, libpulse_dep])

droid_pcm_test = executable('test-droid-pcm',
  ['tests/test-droid-pcm.c', 'tests/hal-stub.c'],
  c_args : ['-DTEST_FIXTURE="' + meson.current_source_dir() / 'tests/audio-policy-fixture.xml' + '"',
            '-DTEST_FIXTURE_NO_PRIMARY="' + meson.current_source_dir() / 'tests/audio-policy-no-primary.xml' + '"'],
  include_directories : [compat_inc, droid_inc, android_inc],
  dependencies : [spa_dep, hybris_dep, hw_dep,
                  cc.find_library('m', required : false),
                  cc.find_library('dl', required : false),
                  cc.find_library('pthread', required : false)],
  link_with : droid_config_only,
  build_by_default : false)
test('droid-pcm', droid_pcm_test)

compat_test = executable('test-compat',
  'tests/test-compat.c',
  include_directories : [compat_inc, droid_inc, android_inc],
  dependencies : [spa_dep, cc.find_library('pthread', required : false)],
  link_with : droid_common,
  build_by_default : false)
test('compat', compat_test)

executable('probe-droid-device', 'tools/probe-droid-device.c',
  dependencies : [spa_dep, cc.find_library('dl', required : false)])

executable('probe-droid-hal', 'tools/probe-droid-hal.c',
  include_directories : [compat_inc, droid_inc, android_inc],
  link_with : droid_common,
  dependencies : [hybris_dep, hw_dep,
                  cc.find_library('pthread', required : false)])

executable('probe-droid-node', 'tools/probe-droid-node.c',
  dependencies : [spa_dep, cc.find_library('dl', required : false)])

# A SCO link of our own, with no PipeWire in between - see tools/probe-bt-sco.c
executable('probe-bt-sco', 'tools/probe-bt-sco.c',
  dependencies : [cc.find_library('bluetooth'),
                  cc.find_library('m', required : false)])

# Does the HAL play to Bluetooth SCO when it is handed the device directly?
executable('probe-bt-sco-out', 'tools/probe-bt-sco-out.c',
  include_directories : [compat_inc, droid_inc, android_inc],
  link_with : droid_common,
  dependencies : [hybris_dep, hw_dep,
                  cc.find_library('m', required : false),
                  cc.find_library('pthread', required : false)])

executable('probe-bt-sco-in', 'tools/probe-bt-sco-in.c',
  include_directories : [compat_inc, droid_inc, android_inc],
  link_with : droid_common,
  dependencies : [hybris_dep, hw_dep,
                  cc.find_library('m', required : false),
                  cc.find_library('pthread', required : false)])

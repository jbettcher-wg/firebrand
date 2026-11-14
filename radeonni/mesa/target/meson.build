# Copyright (c) 2026 kouta-kun and Claude
# SPDX-License-Identifier: MIT
#
# The rdn target: Mesa's GL with r600, radeonsi and the osx-gpu winsys, as
# RadeonNIGLDriver.bundle for Mac OS X. radeonni/build-gl.sh copies this
# directory into the Mesa tree as src/gallium/targets/rdn, with a copy of
# the hardware library's headers in `hw` inside it.

# The device: on Mac OS X the kext's user client, elsewhere (the Linux
# host) the hardware library in this process.
if host_machine.system() == 'darwin'
  # rdn_mem.c: the device divides up its chunks of GART memory itself.
  rdn_device = files('rdn_device_darwin.c', 'hw/rdn_mem.c')
  rdn_device_link_args = ['-framework', 'IOKit', '-framework', 'CoreFoundation']
  rdn_library_link_args = [
    '-Wl,-exported_symbols_list,' + meson.current_source_dir() / 'rdn_gl.exports',
  ]
else
  error('the rdn target is built for Mac OS X only in this tree')
endif

rdn_inc = [
  include_directories('.'), inc_rdn_winsys, inc_rdn_frontend,
  inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_gallium_winsys,
  inc_gallium_drivers, inc_amd_common,
]
rdn_sources = [files('rdn_target.c'), rdn_device]
rdn_link_whole = [librdn_st, libglapi]
rdn_link_with = [libr600, libmesa, libgallium, libgalliumvl, librdnwinsys]
rdn_c_args = []
if with_gallium_radeonsi
  # GCN cards: radeonsi (ACO, no LLVM) and the AMD common code it needs.
  rdn_link_with += radeonsi_gfx_libs + [libradeonsi, amd_common_libs, libamdgpu_addrlib, libvpe, libgm, liblanczos]
  rdn_c_args += ['-DRDN_HAVE_RADEONSI']
endif
rdn_deps = [dep_thread, idep_mesautil, idep_nir]
if with_gallium_radeonsi
  # radeonsi's option cache (driQueryOption*) and its driinfo header's includes.
  rdn_deps += [idep_xmlconfig]
  rdn_inc += [include_directories('../../drivers/radeonsi')]
endif

librdngl = shared_library(
  'rdngl',
  rdn_sources,
  gnu_symbol_visibility : 'hidden',
  c_args : rdn_c_args,
  link_args : [ld_args_gc_sections, rdn_device_link_args, rdn_library_link_args],
  include_directories : rdn_inc,
  link_whole : rdn_link_whole,
  link_with : rdn_link_with,
  dependencies : rdn_deps,
  install : false,
)

# rdnvd: the card's video decoder for programs (rdnvd.h): Mesa's r600 UVD code behind a plain C
# interface, with the H.264 layer of video/. Only the rdnvd_* entry points are exported.
rdnvd_sources = [rdn_sources, files('rdnvd.c', 'video/rdnvd_h264.c')]
rdnvd_link_with = [libr600, libmesa, libgallium, libgalliumvl, librdnwinsys]
rdnvd_args = []
if host_machine.system() == 'darwin'
  # The install name is @rpath/...; a program that keeps the library beside it (a QuickTime component, say)
  # changes it with install_name_tool, since Tiger has no @rpath.
  rdnvd_args = ['-Wl,-exported_symbols_list,' + meson.current_source_dir() / 'rdnvd.exports']
endif
shared_library(
  'rdnvd',
  rdnvd_sources,
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_gc_sections, rdn_device_link_args, rdnvd_args],
  include_directories : rdn_inc,
  link_with : rdnvd_link_with,
  dependencies : rdn_deps,
  install : false,
)

# The OpenGL driver bundle for Mac OS X: Apple's driver interface on the
# outside (gld/RadeonNIGLDriver.c), Mesa inside. The dispatch glue is
# generated from the SDK's header, at the place the toolchain container
# has it. radeonni/build-gl.sh puts the result into a bundle.
if host_machine.system() == 'darwin'
  rdn_dispatch_c = custom_target(
    'rdn_dispatch_gen.c',
    input : 'gld/gen_dispatch.py',
    output : 'rdn_dispatch_gen.c',
    command : [
      prog_python, '@INPUT@',
      meson.get_external_property('rdn_sdk', '/usr/local/MacOSX10.4u.sdk') / 'System/Library/Frameworks/OpenGL.framework/Headers/gliDispatch.h',
      '@OUTPUT@',
    ],
  )
  shared_module(
    'RadeonNIGLDriver',
    files('gld/RadeonNIGLDriver.c', 'gld/rdn_mesa.c', 'gld/rdn_hook.c', 'gld/rdn_arbtext.c', 'gld/rdn_video.c'), rdn_dispatch_c,
    rdn_sources,
    c_args : ['-DRDN_MESA'] + rdn_c_args,
    include_directories : [rdn_inc, include_directories('gld')],
    # Only the gld* entry points leave the bundle. The static libstdc++
    # would otherwise export its weak symbols, and on Mac OS X 10.5 dyld
    # binds them to the system's libstdc++ (another ABI), which then frees
    # pointers it did not allocate during static initialisation.
    link_args : rdn_device_link_args + [
      '-Wl,-exported_symbols_list,' + meson.current_source_dir() / 'gld' / 'rdn_gld.exports',
    ],
    link_whole : rdn_link_whole,
    link_with : rdn_link_with,
    dependencies : rdn_deps,
    name_prefix : '',
    install : false,
  )
endif

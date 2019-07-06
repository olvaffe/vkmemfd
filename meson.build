project(
  'vkmemfd',
  ['c'],
  version : '0.1',
  license : 'MIT',
  default_options : ['c_std=c11']
)

cc = meson.get_compiler('c')
dep_xcb = dependency('xcb')
dep_vulkan = dependency('vulkan')

vkmemfd_files = files(
  'main.c',
  'renderer.c',
  'udmabuf.c',
)

vkmemfd = executable(
  'vkmemfd',
  [vkmemfd_files],
  c_args : ['-D_GNU_SOURCE'],
  dependencies : [dep_xcb, dep_vulkan],
)

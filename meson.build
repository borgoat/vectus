project('vectus', 'c', default_options : ['c_std=c99', 'warning_level=3'])

inc_dir = include_directories('include')
sources = [
    'src/client.c',
    'src/upload.c',
    'src/uploader.c',
]

curl_dep = dependency('libcurl')

libvectus = library('vectus', sources, include_directories : inc_dir, dependencies : curl_dep, install : true)

test_sources = [
    'tests/suite.c',
]

check_dep = dependency('check', include_type : 'system')  # check.h's macros are not -Wpedantic clean

# The tests compile src/client.c themselves, so this sanitizes the code under test without touching the installed library
sanitize = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
if not meson.get_compiler('c').has_multi_link_arguments(sanitize)
    sanitize = []
endif

test_suite = executable('vectus-test', test_sources, include_directories : inc_dir, dependencies : [curl_dep, check_dep], c_args : sanitize, link_args : sanitize)
test('vectus test suite', test_suite)

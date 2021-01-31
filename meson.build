project('vectus', 'c', default_options : ['c_std=c89'])

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

check_dep = dependency('check')

test_suite = executable('vectus-test', test_sources, include_directories : inc_dir, dependencies : [curl_dep, check_dep])
test('vectus test suite', test_suite)

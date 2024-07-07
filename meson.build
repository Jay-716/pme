project(
    'pme',
    'c',
    version: '0.1',
    license: 'MIT',
    default_options: [
        'c_std=c11',
        'warning_level=2',
        'werror=true',
    ],
)

add_project_arguments([
    '-D_POSIX_C_SOURCE=200809L',
    '-Wno-unused-parameter',
    '-Wvla',
], language: 'c')

libnotify = dependency('libnotify')
wayland_client = dependency('wayland-client')
wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols', version: '>=1.27')
wayland_scanner = dependency('wayland-scanner', native: true, version: '>=1.14.91')
wayland_scanner_prog = find_program(wayland_scanner.get_variable('wayland_scanner'), native: true)

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

wayland_scanner_code = generator(
	wayland_scanner_prog,
	output: '@BASENAME@-protocol.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_client = generator(
	wayland_scanner_prog,
	output: '@BASENAME@-client-protocol.h',
	arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)

protos = [
	wl_protocol_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
]

protos_src = []
foreach xml : protos
	protos_src += wayland_scanner_code.process(xml)
	protos_src += wayland_scanner_client.process(xml)
endforeach

executable(
    'pme',
    ['main.c', protos_src,],
    dependencies: [
        wayland_client,
        wayland_server,
        libnotify,
    ],
    install: true,
)


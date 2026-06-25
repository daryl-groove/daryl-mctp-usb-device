project('libmctp', 'c', version: '0.0.0')

# Minimal core-only build: reassembly, fragmentation, routing.
# Excludes bus-specific drivers (USB/PCIe/SMBus/...) and utilities that pull
# in json-c, systemd, or libusb — none of which are needed for a device-side
# FunctionFS binding.  All required feature macros are passed as -D flags so
# the generated config.h (MCTP_HAVE_CONFIG_H) is not needed.
libmctp_core = static_library(
    'mctp',
    ['alloc.c', 'core.c', 'log.c'],
    c_args: [
        '-DMCTP_LOG_STDERR',
        '-DMCTP_HAVE_FILEIO',
        '-DMCTP_HAVE_STDIO',
        '-DMCTP_DEFAULT_ALLOC',
        '-w',   # suppress warnings from third-party source
    ],
    include_directories: include_directories('.'),
)

libmctp_dep = declare_dependency(
    link_with: libmctp_core,
    include_directories: include_directories('.'),
)

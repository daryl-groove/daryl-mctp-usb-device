project(
    'daryl-mctp-usb-device',
    'cpp',
    version: '0.1.0',
    default_options: [
        'cpp_std=c++20',
        'warning_level=2',
    ],
)

# mctp/ = the pure MCTP engine (protocol + DSP0283 USB framing, no I/O);
# ffs/  = the Linux FunctionFS glue (descriptors + ep0/ep event loop), depends
#         on mctp/; app/ = the two daemon entry points. Both engine dirs are on
# the include path so every source keeps unprefixed includes.
engine_inc = include_directories('mctp', 'ffs')

# libmctp-core (Step B): provides reassembly/fragmentation for mctpusbd.
# Try system pkg-config first; fall back to the subprojects/ wrap (NVIDIA fork,
# compiled from source with a minimal core-only meson.build overlay).
libmctp_dep = dependency('libmctp',
    required: false,
    fallback: ['libmctp', 'libmctp_dep'],
)

# The MCTP engine — pure, fully unit-tested off-target.
mctp_engine = files(
    'mctp/mctp_endpoint.cpp',
    'mctp/mctp_packet.cpp',
    'mctp/mctp_control.cpp',
    'mctp/mctp_usb_frame.cpp',
)

# Engine + FunctionFS event loop, shared by both daemon binaries.
ffs_core = files('ffs/ffs_daemon.cpp') + mctp_engine

# ffsd: script-driven (scripts/gadget.sh owns the gadget lifecycle).
executable(
    'ffsd',
    ['app/ffsd.cpp'] + ffs_core,
    include_directories: engine_inc,
    install: false,
)

# mctpusbd: self-contained (libusbg owns the gadget lifecycle in-process) and the
# primary deliverable, so libusbgx is REQUIRED — a missing dependency is a hard
# error, not a skip. libusbgx lives in the OpenBMC cross sysroot; a plain native
# host needs it installed (or build via scripts/cross-test.sh under the SDK).
libusbgx_dep = dependency('libusbgx')
executable(
    'mctpusbd',
    ['app/mctpusbd.cpp', 'mctp/mctp_core_binding.cpp'] + ffs_core,
    include_directories: engine_inc,
    dependencies: [libusbgx_dep, libmctp_dep],
    install: false,
)

# gtest: native has gtest.pc; SDK sysroot bundles it inside gmock.pc (static .a only).
gtest_dep = dependency('gtest', main: true, required: false)
if not gtest_dep.found()
    gtest_dep = dependency('gmock', main: true)
endif

test_descriptors = executable(
    'test_descriptors',
    ['test/test_descriptors.cpp'],
    dependencies: [gtest_dep],
    include_directories: engine_inc,
    install: false,
)
test('descriptors', test_descriptors, protocol: 'gtest')

test_frame = executable(
    'test_frame',
    ['test/test_frame.cpp', 'mctp/mctp_usb_frame.cpp'],
    dependencies: [gtest_dep],
    include_directories: engine_inc,
    install: false,
)
test('frame', test_frame, protocol: 'gtest')

test_control = executable(
    'test_control',
    ['test/test_control.cpp', 'mctp/mctp_control.cpp'],
    dependencies: [gtest_dep],
    include_directories: engine_inc,
    install: false,
)
test('control', test_control, protocol: 'gtest')

test_packet = executable(
    'test_packet',
    ['test/test_packet.cpp', 'mctp/mctp_packet.cpp'],
    dependencies: [gtest_dep],
    include_directories: engine_inc,
    install: false,
)
test('packet', test_packet, protocol: 'gtest')

test_endpoint = executable(
    'test_endpoint',
    [
        'test/test_endpoint.cpp',
        'mctp/mctp_endpoint.cpp',
        'mctp/mctp_packet.cpp',
        'mctp/mctp_control.cpp',
        'mctp/mctp_usb_frame.cpp',
    ],
    dependencies: [gtest_dep],
    include_directories: engine_inc,
    install: false,
)
test('endpoint', test_endpoint, protocol: 'gtest')

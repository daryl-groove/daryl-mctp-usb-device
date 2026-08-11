project('mctpusbd-c', 'c',
  version : '0.1',
  default_options : ['c_std=c17', 'warning_level=2'])

# _DEFAULT_SOURCE exposes htole*/le*toh from <endian.h> on glibc targets.
add_project_arguments('-D_DEFAULT_SOURCE', language : 'c')

# Include paths: mctp/ and ffs/ headers are included unprefixed.
engine_inc = include_directories('mctp', 'ffs')

# ── MCTP engine (pure, no I/O) ────────────────────────────────────────────────
mctp_sources = files(
  'mctp/mctp_usb_frame.c',
  'mctp/mctp_packet.c',
  'mctp/mctp_control.c',
  'mctp/mctp_endpoint.c',
  'mctp/mctp_serial_frame.c',
)

# ── FunctionFS event loop ─────────────────────────────────────────────────────
ffs_sources = files(
  'ffs/ffs_daemon.c',
  'ffs/ffs_pty.c',
)

# Shared engine library (static, reused by both binaries and tests).
engine_lib = static_library('mctp_engine_c',
  mctp_sources + ffs_sources,
  include_directories : engine_inc)

engine_dep = declare_dependency(
  link_with : engine_lib,
  include_directories : engine_inc)

# ── ffsd (FunctionFS test tool, no libusbgx) ──────────────────────────────────
executable('ffsd-c',
  'app/ffsd.c',
  dependencies : engine_dep)

# ── mctpusbd (full daemon with libusbgx) ─────────────────────────────────────
dep_usbgx = dependency('libusbgx', required : false)

if dep_usbgx.found()
  executable('mctpusbd-c',
    'app/mctpusbd.c',
    dependencies : [engine_dep, dep_usbgx])
else
  message('libusbgx not found — mctpusbd-c will not be built (cross-build only)')
endif

# ── Unit tests ────────────────────────────────────────────────────────────────
test_names = ['test_frame', 'test_packet', 'test_control', 'test_endpoint']

foreach t : test_names
  exe = executable(t + '-c',
    'test/' + t + '.c',
    dependencies : engine_dep)
  test(t + '-c', exe)
endforeach

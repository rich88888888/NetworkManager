# SPDX-License-Identifier: LGPL-2.1-or-later

shared_inc = include_directories('.')

nm_default_dep = declare_dependency(include_directories: [top_inc, shared_inc])

glib_nm_default_dep = declare_dependency(dependencies: [glib_dep, nm_default_dep])

libc_siphash = static_library(
  'c-siphash',
  sources: 'c-siphash/src/c-siphash.c',
  include_directories: include_directories('c-stdaux/src'),
)

libc_rbtree = static_library(
  'c-rbtree',
  sources: 'c-rbtree/src/c-rbtree.c',
  include_directories: include_directories('c-stdaux/src'),
  c_args: '-std=c11',
)

sources = files(
  'n-acd/src/n-acd.c',
  'n-acd/src/n-acd-probe.c',
  'n-acd/src/util/timer.c',
)

if enable_ebpf
  sources += files('n-acd/src/n-acd-bpf.c')
else
  sources += files('n-acd/src/n-acd-bpf-fallback.c')
endif

incs = include_directories(
  'c-list/src',
  'c-rbtree/src',
  'c-siphash/src',
  'c-stdaux/src',
)

c_flags = [
  '-D_GNU_SOURCE',
  '-DSO_ATTACH_BPF=50',
  '-std=c11',
  '-Wno-pointer-arith',
  '-Wno-vla',
]

links = [
  libc_rbtree,
  libc_siphash,
]

libn_acd = static_library(
  'n-acd',
  sources: sources,
  include_directories: incs,
  c_args: c_flags,
  link_with: links,
)

libn_acd_dep = declare_dependency(
  include_directories: shared_inc,
  link_with: libn_acd,
)

sources = files(
  'n-dhcp4/src/n-dhcp4-c-connection.c',
  'n-dhcp4/src/n-dhcp4-c-lease.c',
  'n-dhcp4/src/n-dhcp4-client.c',
  'n-dhcp4/src/n-dhcp4-c-probe.c',
  'n-dhcp4/src/n-dhcp4-incoming.c',
  'n-dhcp4/src/n-dhcp4-outgoing.c',
  'n-dhcp4/src/n-dhcp4-socket.c',
  'n-dhcp4/src/util/packet.c',
  'n-dhcp4/src/util/socket.c',
)

incs = include_directories(
  'c-list/src',
  'c-siphash/src',
  'c-stdaux/src',
)

c_flags = [
  '-D_GNU_SOURCE',
  '-Wno-declaration-after-statement',
  '-Wno-pointer-arith',
]

libn_dhcp4 = static_library(
  'n-dhcp4',
  sources: sources,
  c_args: c_flags,
  include_directories: incs,
  link_with: libc_siphash,
)

libn_dhcp4_dep = declare_dependency(
  include_directories: shared_inc,
  link_with: libn_dhcp4,
)

nm_version_macro_header = configure_file(
  input: 'nm-version-macros.h.in',
  output: '@BASENAME@',
  configuration: data_conf,
)

nm_meta_setting_source = files('nm-meta-setting.c')

nm_test_utils_impl_source = files('nm-test-utils-impl.c')

nm_vpn_plugin_utils_source = files('nm-utils/nm-vpn-plugin-utils.c')

libnm_std_aux = static_library(
  'nm-std-aux',
  sources: [
    'nm-std-aux/c-list-util.c',
    'nm-std-aux/nm-std-utils.c',
  ],
  include_directories: top_inc,
  c_args: [
    '-DG_LOG_DOMAIN="@0@"'.format(libnm_name),
    '-DNETWORKMANAGER_COMPILATION=0',
  ],
)

sources = files(
  'nm-glib-aux/nm-dbus-aux.c',
  'nm-glib-aux/nm-dedup-multi.c',
  'nm-glib-aux/nm-enum-utils.c',
  'nm-glib-aux/nm-errno.c',
  'nm-glib-aux/nm-hash-utils.c',
  'nm-glib-aux/nm-io-utils.c',
  'nm-glib-aux/nm-json-aux.c',
  'nm-glib-aux/nm-keyfile-aux.c',
  'nm-glib-aux/nm-logging-base.c',
  'nm-glib-aux/nm-random-utils.c',
  'nm-glib-aux/nm-ref-string.c',
  'nm-glib-aux/nm-secret-utils.c',
  'nm-glib-aux/nm-shared-utils.c',
  'nm-glib-aux/nm-time-utils.c',
)

c_flags = [
  '-DG_LOG_DOMAIN="@0@"'.format(libnm_name),
  '-DNETWORKMANAGER_COMPILATION=(NM_NETWORKMANAGER_COMPILATION_GLIB|NM_NETWORKMANAGER_COMPILATION_WITH_GLIB_I18N_LIB)',
]

links = [
  libc_siphash,
  libnm_std_aux,
]

libnm_utils_base = static_library(
  'nm-utils-base',
  sources: sources,
  dependencies: glib_nm_default_dep,
  c_args: c_flags,
  link_with: links,
)

libnm_utils_base_dep = declare_dependency(
  dependencies: glib_nm_default_dep,
  link_with: libnm_utils_base,
)


libnm_udev_aux = static_library(
  'nm-udev-aux',
  sources: 'nm-udev-aux/nm-udev-utils.c',
  dependencies: [
    glib_nm_default_dep,
    libudev_dep,
  ],
  c_args: c_flags,
)

libnm_udev_aux_dep = declare_dependency(
  include_directories: shared_inc,
  link_with: libnm_udev_aux,
)

libnm_base = static_library(
  'nm-base',
  sources: files(
    'nm-base/nm-ethtool-base.c',
  ),
  dependencies: libnm_utils_base_dep,
  c_args: c_flags,
)

libnm_base_dep = declare_dependency(
  include_directories: shared_inc,
  dependencies: libnm_utils_base_dep,
  link_with: libnm_base,
)

libnm_log_core = static_library(
  'nm-log-core',
  sources: 'nm-log-core/nm-logging.c',
  dependencies: [
    glib_nm_default_dep,
    libsystemd_dep,
  ],
  c_args: [
    '-DG_LOG_DOMAIN="NetworkManager"',
    '-DNETWORKMANAGER_COMPILATION=(NM_NETWORKMANAGER_COMPILATION_GLIB|NM_NETWORKMANAGER_COMPILATION_WITH_GLIB_I18N_LIB)',
  ],
)

libnm_log_core_dep = declare_dependency(
  include_directories: shared_inc,
  dependencies: [
    libnm_utils_base_dep,
  ],
  link_with: libnm_log_core,
)

libnm_platform = static_library(
  'nm-platform',
  sources: [
    'nm-platform/nm-netlink.c',
    'nm-platform/nm-platform-utils.c',
    'nm-platform/nmp-netns.c',
  ],
  dependencies: [
    glib_nm_default_dep,
  ],
  c_args: [
    '-DG_LOG_DOMAIN="NetworkManager"',
    '-DNETWORKMANAGER_COMPILATION=(NM_NETWORKMANAGER_COMPILATION_GLIB|NM_NETWORKMANAGER_COMPILATION_WITH_GLIB_I18N_LIB)',
  ],
)

libnm_platform_dep = declare_dependency(
  include_directories: shared_inc,
  dependencies: [
    libnm_utils_base_dep,
  ],
  link_with: libnm_platform,
)

sources = files(
  'systemd/nm-sd-utils-shared.c',
  'systemd/src/basic/alloc-util.c',
  'systemd/src/basic/env-file.c',
  'systemd/src/basic/env-util.c',
  'systemd/src/basic/escape.c',
  'systemd/src/basic/ether-addr-util.c',
  'systemd/src/basic/extract-word.c',
  'systemd/src/basic/fd-util.c',
  'systemd/src/basic/fileio.c',
  'systemd/src/basic/format-util.c',
  'systemd/src/basic/fs-util.c',
  'systemd/src/basic/hash-funcs.c',
  'systemd/src/basic/hashmap.c',
  'systemd/src/basic/hexdecoct.c',
  'systemd/src/basic/hostname-util.c',
  'systemd/src/basic/in-addr-util.c',
  'systemd/src/basic/io-util.c',
  'systemd/src/basic/memory-util.c',
  'systemd/src/basic/mempool.c',
  'systemd/src/basic/parse-util.c',
  'systemd/src/basic/path-util.c',
  'systemd/src/basic/prioq.c',
  'systemd/src/basic/process-util.c',
  'systemd/src/basic/random-util.c',
  'systemd/src/basic/ratelimit.c',
  'systemd/src/basic/signal-util.c',
  'systemd/src/basic/socket-util.c',
  'systemd/src/basic/stat-util.c',
  'systemd/src/basic/string-table.c',
  'systemd/src/basic/string-util.c',
  'systemd/src/basic/strv.c',
  'systemd/src/basic/strxcpyx.c',
  'systemd/src/basic/time-util.c',
  'systemd/src/basic/tmpfile-util.c',
  'systemd/src/basic/utf8.c',
  'systemd/src/basic/util.c',
  'systemd/src/shared/dns-domain.c',
  'systemd/src/shared/web-util.c',
)

incs = include_directories(
  'systemd/sd-adapt-shared',
  'systemd/src/basic',
  'systemd/src/shared',
)

c_flags = [
  '-DG_LOG_DOMAIN="@0@"'.format(libnm_name),
  '-DNETWORKMANAGER_COMPILATION=NM_NETWORKMANAGER_COMPILATION_SYSTEMD_SHARED',
]

libnm_systemd_shared = static_library(
  'nm-systemd-shared',
  sources: sources,
  include_directories: incs,
  dependencies: glib_nm_default_dep,
  c_args: c_flags,
)

libnm_systemd_shared_dep = declare_dependency(
  include_directories: incs,
  dependencies: glib_dep,
  link_with: libnm_systemd_shared,
)

libnm_systemd_logging_stub = static_library(
  'nm-systemd-logging-stub',
  sources: 'systemd/nm-logging-stub.c',
  dependencies: glib_nm_default_dep,
  c_args: c_flags,
)

if enable_tests
  subdir('nm-glib-aux/tests')
  subdir('nm-platform/tests')
endif

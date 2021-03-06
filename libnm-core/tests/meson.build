# SPDX-License-Identifier: LGPL-2.1-or-later

# test-cert.p12 created with:
#
# openssl pkcs12 -export \
# -in test_key_and_cert.pem \
# -inkey test_key_and_cert.pem \
# -certfile test_ca_cert.pem \
# -name "test-pkcs12" \
# -out test-cert.p12

enum_types = 'nm-core-tests-enum-types'

enum_sources = gnome.mkenums_simple(
  enum_types,
  sources: 'test-general-enums.h',
  identifier_prefix: nm_id_prefix,
  body_prefix: '#include "nm-default.h"',
)

c_flags = [
  '-DNETWORKMANAGER_COMPILATION_TEST',
  '-DNETWORKMANAGER_COMPILATION=NM_NETWORKMANAGER_COMPILATION_LIBNM_CORE',
]

test_cert_dir = join_paths(meson.current_source_dir(), 'certs')

test_units = [
  'test-compare',
  'test-crypto',
  'test-general',
  'test-keyfile',
  'test-secrets',
  'test-setting',
  'test-settings-defaults',
]

foreach test_unit: test_units
  exe = executable(
    'libnm-core-' + test_unit,
    [test_unit + '.c'] + enum_sources,
    dependencies: [
      libnm_keyfile_dep,
      libnm_core_dep,
      libnm_core_nm_default_dep,
      libnm_base_dep,
    ],
    c_args: c_flags,
    link_with: libnm_systemd_logging_stub,
  )

  test(
    'libnm-core/' + test_unit,
    test_script,
    args: test_args + [exe.full_path()],
    timeout: default_test_timeout,
  )
endforeach

project('mc833-p1-187679', 'c',
    version: '0.0.0',
    meson_version: '>= 1.2.0',
    subproject_dir: 'external',
    default_options: {
        'c_std': 'gnu23',
        'buildtype': 'release',
        'warning_level': '3',
        'default_library': 'static',
        'prefer_static': true,
        'b_asneeded': true,
        'b_lundef': true,
        'b_lto': true,
        'b_lto_threads': 0,
        'b_lto_mode': 'default',
        'b_ndebug': 'if-release',
        'b_staticpic': false,
        'b_pie': false,
    },
)

warning_level = get_option('warning_level') == 'everything' ? 5 : get_option('warning_level').to_int()
optimizations_enabled = get_option('optimization') in ['2', '3', 's', 'z']
debugging_enabled = get_option('debug')

# # # # # # # # # #
# COMPILER CHECKS #

add_global_arguments('-D_GNU_SOURCE=1', language: 'c')
cc = meson.get_compiler('c')

required_headers = [
    'netinet/in.h',
    'sys/socket.h',
    'unistd.h',
]
foreach header : required_headers
    cc.check_header(header, required: true)
endforeach

# # # # # # # # # #
# COMPILER FLAGS  #

warnings = []
# Safety warnings
if warning_level >= 1
    warnings += cc.get_supported_arguments(
        '-Wconversion',
        '-Wsign-conversion',
        '-Wformat=2',
        '-Wformat-signedness',
        cc.get_supported_arguments('-Wformat-truncation=2').get(0, '-Wformat-truncation'),
        cc.get_supported_arguments('-Wformat-overflow=2').get(0, '-Wformat-overflow'),
        checked: 'warn',
    ) + cc.get_supported_arguments(
        '-Wstringop-overflow=4', # GCC only
        checked: 'off',
    )
endif
# Extra warnings
if warning_level >= 2
    warnings += cc.get_supported_arguments(
        '-Wunused-result',
        checked: 'warn',
    )
endif
# Pedantic warnings
if warning_level >= 3
    warnings += cc.get_supported_arguments(
        optimizations_enabled ? '-Winline' : [],
        checked: 'warn',
    ) + cc.get_supported_arguments(
        optimizations_enabled ? '-Rpass=.*' : [], # Clang only
        optimizations_enabled ? '-Rpass-missed=.*' : [], # Clang only
        checked: 'off',
    )
endif

optimizations = []
if optimizations_enabled
    optimizations += cc.get_supported_arguments(
        not meson.is_cross_build() ? '-march=native' : [],
        not meson.is_cross_build() ? '-mtune=native' : [],
        not get_option('b_staticpic') ? '-fno-pic' : [],
        not get_option('b_pie') ? '-fno-pie' : [],
        not get_option('b_pie') ? '-fno-plt' : [],
        '-fdata-sections',
        '-ffunction-sections',
        '-fno-exceptions',
        '-ffast-math',
        # '-freg-struct-return',
        # '-fshort-enums',
        checked: 'warn',
    ) + cc.get_supported_arguments(
        get_option('b_lto_mode') == 'default' ? '-flto=full' : [], # Clang only
        '-fallow-store-data-races', # GCC only
        '-fwhole-program', # GCC only
        checked: 'off',
    )
endif

general_codegen = cc.get_supported_arguments(
    '-pipe',
    '-fvisibility=hidden',
    debugging_enabled ? '-fstrict-overflow' : '-fno-strict-overflow',
    '-D_FORTIFY_SOURCE=@0@'.format(debugging_enabled ? 3 : 1),
    checked: 'warn',
)

debugging = []
if debugging_enabled
    debugging += cc.get_supported_arguments(
        '-DDEBUG',
        '-ggdb',
        '-g3',
        '-fstack-clash-protection',
        '-fcf-protection',
        '-ftrapv',
        '-fno-omit-frame-pointer',
        '-mno-omit-leaf-frame-pointer',
        checked: 'warn',
    )
endif

linker_options = cc.get_supported_link_arguments(
    optimizations_enabled ? '-Wl,-O1' : [],
    '-Wl,--gc-sections',
    '-Wl,--sort-common',
    '-Wl,--as-needed',
    '-Wl,-z,relro',
    '-Wl,-z,now',
    '-Wl,-z,pack-relative-relocs'
)

# # # # # # # # #
# DEPENDENCIES  #

sqlite3 = subproject('sqlite3',
        default_options: {
            'strict': debugging_enabled,
            'all-extensions': 'disabled',
        },
        required: true,
        version: ['>=3.49.1', '<4.0.0'],
    ) \
    .get_variable('sqlite3_dep') \
    .partial_dependency(
        compile_args: true,
        link_args: true,
        sources: true,
        includes: true,
        links: false,
    ) \
    .as_system('system')

sqlite3_overrides = [
    '-DSQLITE_API=',
]

# # # # # #
# TARGETS #

executable('main', files('src/main.c'),
    include_directories: include_directories('src/'),
    c_args: warnings + optimizations + general_codegen + debugging + sqlite3_overrides,
    link_args: linker_options,
    gnu_symbol_visibility: 'hidden',
    export_dynamic: false,
    dependencies: sqlite3,
)

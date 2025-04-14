project('mc833-p1-187679', 'c',
    version: '0.0.0',
    meson_version: '>= 1.2.0',
    subproject_dir: 'external',
    default_options: {
        'c_std': 'gnu23',
        'buildtype': 'release',
        'warning_level': '2',
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
        '-Wno-nullability-extension', # Clang only
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
        sources: false,
        includes: true,
        links: false,
    ) \
    .as_system('system')

# See https://www.sqlite.org/compile.html
sqlite3_overrides = {
    'HAVE_FDATASYNC': 1,
    'HAVE_GMTIME_R': 1,
    'HAVE_ISNAN': 1,
    'HAVE_LOCALTIME_R': 1,
    'HAVE_MALLOC_USABLE_SIZE': 1,
    'HAVE_STRCHRNUL': 1,
    'HAVE_UTIME': 1,
    'SQLITE_API': '__attribute__((fastcall)) static',
    'SQLITE_APICALL': '__attribute__((fastcall))',
    'SQLITE_EXTERN': '__attribute__((fastcall)) static',
    'SQLITE_CALLBACK': '__attribute__((fastcall)) static',
    'SQLITE_CDECL': '__attribute__((fastcall))',
    'SQLITE_SYSAPI': '__attribute__((fastcall))',
    'SQLITE_DEFAULT_AUTOMATIC_INDEX': 0,
    # 'SQLITE_DEFAULT_AUTOVACUUM': 2,
    'SQLITE_DEFAULT_FILE_PERMISSIONS': 0o600,
    'SQLITE_DEFAULT_FOREIGN_KEYS': 1,
    'SQLITE_DEFAULT_LOCKING_MODE': 0,
    'SQLITE_DEFAULT_MEMSTATUS': 0,
    'SQLITE_DEFAULT_SYNCHRONOUS': 1,
    'SQLITE_DEFAULT_WAL_SYNCHRONOUS': 1,
    'SQLITE_DIRECT_OVERFLOW_READ': 1,
    'SQLITE_DISABLE_PAGECACHE_OVERFLOW_STATS': 1,
    'SQLITE_DQS': 0,
    'SQLITE_ENABLE_SORTER_REFERENCES': 1,
    'SQLITE_ENABLE_UNKNOWN_SQL_FUNCTION': 0,
    'SQLITE_LIKE_DOESNT_MATCH_BLOBS': 1,
    'SQLITE_MAX_EXPR_DEPTH': 0,
    'SQLITE_OMIT_AUTHORIZATION': 1,
    'SQLITE_OMIT_AUTOINIT': 1,
    'SQLITE_OMIT_AUTOMATIC_INDEX': 1,
    'SQLITE_OMIT_AUTORESET': 1,
    'SQLITE_OMIT_BETWEEN_OPTIMIZATION': 1,
    'SQLITE_OMIT_BLOB_LITERAL': 1,
    'SQLITE_OMIT_CASE_SENSITIVE_LIKE_PRAGMA': 1,
    'SQLITE_OMIT_CAST': 1,
    'SQLITE_OMIT_COMPILEOPTION_DIAGS': 1,
    'SQLITE_OMIT_COMPLETE': 1,
    'SQLITE_OMIT_COMPOUND_SELECT': 1,
    'SQLITE_OMIT_CTE': 1,
    'SQLITE_OMIT_DATETIME_FUNCS': 1,
    'SQLITE_OMIT_DECLTYPE': 1,
    'SQLITE_OMIT_DEPRECATED': 1,
    'SQLITE_OMIT_DESERIALIZE': 1,
    'SQLITE_OMIT_FLOATING_POINT': 1,
    'SQLITE_OMIT_GENERATED_COLUMNS': 1,
    'SQLITE_OMIT_GET_TABLE': 1,
    'SQLITE_OMIT_INCRBLOB': 1,
    'SQLITE_OMIT_INTROSPECTION_PRAGMAS': 1,
    'SQLITE_OMIT_LIKE_OPTIMIZATION': 1,
    'SQLITE_OMIT_LOAD_EXTENSION': 1,
    'SQLITE_OMIT_LOCALTIME': 1,
    'SQLITE_OMIT_MEMORYDB': 1,
    'SQLITE_OMIT_OR_OPTIMIZATION': 1,
    'SQLITE_OMIT_PROGRESS_CALLBACK': 1,
    'SQLITE_OMIT_SHARED_CACHE': 1,
    'SQLITE_OMIT_TCL_VARIABLE': 1,
    'SQLITE_OMIT_TEMPDB': 1,
    'SQLITE_OMIT_UTF16': 1,
    # 'SQLITE_OMIT_VACUUM': 1,
    'SQLITE_OMIT_WINDOWFUNC': 1,
    'SQLITE_POWERSAFE_OVERWRITE': 1,
    'SQLITE_STRICT_SUBTYPE': 1,
    'SQLITE_TEMP_STORE': 3,
    'SQLITE_THREADSAFE': 2,
    'SQLITE_UNTESTABLE': 1,
    'SQLITE_USE_ALLOCA': 1,
    ## buggy SQLite compile options
    # 'SQLITE_OMIT_ALTERTABLE': 1,
    # 'SQLITE_OMIT_ANALYZE': 1,
    # 'SQLITE_OMIT_ATTACH': 1,
    # 'SQLITE_OMIT_REINDEX': 1,
    # 'SQLITE_OMIT_SUBQUERY': 1,
    # 'SQLITE_OMIT_VIEW': 1,
    # 'SQLITE_OMIT_VIRTUALTABLE': 1,
}
if debugging_enabled
    sqlite3_overrides += {
        'SQLITE_DEBUG': 1,
        'SQLITE_ENABLE_API_ARMOR': 1,
        'SQLITE_REVERSE_UNORDERED_SELECTS': 1,
        'SQLITE_TRUSTED_SCHEMA': 0,
    }
endif
if optimizations_enabled
    sqlite3_overrides += {
        'SQLITE_OMIT_EXPLAIN': 1,
        'SQLITE_OMIT_TRACE': 1,
        'SQLITE_TRUSTED_SCHEMA': 1,
    }
endif

sqlite3_flags = []
foreach option, value : sqlite3_overrides
    sqlite3_flags += [f'-D@option@=@value@']
endforeach

# # # # # #
# TARGETS #

executable('main', files('src/main.c', 'src/database/database.c'),
    include_directories: include_directories('src/'),
    c_args: warnings + optimizations + general_codegen + debugging + sqlite3_flags,
    link_args: linker_options,
    gnu_symbol_visibility: 'hidden',
    export_dynamic: false,
    dependencies: sqlite3,
)

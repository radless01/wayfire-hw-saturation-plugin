shared_module(
    'hw_saturation',
    sources: files(
        'hw_saturation.cpp',
    ),

    dependencies: [
        wayfire,
        libdrm,
    ],

    install: true,
    install_dir: wayfire.get_variable(pkgconfig: 'plugindir'),

    name_prefix: 'lib',

    cpp_args: [
        '-D_FILE_OFFSET_BITS=64',
    ],
)

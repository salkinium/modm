# modm command line interface




```
modm [-h, --help] [-v, --verbose] [--version] [--plain] ACTIONS
```

General options for all subactions

```
modm init [--release RELEASE, --head] [REPO]
```

Download latest modm release to `REPO` path and generate `modm.xml` file at the
current location 

```
modm new [--board BOARD, --mcu MCU] [-m, --module MODULES] [--example EXAMPLE] [--build-path PATH] [PATH]
```

```
modm import [--cubemx] SETTINGS
```


- discover-releases [RELEASE]
- discover-examples [NAME]
- discover-boards [NAME]

- build [--profile PROFILE] => scons/cmake build
- upload [--profile PROFILE] => scons/cmake upload

- discover-modules => lbuild discover
- discover-options => lbuild discover-options
- validate => lbuild validate
- update => lbuild build



lbuild [-h] [-r REPO] [-c CONFIG] [-p PATH] [-D OPTION] [-v] [--plain] [--version]
- discover
- discover-options
- validate
- build
- clean
- init
- update
- dependencies
# Build Systems

For modm, build systems are not special and are treated as just another module
that *generates* the build system configuration as part of your custom library.

The required data for this is a list of generated files provided by lbuild itself,
and lots of metadata like header include paths, compile flags, preprocessor definitions,
libraries to be linked, that is collected from each module.

This information is then assembled by the [`modm:build` module](../module/modm-build)
which generates generic support files and data for providing compilation,
uploading and debugging functionality.

This alone is not enough to compile your project though, so you have to select
one or more `modm:build:*` submodules, that translates this data for the
specific build system.

!!! warning "Build System do not interoperate"
	While you can generate and use multiple build systems at the same time, they
	do not have the same feature set and especially do not interoperate. So you
	cannot compile your program with one build system and upload the firmware to
	your target with another.

!!! info "Your build system"
	A lot of the difficult work is done by the `modm:build`, so adding another
	build system generator is not that difficult. Feel free to open up an PR that
	adds support for your build system and we'll walk you through all the
	necessary steps.


## SCons

The `modm:build:scons` module extends the default SCons build system with many
custom utilities for a smooth integration of embedded tools.
The SCons build system is our preferred and recommented one and features many
very useful additional modm- and embedded-specific tools.
It is however more difficult

[Read the module documentation for all the details](../module/modm-build-scons).


## CMake

The `modm:build:cmake` module generates a CMake build script, which you can
import into a lot of IDEs and compile it from there.
This module ships with a Makefile that wraps all of the CMake commands.

[Read the module documentation for all the details](../module/modm-build-cmake).


## Customization

All build system modules have many lbuild options, which you can set in your
`project.xml` configuration file. However, there are also lbuild collectors,
which are the mechanism used by the library modules themselves to communicate
the required metadata to the build modules.

You too can specify these in your `project.xml` file to customize your build!
For example, if you disagree with our compile flags you can extend them like so:

```xml
<library>
  <collectors>
  	<!-- Warn about strict ISO C and C++ usage -->
  	<collect name="modm:build:ccflags">-Wpedantic</collect>
  	<!-- Don't warn about double promotion, I know what I'm doing (no, you don't) -->
  	<collect name="modm:build:ccflags">-Wno-double-promotion</collect>
  </collectors>
</library>
```

This can be significantly easier than manually editing the generated build
scripts, and is also persistent across library regeneration.
Please see the [`modm:build` documentation](../module/modm-build/#collectors)
for the available collectors.

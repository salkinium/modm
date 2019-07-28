# Module Documentation

Since modm supports several hundred different microcontroller targets of 
different architectures using the modular code generation engine in lbuild,
its documentation is more complex than a typical C++ embedded library. 

Documentation is available before and after code generation:

1. Module descriptions: shows at a high-level what the module does and how it
   can be configured. This documentation is shown by [`lbuild discover`](../../guide/getting-started#discovering-modm)
   *before* code generation.
2. Doxygen: The generated C/C++ code is documented using traditional Doxygen 
   grouped by the module scope. This documentation is enabled by the 
   [`modm:docs`](../module/modm-docs) module *after* code generation.

The Doxygen documentation contains the module descriptions as part of the group
documentation, which is organized in the same hierarchy as shown in `lbuild discover`.

!!! info "All documentation is target-specific"
	Each target has different modules available and generates different code.
	Please take care to check `lbuild discover` and generate the Doxygen
	documentation for your specific target.


## Custom Documentation

The best way to read the documentation is to include the `modm:docs` module in
your `project.xml` configuration and generate the library. Note that you must
*manually* call Doxygen, which can take several minutes, and compiles the
target-specific documentation into `modm/docs/html`:

```sh
lbuild build -m "modm:docs"
(cd modm/docs && doxygen doxyfile.cfg)
# then open: modm/docs/html/index.html
```


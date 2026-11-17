`jemalloc.heap` is an actual dump captured on macOS from locally cached
tikv-jemalloc-sys 0.7.1's jemalloc (5.3.1 development revision
81034ce1f1373e37dc865038e1bc8eeecf559ce8), built in /private/tmp with
`--enable-prof --disable-shared --with-jemalloc-prefix=je_`.

The capture program allocated 1048576 bytes with je_malloc, called
je_mallctl("prof.dump", NULL, NULL, &path, sizeof(path)), then freed it.
It ran with JE_MALLOC_CONF=prof:true,prof_active:true,lg_prof_sample:19.
The raw mapped-library section is retained. Linux mapping conversion is tested
separately with synthetic /proc/self/maps, since macOS uses a different format.
This cache copy is only a fixture generator, not a new hogd dependency.

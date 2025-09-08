
![](./logo.svg "X23 logo")

# RPC and XDR in C++23

X23 is an implementation of the Internet-standard [XDR][] interface
definition language and [RPC][] remote procedure call protocol for
linux in C++23.  X23 makes extensive use of modern C++
features--`std::expected`, explicit member functions, concepts,
coroutines, and explicit template parameters for lambdas--which
simplifies code and improves performance.  Until your compiler
supports [reflection][], X23 is likely to remain the most advanced
XDR/RPC system you can get.

## Installation

X23 uses cmake.  To install it, you need flex, bison, and either gcc
14.2 or later or clang and libc++ 18.1 or later.  Run the following
commands from a fresh checkout:

```
cmake .
make
sudo make install
```

These commands will install the `x23c` compiler and X23 headers and
libraries on your system.  Then, to compile an XDR file defining data
types, run:

```
x23c file.x
```

`x23c` will create a `file.hh` containing the C++23 representation of
your XDR types.  To compile source that includes `file.hh`, you will
want to make sure you have a C++23 compiler and add
`-I/usr/local/include` to your `CXXFLAGS` as well as `-L/usr/local/lib
-lx23` to your `LIBS` or `LDADD`.

To ensure you have a C++23 compiler, you will likely want to use
either `g++ -std=c++23` or `clang++ -std=c++23 -stdlib=libc++
-fexperimental-library`.  (It does not work to use `clang++` with
GCC's `libstdc++` or `g++` with LLVM's `libc++`.)

### Integration with cmake

There are two ways to integrate X23 with a cmake-based project.

1. You can add X23 as a submodule to your project, in a subdirectory
   of your project root called `x23`.  In that case, your top-level
   `CMakeLists.txt` should start something like this:

    ```
    cmake_minimum_required(VERSION 3.28)
    project(myproj)

    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(STATUS "Using Clang, setting libc++")
        add_compile_options(-stdlib=libc++ -fexperimental-library)
        add_link_options(-stdlib=libc++ -fexperimental-library)
    endif()

    add_subdirectory(x23)
    ```

1. The other way to use X23 is assuming it has been installed on your
   system, in which case your top-level `CMakeLists.txt` file can look
   like this:

    ```
    cmake_minimum_required(VERSION 3.30)
    project(cmktest)

    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    # Only necessary if you installed x23 someplace non-standard
    # list(APPEND CMAKE_PREFIX_PATH "/usr/local")

    find_package(x23)
    ```

To compile XDR-format files ending `.x` into header files, you also
have two options.

1. The first option is to list `.x` files like normal source files in
   the sources of an executable or library, and then to apply the
   function `use_x23` to the target.  For example:

    ```
    add_executable(test test.cc myprot.x)
    use_x23(test)
    ```

1. The other option is to use the `add_xdr` function to create an
   "interface library" out of one or more `.x` files, and then to link
   against this library.  The interface library does not actually
   produce any object code, but linking against it will appropriately
   adjust your include path and add the X23 library.  For example:

    ```
    add_xdr(protocol myprot.x)
    add_executable(test test.cc)
    target_link_libraries(test protocol)
    ```

## XDR types

The C++ representations of XDR types should mostly be intuitive.

| XDR type | C++23 type |
|:---------|:-----------|
| `bool` | `bool` (see below) |
| `int` | `std::int32_t` |
| `unsigned int` | `std::uint32_t` |
| `hyper` | `std::int64_t` |
| `unsigned hyper` | `std::uint64_t` |
| `float` | `float` |
| `double` | `double` |
| `quadruple` | `quadruple` (but unimplemented) |
| `string<N>` | `x23::BoundedStr<N>` |
| `opaque[N]` | `std::array<std::uint8_t,N>` |
| `opaque<N>` | `x23::Vec<std::uint8_t,N>` |
| `T*` | `x23::Ptr<T>` |
| `T[N]` | `std::array<T,N>` |
| `T<N>` | `x23::Vec<T,N>` |
| `enum` | `enum` |
| `struct` | `struct` |
| `union` | `struct` (see below) |

* `bool` compiles to `bool`, but don't forget that in the XDR language
  the two values are `TRUE` and `FALSE` (capital), which `x23c`
  compiles to the C++ lower-case `true` and `false` values (e.g., in
  `case` statements).

* `x23::BoundedStr` is similar to a `std::string`, but supports a
bounded string size as a template argument.  You can also use the type
`x23::Str` for a string with the maximum bound.  You can create an
`x23::Str` with the function `x23::uformat` (which is similar to
`std::format`), and you can append to a `x23::Str s` or a `std::string
s` with `x23::uformat_to(&s, "format-string", ...)`.

* `x23::Vec<T,N>` is similar to `std::vector<T>`, but with a maximum
  number of elements `N`.  If `N` is omitted, it defaults to the
  maximum bound.

* `x23::Ptr` is a cross between `std::unique_ptr` and `std::optional`.
  Like `std::unique_ptr`, a struct `S` can contain fields of type
  `Ptr<S>`.  Like `std::optional`, however, it has a copy constructor
  and performs deep copies.

* XDR unions are compiled to C++23 structures, but the fields of the
  union are all methods.  To set the discriminant, you must pass the
  new value to the discriminant method.  For other fields, the method
  returns a reference you can set.  For example, given the xdr:

    ```
    union U switch (bool big) {
    case FALSE:
      int i;
    case TRUE:
      hyper h;
    }
    ```

    You can access the fields as follows:

    ``` cpp
    U u;
    u.big(false);
    u.i() = 23;
    u.big(true);
    u.h() = std::int64_t(1) << 33;
    assert(u.big() == true);
    ```

## XDR extensions

X23 implements two extensions to the XDR language specified in
[RFC4506][XDR].

1. X23 allows definitions to be enclosed in a C++ `namespace {`
   ... `}` block.  The reason is that to specialize various trait
   objects (such as `std::tuple_element`), the compiler needs to know
   where it is in the namespace hierarchy, which wouldn't work if you
   just wrapped a `#include` directive in a namespace block.  To
   ensure your XDR file is not hard to use with other languages and
   implementations, use this extension once per file, not for
   different groups of definitions.

1. Any line starting with a literal `%` character is copied literally
   into the output.  This can be used to insert a `#include`
   directive, if certain compiled XDR files need to include other
   compiled XDR files.

In addition to these two extensions, `x23c` is more permissive than
the specification about a few minor points, like having a trailing
comma in an enum or grouping other cases with a default statement.  If
you want to be strictly conforming, you can run `x23c` with the
`--strict` flag to turn warnings about such non-conforming code into
errors.

## Serialization

Serialization is available in the header file `<x23/serialize.h>`,
which supplies the following functions:

* `Buf serialize(const is_xdr_type auto &...x)` serializes zero or
  more XDR datatypes to `Buf`, which is just a `Vec<std::uint8_t>`.

* `void deserialize(is_xdr_type auto *out, BufViewEnd buf)`
  deserializes bytes into an XDR data type `*out`, and throws an
  exception if all bytes are not consumed.  A `BufViewEnd` is derived
  from `BufView` which is derived from `std::span<const
  std::uint8_t>`, and can be initialized from a `Buf` or `BufView`
  (optionally with the buffer into which the `BufView` is a view).  It
  keeps track of the capacity of the buffer, which allows an
  optimization where deserialization elides some length checks to read
  beyond the size of the buffer up to its capacity (before of course
  throwing an exception if the buffer was too short).

* `size_t deserialize_from(is_xdr_type auto *out, BufViewEnd buf)`
  deserializes and returns the number of bytes consumed.  It does not
  throw an exception if the there are bytes remaining in `buf`.  If it
  returns `n`, subsequent XDR types can be deserialized from
  `buf.subspan(n)`.

Deserialization and in some cases Serialization throw exceptions on
invalid input.  All of these exceptions are derived from
`XdrException`, which itself is derived from `std::exception`.

## Debugging

Datatypes generated by `x23c`, particularly unions, can be hard to
examine in the debugger because of lots of extraneous fields.  If you
are debugging X23 applications with `gdb`, you will want to load the
X23 gdb extensions.  If X23 is installed system-wide, you can do this
by putting the following line in your `~/.gdbinit` file:

```
python import x23_gdb
```

Otherwise, you will need to load the file `x23/x23_gdb.py` from the
X23 source directory, by running the gdb command:

```
source /path/to/x23/x23/x23_gdb.py
```

If you want to debug interactions between clients and servers, you
will want to set a log file descriptor on all your connections.  You
can do this with the following line of code in `main`, assuming you
also create your `RpcStreams` with `rpc_stream_logger` (see below).

``` cpp
x23::RpcLogger::default_logfd = 2;
```

## Coroutines and `Async<T>`

X23 supports asynchronous remote procedure calls using coroutines.
The result of an asynchronous remote procedure call is not available
immediately on clients.  On the server side, the implementation of an
RPC service may also need to contact remote machines.  Hence, when an
XDR file declares a function to return type `T`, the asynchronous X23
implementation must return an `x23::Async<T>`.

Any function returning an `Async<T>` can be a coroutine, so long as it
uses the C++ keyword `co_return` to return its result, rather than
`return`.  Any coroutine can use the keyword `co_await` to get the
result of an `Async<T>`, which will suspend the coroutine if the
result is not ready, and resume it once the result is ready.  As a
trivial example (which never suspends a coroutine):

``` cpp
Async<int>
get5()
{
  co_return 5;
}

Async<int>
get10()
{
  co_return (co_await get5()) + (co_await get5());
}
```

In a coroutine, you should almost always use `co_await` to retrieve
the result of an `Async`.  In non-coroutine code, you have two
options to get a value from an `Async`:

* Put the current thread to sleep and synchronously wait for the
  result to be ready, by invoking the `Async` with no arguments:

    ``` cpp
      std::println("The result is {}", get10()());
    ```

* Pass the `Async` a callback to be invoked later when the result is
  ready:

    ``` cpp
      get10()([](int result) {
        std::println("The result is {}", result);
      });
    ```

## RPC services

The [RPC language][RPC-language] specifies that RPC interfaces are
defined by a `version` block inside a `program` block.  The program
and version are both specified by 32-bit unsigned numbers.  Within a
version, functions are also each assigned a 32-bit number.  The
[specification][RPC-assignment] suggests choosing program numbers in
the range 0x20000000 - 0x2fffffff for programs that have not been
officially registered with IANA.  For example:

```
program myprog {
  version myvers {
    void null(void) = 0;
    int add(int, int) = 1;
  } = 1;
} = 0x21231231;
```

`x23c` generates a placeholder type named like the version (`myvers`)
to represent the interface.  To implement this RPC service, you must
define a class with methods of the same argument types, but that
return an `Async` result.  For example:

``` cpp
using x23::Async;

struct MyVersImpl {
  using rpc_service = myvers;
  Async<void> null() { co_return; }
  Async<std::int32_t> add(std::int32_t a, std::int32_t b)
  {
    // Note: signed arithmetic overflow would be undefined behavior,
    // which we shouldn't allow a client to trigger.  However, as of
    // C++20, casting unsigned to signed is always well-defined.
    co_return std::int32_t(std::uint32_t(a) + std::uint32_t(b));
  }
};
static_assert(is_myvers<MyVersImpl*, Async>);
```

The `static_assert` checks that your `MyVersImpl` type properly
implements the `myvers` interface, with result types transformed by
`Async`.  It causes the compiler to provide a more useful error
message about which method is missing or has the wrong type than if
you just tried to use an implementation that didn't quite implement
the interface.

The `rpc_service` type definition acts as a default interface in some
cases, but is not necessary.  A single structure can implement
multiple RPC programs and versions.

Note that for short RPC functions that always run to completion, you
do not need to return an `Async<T>`, but rather can return the
underlying type `T` to save the overhead of allocating a coroutine
frame.  For example, if `null()` never needs to suspend, fast, it's
valid to say:

``` cpp
using x23::Async;

struct MyVersImpl {
  using rpc_service = myvers;
  void null() {}
  Async<std::int32_t> add(std::int32_t a, std::int32_t b)
  {
    // ...
  }
};
static_assert(is_myvers<MyVersImpl*, Async>);
```

You may also have a method that is fast most of the time and needs to
suspend occasionally.  In that case, you can return a `MaybeAsync<T>`
which can be constructed from either a `T` or an `Async<T>`.  For
example:

``` cpp
using x23::Async;
using x23::MaybeAsync;

std::optional<std::int32_t> cached_add(std::int32_t a, std::int32_t b);
Async<std::int32_t> slow_add(std::int32_t a, std::int32_t b);

struct MyVersImpl {
  using rpc_service = myvers;
  void null() {}
  MaybeAsync<std::int32_t> add(std::int32_t a, std::int32_t b)
  {
    if (auto sum = cached_add(a, b))
      return *sum;
    return slow_add(a, b);  // no co_await, just return the Async
  }
};
static_assert(is_myvers<MyVersImpl*, Async>);
```

## Using asynchronous RPC over stream sockets

To invoke and serve RPC services over a stream socket, include the
header file `<x23/rpc.h>`.  You hand a socket file descriptor to the
X23 runtime by means of the functions `rpc_stream` or
`rpc_stream_logger` (the latter of which logs a trace of all RPC calls
if you set `x23::RpcLogger::default_logfd` to an output file
descriptor such as 1).  These functions return a `Configurator` object
that can then be used to configure the stream, first by declaring
services to be provided (if any), then by creating client objects.

The asynchronous RPC system requires two global objects: a
`ThreadPool` and a `CoroPoll`.  It generally suffices to have a single
global instance of these types, though each `CoroPoll` has a single
epoll socket, so under extreme scalability requirements it could be
useful to have multiple `CoroPoll` instances.

Here's an example of using the client interface to invoke an RPC:

``` cpp
#include <x23/rpc.h>
#include "myfile.hh"

x23::ThreadPool pool;
x23::CoroPoll cp(pool);

int
client_example(int fd)
{
  auto cstream = rpc_stream_logged(cp, fd);
  // Now X23 owns the file descriptor

  myvers::client client = cstream->client(myvers{});
  return client.add(5, 5)(); // second set of parens to wait synchronously
  // file descriptor will be closed on destruction of client
}
```

On the server side, you must register a service as follows:

``` cpp
MyVersImpl impl;

void
server_example(int fd)
{
  rpc_stream_logged(cp, fd)->server(&impl);
}
```

You can only call `rpc_stream` or `rpc_stream_logged` once on a
particular file descriptor, but you can always assign the
`Configurator` to a variable to call `server` multiple times, or
simply chain calls.  For instance if `MyVersImpl*` implemented
multiple interface versions, you could say:

``` cpp
  rpc_stream_logged(cp, fd)->server(&impl, myvers{})
                           ->server(&impl, myvers2{});
```

Note that before X23 will read from a socket, you must either destroy
the configurator object (as in the above server examples, where the
result of `rpc_stream_logged` is not assigned to a variable), or else
issue an RPC as a client, or else call the `go` method on the stream
object.  If you forget to do this, your code will get stuck and not do
anything.  The reason for this requirement is to avoid a race
condition where X23 would reject calls for services that have not yet
been configured on the socket.

## Using synchronous RPC over stream sockets

If you don't want coroutines, there is also a *synchronous* rpc
interface defined in `<x23/srpc.h>`.  In the server, simply define
methods that match the return types of RPC methods (without wrapping
them in `Async` the way you do for asynchronous servers).  Note that
the synchronous server RPC reads and writes RPCs in batches, and
expects all methods to run to completion.  Methods that sleep or take
a long time to execute will cause head-of-line blocking on the file
descriptor.  Other than that, configuration is similar to asynchronous
RPC except that you use `srpc_stream_logged` instead of
`rpc_stream_logged`.

``` cpp
struct MyVersImpl {
  using rpc_service = myvers;
  void null() { return; }
  std::int32_t add(std::int32_t a, std::int32_t b)
  {
    return std::int32_t(std::uint32_t(a) + std::uint32_t(b));
  }
};
static_assert(is_myvers<MyVersImpl*, std::type_identity_t>);

MyVersImpl impl;

void
server_example(int fd)
{
  srpc_stream_logged(fd)->server(&impl);
}
```

Here's an example of using the synchronous client interface:

``` cpp
#include <x23/srpc.h>
#include "myfile.hh"

int
client_example(int fd)
{
  auto cstream = srpc_stream_logged(fd);
  // Now X23 owns the file descriptor

  myvers::client client = cstream->client(myvers{});
  return client.add(5, 5);
  // file descriptor will be closed on destruction of client
}
```

[XDR]: https://datatracker.ietf.org/doc/html/rfc4506
[RPC]: https://datatracker.ietf.org/doc/html/rfc5531
[RPC-assignment]: https://datatracker.ietf.org/doc/html/rfc5531#section-8.3
[RPC-language]: https://datatracker.ietf.org/doc/html/rfc5531#section-12
[reflection]: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2996r0.html


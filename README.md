# Really Easy Build System

REBS (Really Easy Build System) is a build system intended for C and C++ packages. It is intended to build packages from a directory of C/C++ source code with little-to-no configuration required.

## Usage

### Basic usage

To build and run packages that use the C++ standard library, you can just `cd` into a directory containing recognized source files (.c, .cpp, .cc, .asm, etc) and run `rebs`. The C++ sources in this directory and all subdirectories will build, and if successful, they resulting program will run.

Some other commands are supported such as:
* `--clean` - Removes all source files for the package.
* `--build` - Just builds the package, but does not run it.
* `--run` - Builds, and if successful, runs the package. (Default.)
* `--test` - Builds and run unit tests.

The above commands can be ran with a combination of:
* `--debug` - Builds with no optimizations and debug symbols embedded.
* `--fast` - Builds with default optimizations. (Default.)
* `--optimized` - Builds with aggressive whole program optimization. (Slow.)

You can also override the environment being build for:
* `--os=` - The OS being targetted.
* `--arch=` - The architecture being targetted.

For more usage arguments, pass `--help`.

### Configuring packages

Often, packages need to use more than just the C++ standard library. The root directory of a package can contain a `.package.rebs.jsonet` file. REBS uses [Jsonett](https://jsonnet.org/) for configuring files.

For example, if your package depends on [SDL](https://www.libsdl.org/), assuming you have it installed somewhere where REBS can find it (more on that later), you can create a `.package.rebs.jsonet` containing:

```
{
  dependencies+: [
    "SDL",
  ],
}
```

(The `+:` is Jsonett syntax meaning to concatinate `dependencies` with the existing default `dependencies`, which tend to be system libraries. Without the `+`, the system dependencies may not be included.)

You can include pre-processor definitions:

```
{
  defines+: [
    "PREPROCESS_DEFINITION",
    "SOME_OTHER_PREPROCESSOR_DEFINITION=1",
  ]
}
```

If a define starts with `-`, then it removes this pre-processor definiton. For example, if a dependency exposes a public define but you want to remove it for your package. There is no support to redefine an undefined preprocessor definition.

And include directories, which are relative to the package's root directory.
```
{
  include_directories: [
    "public",
    "third_party/some_library/include",
  ],
}
```

If a package only wants to build the files inside of specific sub-directory, they can specify the directories containing sources:
```
{
  sources: [
    "sources",
  ],
}
```

(In this example, `sources` is overriden with `:` and not appended to with `+:` to exclude the default definition of building everything in the package's root directory.)


## Package Management
REBS has a configuration file that lives in `~/.rebs.jsonnet`. If it doesn't exist when REBS runs, it is created. You can override it to a custom path by setting the `REBS_CONFIG` environment variable to a custom path.

By default (which can be changed by modifying `.rebs.jsonnet`), REBS is configured to search for packages in:

* ~/sources/applications
* ~/sources/libraries
* ~/sources/third_party

If you run `rebs` without specifying a package name, it assumes the package is the current working directory. If you pass an argument that doesn't begin with `--` to `rebs`, it is assumed to be the package that you want to build. The package may either be a path, or a name. If it is a name, then the above directories are scanned to look for a sub-directory that matches that name. For example, if you want to build the SDL library, assume it lives in ~/sources/third_party/SDL, you can run `rebs SDL`. If multiple packge names (divided by spaces) are specified, they are all built. If a package name has spaces, e.g. "Facebook Yoga", then the space needs either be escaped, or the package name quoted. For example:

* `rebs Facebook\ Yoga`
* `rebs "Facebook Yoga"`

Passing `--all` ignores the provided packages and attempts to operate on all known packages that REBS can find.

Package names need to be unique - they are first found, first served. To minimize naming conflicts, it's recommmend that you name your libraries by including their author/publisher. For example, "Google Skia" instead of just "Skia".

### Libraries
At the very least, a library package needs a `.package.rebs.jsonet` file in its root directory containing the following:
```
{
  package_type: "library",
}
```

This tells REBS that the output is not a standalone executable, but a library that can be a dependency of other libraries or applications.

It might also be useful for libraries to expose pre-processor definitions to any packages that depend on it:

```
{
  public_defines: [
    "PREPROCESS_DEFINITION",
    "SOME_OTHER_PREPROCESSOR_DEFINITION=1",
  ]
}
```

Libraries usually want to expose include directories to any packages that depend on it, which are relative to the library's root directory. For example, if you have a sub-directory named "public" containing the public headers that your library exposes:

```
{
  include_directories: [
    "public",
  ],
}
```

Sometimes a library doesn't produce executable code to be linked against, for example if a library consists of only assets or only headers. For these libraries you can define `no_output_file`:

```
{
  no_output_file: 1
}
```
## Advanced topics

### How the Jsonnet configurations work
The package's `.package.rebs.jsonet` gets appended to `~/.rebs.jsonnet`. When a package gets built, the configuration files get appended to build the package's metadata that describes how to build a particular package.

### Supporting new languages
The default `~/.rebs.jsonnet` includes the rules for building files based on the file extensions of source files. These rules can be extended for every project in `~/.rebs.jsonnet`, or they can be appended to or replaced for a specific package in the package's `.package.rebs.jsonet`.

For example, this is how the support for assembling .s and .S files is defined:
```
build_commands+: {
    // AT&T ASM:
    local att_asm = 'nasm -o ${out} ${in}',
    "s": att_asm,
    "S": att_asm
  },
```

The supported placeholders are:

* `${in}` - The input file.
* `${out}` - The output object file.
* `${cdefines}` - The preprocessor defines, in standard C compiler style.
* `${cincludes}` - The include directories, in standard C compiler style.
* `${deps file}` - The file to write the dependencies to, in standard C compiler style.
* `${package name}` - The name of the currently building package.
* `${temp directory}` - The path of the temp directory during this build. This is not unique to the package.

The linker command can also be overriden:

```
linker: "clang -o ${out} ${in}
```

The supported placeholders are:

* `${in}` - A list of input files for the linker.
* `${out}` - The output file for the linker.

### Jsonnet external variables
There are variables about the current build environment that can be accessed in the Jsonet configuration files via `std.extVar`. They are:

* `optimization_level` - Either `optimized`, `debug`, or `fast`.
* `target_architecture` - The target architecture. e.g. `x86`
* `target_os` - The target OS.

### Local universes
Sometimes you might want a "universe" of packages to be isolated from the rest of the build system. For example, if you want are building software for an embedded system with its own set of libraries. If the directory you're running `rebs` from contains its own `.universe.rebs.jsonet` then it gets appended to the `~/.rebs.jsonnet` before appending a package's `.package.rebs.jsonet`.

The intention is that the universe may want to override the build commands and package directories in a way that's completely different to building general packages that will run on the host OS.

You can also set the default architecture and operating system with, so that you do not have to provide `--os=` and `--arch=` each time you run `rebs`:

```
{
  default_os: "bare-metal",
  default_arch: "arm64",
}
```

(Note that the `.universe.rebs.jsonet` will be parsed using the host's architecture and OS, but then the `default_os` and `default_arch` will be used parsing `.package.rebs.jsonet`.)

### Global run commands
In your `.universe.rebs.jsonnet` or `~/.rebs.jsonnet` you can specify a `global_run_command`, which is a command that is ran after all packages are built. When a `global_run_command` is specified, this command is ran once, rather than attempting to execute each built application. The intent of this command is to provide a custom command to start an emulator or other environment.

### Excluding source files to build.
You can files to exclude, relative to the package's root directory, using regular expressions:

```
{
  exclude_files: [
    `sources/.*arm.S`
  ]
}
```

In the above example, `/sources/math/cos-arm.S` will not get built, but `/sources/math/cos-x86.S` will get built. The intention is that you might want to section off code that is specific to certain architectures or operating systems.

### Destination directory
You can specify a destination directory to copy the built binary and any assets to. For example:

```
{
  destination_directory: "${temp directory}/fs/Applications/${package name}",
}
```

### Assets
Assets are files that are copied from the package to the destination directory. Files are copied if the source is newer than the destination. Deleted files in the source do not get removed on subsequent runs.

```
{
  asset_directories: [
    "Assets"
  ],
}
```

### Ignoring files
You can choose to ignore to build certain files. The paths are relative to the package's root directory.

```
{
  files_to_ignore: [
    "Source/abc.cc",
    "Source/def.cc",
  ],
}
```

### Other usage
Run `rebs --help` for complete usage.

## Building

You can build Really Easy Build System using the Really Easy Build System, but the. But, to avoid the "chicken and the egg" problem, you can also build using `make` in the root directory of this repository.

You should also have the following tools installed:

* [make](https://www.gnu.org/software/make/) - for building REBS from source without REBS.
* Some kind of C++2x compatible compiler (I recommend [clang](https://clang.llvm.org/).)
* [Jsonett](https://jsonnet.org/)

### POSIX
On a POSIX system, a simple way to build and install REBS is to call:

```
make && sudo cp rebs /usr/local/bin/rebs
```

## Contributing
See [docs/contributing.md](docs/contributing.md) for information on contributing.

This is not an officially supported Google project.

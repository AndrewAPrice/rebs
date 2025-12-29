# Really Easy Build System

REBS (Really Easy Build System) is a build system intended for C and C++ packages. It is intended to build packages from a directory of C/C++ source code with little-to-no configuration required.

## Usage

### Quick Start

To build and run packages that use the C++ standard library, you can just `cd` into a directory containing recognized source files (.c, .cpp, .cc, .asm, etc) and run `rebs`. The C++ sources in this directory and all subdirectories will build, and if successful, the resulting program will run.

### Command-line Arguments

#### Invocation Actions

These flags determine the main action REBS performs.

*   `--build` - Builds the identified packages but does not run them.
*   `--clean` - removes temporary files for the identified packages.
*   `--deep-clean` - Removes all temporary files, including cached repositories.
*   `--run` - Builds and runs the packages. This is the default action if none is specified.
*   `--test` - Builds and runs unit tests for the packages.
*   `--list` - Lists all known packages with their names and paths.
*   `--generate-clangd` - Generates `.clangd` configuration files for the packages (useful for IDEs).
*   `--update` - Updates third-party dependencies. Can be combined with other actions.
*   `--complete` - Used for shell auto-completion.

#### Optimization Levels

These flags control the compiler optimization settings.

*   `--debug` - Builds with debug symbols and no optimizations.
*   `--fast` - (Default) Builds with some optimizations enabled (-O1 or similar).
*   `--optimized` - Builds with full optimizations enabled (e.g. -O3, LTO).

#### Target Environment

You can override the environment being targeted:

*   `--os=<os>` - The OS being targeted.
*   `--arch=<arch>` - The architecture being targeted.

#### Options

*   `--all` - Ignores the input arguments and applies the action to *all* known packages found on the system.
*   `--verbose` - Prints detailed information about the commands being executed.
*   `--help` - Prints the usage help message.

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
* `${clangincludes}` - The output of `clang -print-resource-dir`.
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

## Third Party Support

REBS supports managing third-party dependencies through a `third_party.json` file in your package's root directory. This allows you to fetch external code (via git, zip, or direct download) and perform various operations like copying files, executing commands, and manipulating version strings.

### The `third_party.json` file

The file should contain a JSON object with two optional main sections: `repositories` and `operations`.

#### Repositories

The `repositories` array defines external sources to fetch. Each repository must specify a `type`, `url`, and `placeholder`. The `placeholder` is used to reference the downloaded location in subsequent operations.

Supported types:
*   `git` - Clones a git repository.
*   `zip` - Downloads and unzips a file.
*   `download` - Downloads a single file.

Example:
```json
"repositories": [
  {
    "type": "git",
    "url": "https://github.com/nlohmann/json.git",
    "placeholder": "nlohmann_json_root"
  }
]
```

#### Operations

The `operations` array defines a sequence of actions to perform. Each operation has an `operation` field specifying the type.

Supported operations:

*   **copy**: Copies files or directories.
    *   `source`: Path(s) to copy from (can use placeholders).
    *   `destination`: Path(s) to copy to (relative to package root).
    *   `recursive`: (Optional) Boolean to copy directories recursively.
    *   `exclude`: (Optional) List of regex patterns to exclude.
    *   `replace`: (Optional) List of objects to perform string replacements in files.
        *   `file`: File(s) to apply replacements to.
        *   `replacements`: List of `[ "search", "replace" ]` pairs.
        *   `prepend`: String to prepend to the file.
*   **createDirectory**: Creates directories.
    *   `path`: Path(s) to create.
*   **evaluate**: Evaluates Python expressions to generate variables.
    *   `values`: Map of variable names to expressions.
*   **execute**: Executes a shell command.
    *   `command`: The command string to execute.
    *   `directory`: (Optional) Working directory for the command.
    *   `inputs`: (Optional) List of input files (for cache invalidation).
    *   `outputs`: (Optional) List of output files (for cache invalidation).
    *   `alwaysRun`: (Optional) Boolean to force execution every time.
*   **joinArray**: Joins an array of strings into a single string.
    *   `value`: Array of strings (or single string) to join.
    *   `joint`: Separator string.
    *   `placeholder`: Variable name to store the result.
*   **readFilesInDirectory**: Lists files in a directory.
    *   `path`: Directory path(s) to read.
    *   `extensions`: (Optional) List of file extensions to include.
    *   `fullPath`: (Optional) Boolean to return full paths instead of filenames.
    *   `placeholder`: Variable name to store the list.
*   **readRegExFromFile**: Extracts text from a file using regex.
    *   `file`: Path to the file.
    *   `values`: Map where keys are comma-separated variable names and values are regex patterns.
*   **set**: Sets variables directly.
    *   `values`: Map of variable names to values (strings or arrays).

#### Example `third_party.json`

```json
{
  "repositories": [
    {
      "type": "git",
      "url": "https://github.com/fmtlib/fmt.git",
      "placeholder": "fmt_root"
    }
  ],
  "operations": [
    {
      "operation": "copy",
      "source": [ "${fmt_root}/include/fmt" ],
      "destination": [ "third_party/fmt/include/fmt" ],
      "recursive": true
    },
    {
      "operation": "copy",
      "source": [ "${fmt_root}/src/format.cc" ],
      "destination": [ "third_party/fmt/src/format.cc" ]
    }
  ]
}
```

### Strings and Placeholders

All string values in `third_party.json` can contain placeholders using the syntax `${variable}`.

#### Expansion Rules

*   **Variables can hold multiple values**: A variable in REBS can be a list of strings (an array).
*   **Cartesian Product**: If a string contains placeholders that resolve to arrays, the string is expanded into a new array containing every permutation of the placeholders.
    *   Example: `prefix-${var}-suffix` where `var` is `["a", "b"]` expands to `["prefix-a-suffix", "prefix-b-suffix"]`.

### Shell Auto-Completion

To enable shell auto-completion, add the following line to your `~/.bashrc` (or `~/.bash_profile` on macOS):

```bash
complete -C "rebs --complete" rebs
```

If you use Zsh on macOS, add the following to your `~/.zshrc`:

```bash
# Enable bash completion support in Zsh
autoload -Uz compinit && compinit
autoload -Uz bashcompinit && bashcompinit

# Register rebs completion
complete -C "rebs --complete" rebs
```

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

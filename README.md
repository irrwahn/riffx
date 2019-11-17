
# RiffX

RiffX is a simple utility that can be used to extract RIFF data streams
embedded in other files.

It is useful e.g. for extracting audio clips from certain game files,
e.g. Borderlands 2/TPS `*.pck` audio packages and others.

### RIFFX IS NOT A RIFF PARSER!

It has no deeper knowledge about a RIFF file's inner structure and just
indiscriminately dumps anything that even remotely looks like a RIFF chunk
into a separate output file.  You break it, you get to keep the pieces!


## Usage

The general invocation looks like this:

`riffx [-b] [-l] [-g] [-v] infile_0 [... infile_N] [out_dir]`

Options shall be placed before any non-option arguments.  Input files are
processed in the order they are specified.  If the last argument is not a
readable input file, it is taken to be the name of the desired output
directory.  If no output directory name is specified, it defaults to
'output' in the current working directory.  The output directory is
created, if it does not already exist.  CAVEAT: `riffx` will overwrite
any existing file having the same name as a dump file, without asking for
confirmation!

By default `riffx` creates a directory structure in the `output` directory
that reflects the path(s) used to specify the input file(s).  The `-b`
option causes `riffx` to alternatively create a flat output directory,
resulting in all generated files being placed directly in `output`, with
a file number and the base name of the input file included in the dump
file name to help disambiguate the files.

The `-l` option activates a primitive heuristic that tries to extract a
text label from each RIFF chunk and include it in the dump file name.
In the absence of a suitable label chunk it falls back to the default
behavior of using solely the stream index count to create the output
file name.  This method is unreliable and may or may not give meaningful
results depending on your input files.

With the `-g` option `riffx` can be instructed to ignore the respective
size fields in the embedded RIFF chunks and instead assume that each
chunk ends where the next one begins, or at the end of the input file.
This is imprecise, but may help in certain cases where the RIFF size
fields may contain bogus values.

To make `riffx` be a bit more verbose about its operation you can pass
it the `-v` flag.

**NOTE:** The extracted raw RIFF streams will most likely require some
form of post-processing to be useful.  To turn e.g. the Audiokinetic
Wwise RIFF/RIFX sound format into something any run-of-the-mill audio
player can digest you should:

1. Run each individual RIFF file through the `ww2ogg` converter, cf.
   [https://github.com/hcs64/ww2ogg](https://github.com/hcs64/ww2ogg)

2. Fix up the resulting Ogg Vorbis files with `revorb`, cf.
   [https://github.com/jonboydell/revorb-nix](https://github.com/jonboydell/revorb-nix)

Both mentioned third party projects are also referenced as Git submodules
and can conveniently be cloned and built right in the `riffx` project tree.
Please consult the documentation of each respective project for more
information on how and under what conditions you may use it.

### Example

The following is a simple example to demonstrate the extraction of and
conversion to the Ogg Vorbis format of sound clips embedded in a file
named `audio_banks.pck`:

```
  $ riffx -blv audio_banks.pck outdir
  $ for f in outdir/*.riff ; do ww2ogg $f ; done
  $ for f in outdir/*.ogg ; do revorb $f ; done
```

NOTE:
It may turn out necessary to tweak the `ww2ogg` command line parameters
in order to get satisfactory results, please refer to the `ww2ogg`
documentation for details.


## Build

Simply run `make` without any parameters to build all included tools and
submodules, or `make riffx` to build only the `riffx` utility.

Alternatively just translate `riffx.c` using your C compiler of choice
and hope for the best.


## Porting

RiffX was written on Linux and makes use of some OS specific features,
most prominently the POSIX `mmap()` function to make processing large
files easy and efficient.  Any effort to port `riffx` to another platform
may entail:

 * Replace open/mmap/creat/write with suitable ISO C functions (malloc,
   fopen, fread, fwrite), while taking extra care of chunk splitting on
   buffer boundaries.

 * Port the mkdirp() function.


## Helper

Another small utility named `unriffle` is included in this repository.

Again, this is not a real RIFF parser either, but it can dump the chunks
of a RIFF/RIFX file to standard output and thereby help to get a rough
idea on what kind of data it may contain.  A tiny subset of chunk types
known to be present in some RIFF audio formats is given special treatment
to make their content more accessible to mere humans.  This might help
identify the format of the data stored in the file.

`Unriffle` is build automatically when calling `make` in the project
directory.  In contrast to `riffx` it is written entirely in portable
ISO C99.


## Alternatives

As `riffx` was written as a quick-and-dirty tool for a specific use case
chances are high it might not work in your specific scenario.  In this
case you may want to check out other, more generalized, data extraction
tools, like e.g. the QuickBMS tool developed by Luigi Auriemma.


## License

RiffX is distributed under the 0BSD ("Zero-clause BSD") license.
See `LICENSE` file for more information.

Each referenced third party submodule comes with its own license terms,
please consult the respective project's documentation.

----------------------------------------------------------------------

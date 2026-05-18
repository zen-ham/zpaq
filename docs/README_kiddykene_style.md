# zpaq
sub'd to deepseek for too long, started thinking about compression. so heres a python package that does zpaq compression in memory, which is something nobody else has done. all the other zpaq packages on pypi are just running the zpaq exe as a subprocess which is honestly aids. you put bytes in, get bytes out. very simple.

```py
import zpaq
b = zpaq.compress(b"hello world " * 1000, level=3)
assert zpaq.decompress(b) == b"hello world " * 1000
```

ALSO somehow it ended up faster than the actual zpaq.exe lmaoooo. like 6x faster on 10mb files, in both directions. didnt mean to do this but i guess multi threading the decompress and turning on jit and adding libsais and avx2 will do that. like idk how mahoney made zpaq.exe not multithreaded by default but ok.

the original library (libzpaq) was made by matt mahoney, hes some legend that just released it into the public domain and called it a day, so i basically just wrapped it with pybind11 and put my own threading stuff on top. no affiliation just want to be clear.

## how to use it

import it and call compress/decompress, what are you a mug?

```py
zpaq.compress(data, level=5, threads=0, dedup=False)
zpaq.decompress(data, threads=0)
```

level is 0 to 5. 0 = no compression which is a bit useless. 5 = maximum compression which is what i recomend. threads=0 means use every core you have, threads=1 means use one core, threads=N means use N cores like duh. dedup=True splits the input into 64kb chunks based on a rolling hash, hashes each chunk with sha1, and only stores unique ones. great if you have like 5 copies of the same file glued together. matches what the zpaq exe does internally.

verify=True does sha1 checksums on each block but its off by default cause its slower and most people dont care. method= lets you pass a custom libzpaq method string if you know what youre doing (you dont).

## why this is faster than the actual zpaq exe (i still cant get over it)

so theres this thing called JIT that the zpaq exe uses on x86_64 where instead of running the predictor as an interpreter loop it just emits actual x86 instructions at runtime, then jumps to them. obviously waaaay faster. the libzpaq library has this built in but you have to compile with -DJIT or whatever. my x86_64 wheels have it on. arm wheels dont because the jit only knows how to emit x86 and would crash on arm, kinda obvious.

multi threading the compress was easy enough, just split the input into blocks and run compressBlock on each thread. The decompress was more painful because libzpaq's decompress api is sequential, so what i did was scan the bytes for the 13 byte locator tag thingy that marks the start of every zpaq block, then dispatch each block to its own worker, then concatenate the outputs. works super fkn well.

theres also libsais which does the suffix array faster than the original libdivsufsort thats in libzpaq (only used at level 3 BWT mode, doesnt help level 5 default), and i added /arch:AVX2 so the compiler auto-vectorises where it can, and skipping the sha1 checksum by default cause its just a waste of cycles if you arent paranoid.

## numbers

| size | zpaq.exe comp | mem comp | zpaq.exe decomp | mem decomp |
| --- | --- | --- | --- | --- |
| 40kb | 0.14s | 0.13s | 0.14s | 0.12s |
| 1mb | 2.28s | 0.58s | 2.23s | 0.58s |
| 10mb | 24.1s | 3.83s | 25.1s | 3.83s |
| 100mb | 252.5s | 74.1s | 250.7s | 73.2s |

(12 core ryzen, level 5, mem using threads=0 which uses all cores)

## installing

```
pip install zpaq
```

if youre on windows linux or mac i have prebuilt wheels for you so it just works. if you have some weird cpu without avx2 it might not work and you'll have to compile from source but who has a cpu without avx2 in 2026 lmao. python 3.9+. on windows i statically linked the runtime so you dont need the visual c++ redistributable garbage.

## stuff im gonna do later (maybe)

- PGO (profile guided optimization). adds another like 10% but the github actions stuff makes it annoying to set up so i didnt bother.
- writing the JIT to emit AVX2 mul-adds for the predictor. would actually be properly fast then. but its like 1000 lines of x86 codegen so im not doing that today
- multi threading the dedup compress (currently single threaded for some reason)
- letting you grab individual files out of a multi file zpaq archive instead of getting the whole concatenated thing

## license

public domain, same as libzpaq. libsais is apache 2.0. im not paying for a lawyer to fight about it idc what you do with it.

if you actually use this for something cool message me i think thats neat

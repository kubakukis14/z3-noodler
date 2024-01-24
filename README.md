# Z3-Noodler

Z3-Noodler is an SMT solver for string constraints such as those that occur in symbolic execution and analysis of programs, 
reasoning about configuration files of cloud services and smart contracts, etc.
Z3-Noodler is based on the SMT solver [Z3 v4.12.2](https://github.com/Z3Prover/z3/releases/tag/z3-4.12.2), in which it replaces the solver for the theory of strings. 
The core of the string solver implements several decision procedures, but mainly it relies on the equation stabilization algorithm (see [Publications](#publications)).

Z3-Noodler utilizes the automata library [Mata](https://github.com/VeriFIT/mata/) for efficient representation of automata and their processing.

For a brief overview of the architecture, see [SMT-COMP'23 Z3-Noodler description](doc/noodler/z3-noodler_systems-description.pdf).

[//]: # (TODO: Write the following paragraphs. ## Differences from Z3)

## Building and running

### Dependencies

1) The [Mata](https://github.com/VeriFIT/mata/) library for efficient handling of finite automata. Minimum required version of `mata` is `v1.1.0`.
    ```shell
    git clone 'https://github.com/VeriFIT/mata.git'
    cd mata
    make release
    make install
    ```

### Building Z3-Noodler

```shell
git clone 'https://github.com/VeriFIT/z3-noodler.git'
mkdir z3-noodler/build
cd z3-noodler/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```
See [instructions for building Z3][cmake] for more details.

[visual_studio]: README-Z3.md#building-z3-on-windows-using-visual-studio-command-prompt
[make]: README-Z3.md#building-z3-using-make-and-gccclang
[cmake]: README-Z3.md#building-z3-using-cmake

To build tests for Z3-Noodler (assuming you have [Catch2](https://github.com/catchorg/Catch2) version 3 installed), run the following 
command.
```shell
make test-noodler
```

### Running Z3-Noodler
To run Z3-Noodler, select Z3-Noodler as Z3's string solver when executing Z3.
```shell
cd build/
./z3 smt.string_solver="noodler" <instance_file.smt2> 
```

To run tests for Z3-Noodler, execute
```shell
cd build/
./test-noodler
```

## Limitations
The following functions/predicates of the [SMTLIB Strings theory](https://smtlib.cs.uiowa.edu/theories-UnicodeStrings.shtml) are not supported at the moment.
```
str.replace_all
str.replace_re_all
str.to_int
str.from_int
```

We provide a full support for `str.contains`, but a limited support for its negated version.
We also do not support string variables as arguments of `str.to_re` and `re.range`.

## Publications
- Y. Chen, D. Chocholatý, V. Havlena, L. Holík, O. Lengál, and J. Síč. [Solving String Constraints with Lengths by Stabilization](https://doi.org/10.1145/3622872). In *Proc. of OOPSLA'23*, Cascais, Portugal, Volume 7, Issue OOPSLA2, pages  2112–2141, 2023. ACM.
- F. Blahoudek, Y. Chen, D. Chocholatý, V. Havlena, L. Holík, O. Lengál, and J. Síč. [Word Equations in Synergy with Regular Constraints](https://doi.org/10.1007/978-3-031-27481-7_23).  In *Proc. of FM’23*, Lübeck, Germany, volume 14000 of LNCS, pages 403–423, 2023. Springer.


## Z3-Noodler source files

The string solver of Z3-Noodler is implemented in [src/smt/theory_str_noodler](src/smt/theory_str_noodler).

Tests for Z3-Noodler are located in [src/test/noodler](src/test/noodler).

## Authors
- :envelope: [Vojtěch Havlena](mailto:ihavlena@fit.vut.cz?subject=[GitHub]%20Z3-Noodler),
- :envelope: [Juraj Síč](mailto:sicjuraj@fit.vut.cz?subject=[GitHub]%20Z3-Noodler),
- [Yu-Fang Chen](mailto:yfc@iis.sinica.edu.tw?subject=[GitHub]%20Z3-Noodler),
- [David Chocholatý](mailto:xchoch08@stud.fit.vutbr.cz?subject=[GitHub]%20Z3-Noodler),
- [Lukáš Holík](mailto:holik@fit.vut.cz?subject=[GitHub]%20Z3-Noodler),
- [Ondřej Lengál](mailto:lengal@fit.vut.cz?subject=[GitHub]%20Z3-Noodler).


## Original Z3 README

For the original Z3 README, see [README-Z3.md](README-Z3.md).

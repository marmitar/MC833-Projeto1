# MC833 - Projeto 1

## Build

```sh
> CC=clang meson setup build
> meson compile -C build
```

## Linter

```sh
> ninja -C build clang-tidy
```

## Format

```sh
> ninja -C build clang-format
```

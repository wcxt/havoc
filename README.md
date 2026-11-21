# Havoc

A simple HTTP server written in C that serves static files from a directory.

## Build

```bash
make
```

## Use

Serve the test fixture directory on the default port, `8080`:

```bash
./havoc -d tests/fixture
```

Open `http://localhost:8080` in your browser.

Use a custom port:

```bash
./havoc -d tests/fixture -p 3000
```

## Test and Debug

Build the sanitizer-enabled debug binary and run the smoke tests:

```bash
make check
```

Build only the debug binary:

```bash
make debug
```

Run it manually:

```bash
./havoc-debug -d tests/fixture
```

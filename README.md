# databento-to-lobster

A high-speed C++ converter that turns Databento's NASDAQ TotalView-ITCH dataset (`XNAS.ITCH`, market-by-order) into standard LOBSTER-format CSV files.

## Purpose

The goal is simplicity: to let Databento users who want to research limit order book data work with LOBSTER-format tooling with less effort.

## What it does

- Reads Databento MBO files (`.dbn.zst`) and writes LOBSTER-format CSV.
- Fully automated, one run per input file.
- Re-expresses events in the LOBSTER layout only. It does not modify, resample, or synthesize any underlying market event.

More details on how the conversion happens are on the [wiki](https://github.com/<user>/<repo>/wiki).

## Build

Requirements: MSVC (developed with Visual Studio 2026), C++23, CMake.

```
git clone --recurse-submodules <repo-url>
cd databento_to_lobster
external\vcpkg\bootstrap-vcpkg.bat
cmake -B build -DCMAKE_TOOLCHAIN_FILE=external/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Dependencies (`fmt`, `openssl`, `zstd` via vcpkg; `databento-cpp` as a submodule) are fetched automatically.

## Usage

```
databento_to_lobster <input.dbn.zst> <output_dir>
```

## Disclaimer

This is an independent project developed during my thesis. I claim no ownership of the data, the source schemas, or the LOBSTER format. The conversion was obtained by reverse engineering the public documentation listed below. Databento data remains subject to Databento's own license and terms.

## Sources

- Nasdaq, *TotalView-ITCH Specification*: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf
- Huang & Polak, *LOBSTER: Limit Order Book Reconstruction System* (SSRN): https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1977207
- Databento, *Limit order book example*: https://databento.com/docs/examples/order-book/limit-order-book?historical=cpp&live=cpp&reference=python
- Databento, *XNAS.ITCH dataset, MBO normalization*: https://databento.com/docs/venues-and-datasets/xnas-itch#mbo-normalization?historical=cpp&live=cpp&reference=python

## License

See `LICENSE`.

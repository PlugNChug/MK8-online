# Asset tools

Pipeline (run from the SDK root):

```sh
python3 linux/tools/extract_all.py
```

| Script | Purpose |
|---|---|
| `extract_shaders.py` | Scan the PE for `FXZF` zlib streams, decompress to SPIR-V, write `index.json` |
| `extract_models.py` | Find the four model `getWeights(preset)` dispatchers, extract/dedupe 128 KiB banks |
| `analyze_shaders.py` | Parse SPIR-V entry points, bindings, capabilities into `catalog_raw.json` |
| `build_catalog.py` | Classify pass/model/tier/color/flags into `catalog_final.json` |
| `decode_selectors.py` | Decode the 324 selector-table pairs into `selector_tables.json` |
| `verify_assets.py` | Check counts, sizes, model hashes, catalog/selector coverage |
| `extract_all.py` | Run the full pipeline and write the top-level `manifest.json` |

`spv.py` is the small SPIR-V parser used by `analyze_shaders.py` and
`build_catalog.py`.

No DLL offsets are hardcoded. The signed DLL is read-only input.

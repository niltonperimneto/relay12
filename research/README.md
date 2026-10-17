# Opt-in research

The upload-limit experiment from `measure/no-upload-limit` is available without
changing the shipped patch series or skipping its regression gate:

```sh
python3 research/zero_upload_limit.py /path/to/prepared-production-source /path/to/new-research-source
```

Prepare the production source with `scripts/prepare-d3d11on12-source.sh` first.
The command validates that tree, copies it to a separate output, and changes only
the upload-submit limit to zero. It records the original and modified file
hashes in `RESEARCH-ONLY.json`. Existing outputs are refused.

Build the copy into a separate output directory and use a disposable test
prefix for controlled comparisons. The regular production gate deliberately
rejects this variant. It must never replace the runtime release payload or be
installed into a live bottle. Keep the regular tests enabled for both sides of
the comparison and report the variant's expected gate rejection separately.

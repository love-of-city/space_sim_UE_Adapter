"""Generate UE import settings and a runtime MJCF asset catalog."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from bsk_render_adapter.mjcf_assets import parse_mjcf_geometry_metadata


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mjcf", type=Path, required=True)
    parser.add_argument("--destination", required=True, help="UE content path, for example /Game/BSK/Generated/UR5e")
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--import-settings", type=Path, required=True)
    parser.add_argument("--build-scale", type=float, default=100.0)
    parser.add_argument("--component-scale", type=float, default=1.0)
    args = parser.parse_args()

    metadata = parse_mjcf_geometry_metadata(args.mjcf)
    sources = sorted(
        {Path(item.source_path).resolve() for item in metadata if item.source_path},
        key=lambda item: item.as_posix().casefold(),
    )
    if not sources:
        raise RuntimeError(f"no mesh assets were found in {args.mjcf}")
    missing = [source for source in sources if not source.is_file()]
    if missing:
        raise FileNotFoundError(f"mesh source does not exist: {missing[0]}")
    stems = [source.stem.casefold() for source in sources]
    if len(stems) != len(set(stems)):
        raise ValueError("mesh file stems must be unique within one UE destination")

    destination = args.destination.rstrip("/")
    texture_sources = sorted(
        {Path(item.material_texture_source).resolve() for item in metadata if item.material_texture_source},
        key=lambda item: item.as_posix().casefold(),
    )
    missing_textures = [source for source in texture_sources if not source.is_file()]
    if missing_textures:
        raise FileNotFoundError(f"texture source does not exist: {missing_textures[0]}")
    all_stems = stems + [source.stem.casefold() for source in texture_sources]
    if len(all_stems) != len(set(all_stems)):
        raise ValueError("mesh and texture file stems must be unique within one UE destination")
    catalog = {
        "schema": "bsk-render-asset-catalog/1",
        "source_mjcf": str(args.mjcf.resolve()),
        "assets": {
            str(source): {
                "asset_type": "static_mesh",
                "asset_path": f"{destination}/{source.stem}.{source.stem}",
                # OBJ has no unit metadata. Apply metres-to-centimetres while
                # UE builds LOD0 so small source geometry is not simplified or
                # classified as degenerate before a runtime scale is applied.
                "build_scale": [args.build_scale] * 3,
                "component_scale": [args.component_scale] * 3,
            }
            for source in sources
        },
        "textures": {
            str(source): f"{destination}/{source.stem}.{source.stem}"
            for source in texture_sources
        },
    }
    import_settings = {
        "ImportGroups": [
            {
                "GroupName": "BSK MJCF meshes",
                "Filenames": [str(source) for source in (*sources, *texture_sources)],
                "DestinationPath": destination,
                "bReplaceExisting": True,
                "bSkipReadOnly": False,
            }
        ]
    }
    args.catalog.parent.mkdir(parents=True, exist_ok=True)
    args.import_settings.parent.mkdir(parents=True, exist_ok=True)
    args.catalog.write_text(json.dumps(catalog, indent=2), encoding="utf-8")
    args.import_settings.write_text(json.dumps(import_settings, indent=2), encoding="utf-8")
    print(f"Prepared catalog for {len(sources)} mesh and {len(texture_sources)} texture assets")


if __name__ == "__main__":
    main()

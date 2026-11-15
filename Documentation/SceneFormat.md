# Together Engine Scene Format v1

Scene files use UTF-8 JSON and the `.tws.json` extension. Unknown fields may be ignored by version 1 readers, while required fields and all values are validated. A document is limited to 16 MiB, 10,000 objects, and 256 UTF-8 bytes for scene and object names.

```json
{
  "format": "TogetherEngineScene",
  "version": 1,
  "name": "Untitled Scene",
  "objects": [
    {
      "id": 1,
      "name": "Cube 1",
      "visible": true,
      "transform": {
        "position": [0, 0, 0],
        "rotation": [0, 0, 0],
        "scale": [1, 1, 1]
      },
      "material": {
        "baseColor": [0.55, 0.62, 0.48],
        "roughness": 0.72,
        "metallic": 0,
        "emission": [0, 0, 0],
        "baseColorTextureAssetId": ""
      },
      "modelAssetId": "",
      "materialAssetId": ""
    }
  ]
}
```

IDs are positive JSON integers and unique within the scene. Position and Euler rotation are finite numbers within +/-1,000,000. Scale components must be positive and within the same bound. Base color, roughness, and metallic values are in [0, 1]; emission is in [0, 64]. Asset ID fields are optional for older version 1 files and are preserved as strings even if the referenced asset is missing. Saving validates first and writes to a temporary sibling file before replacing the destination, so an unsuccessful save does not truncate the previous scene.

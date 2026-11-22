# Release 1 release note (in preparation)

Draft for the next release after RC3 (`v2026.11.07-rc3`). Items are added when they are merged into `next`; the
release text is written when the Release 1 gate (`docs/status/release.yaml`) is green.

## Upgrade notes

- **T23: H+V flip now really flips both axes (#795).** Before, with `image.hflip = 1` and `image.vflip = 1` the T23
  picture was only mirrored: the second MSCA update request was lost while the first was still pending. After the
  update both axes are flipped. Cameras that used H+V to get an upright picture must set their flip once again
  (usually `image.hflip = 1`, `image.vflip = 0`, or both 0); check the picture after the update.

# Source layout reservation

The Phase 0 native CLI is scaffolded before the integrated application. The .NET application remains gated until M3.

Planned components:

- `A0CameraStitcher.Phase0` (C++20 CLI, current)
- `A0CameraStitcher.App` (M3)
- `A0CameraStitcher.Core` (M3)
- `A0CameraStitcher.CameraAgent` (single C++ process, M3)
- `A0CameraStitcher.StitchEngine`
- `A0CameraStitcher.Storage`

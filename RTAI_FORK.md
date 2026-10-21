# luanti-RTAI: What This Fork Adds

**Last Updated:** 2026-10-20

This is a fork of [Luanti](README.md) used as the environment for the [Luanti RL Vision Learning](https://github.com/rjogilvie/luanti-rl-visionlearning) active-vision platform. All additions are gated behind the `ENABLE_TRACKING_EXPORT` CMake option:

```bash
cd build
cmake .. -DENABLE_TRACKING_EXPORT=ON -DBUILD_SERVER=OFF
make -j$(nproc)
```

Project-wide status and plan: [System Status](https://github.com/rjogilvie/luanti-rl-visionlearning/blob/main/docs/implementation-status/SYSTEM_STATUS.md) · [Roadmap](https://github.com/rjogilvie/luanti-rl-visionlearning/blob/main/docs/development/ROADMAP.md)

## Features

| Feature | Status | Key code |
|---------|--------|----------|
| **Frame export**: rendered frames sent to the agent | ✅ | `src/client/tracking_export.{h,cpp}` |
| ↳ `tracking_mode = network`: FrameProducer P2P via a NetworkServer control plane (zstd) | ✅ since 2025-11-08 | uses `visionlearningpipeline` `frame_producer` |
| ↳ local mode: POSIX shared memory (`tracking_shm_name`) | ✅ | |
| ↳ UDP broadcast to :8000 | ❌ removed 2025-11-08 (described in [NETWORK_MODE.md](NETWORK_MODE.md)) | |
| **Gaze-command camera control** (SACCADE, PURSUIT, FIXATION, DRIFT, RESET) | ✅ | `applyCameraControl()`, called from `game.cpp` |
| **HUD targets** over UDP :8002: circle/square/cross; static/linear/circular/sinusoidal motion | ✅ | `processTargetCommands()`, `renderTargets()` |
| **Reward events**: survival, health delta, tracking accuracy, target acquisition | ⚠️ computed and logged only; network sending removed 2025-11-08 (`TODO` in `tracking_export.cpp`) | `tracking_export.cpp`, `game.cpp` |
| **Headless operation** | ✅ | `disable_singleplayer_autopause`, `force_rendering` |
| **Dataset mode**: scripted walking/gazing agent, HD RGB, GPU semantic segmentation, metadata | ✅ | `dataset_generator.*`, `render/semantic_step.*`, `semantic_segmenter.*` (CPU fallback), `frame_dumper.*` |

Dataset mode is documented in the parent repo: [DATASET_GENERATION.md](https://github.com/rjogilvie/luanti-rl-visionlearning/blob/main/docs/DATASET_GENERATION.md) and [GAZING_AGENT.md](https://github.com/rjogilvie/luanti-rl-visionlearning/blob/main/docs/GAZING_AGENT.md).

## Settings

| Setting | Purpose |
|---------|---------|
| `enable_tracking_export` | Master switch |
| `tracking_mode` | `network` (FrameProducer) or local shared memory |
| `tracking_server_host`, `tracking_tcp_port`, `tracking_client_id` | NetworkServer control-plane connection (default TCP 7001) |
| `tracking_shm_name` | Shared-memory segment name (must be unique per instance) |
| `disable_singleplayer_autopause`, `force_rendering` | Headless/Xvfb operation |
| `dataset_mode`, `dataset_output_dir`, `dataset_max_frames`, `dataset_frame_skip`, `dataset_walk_speed`, `dataset_semantic_grid_size` | Dataset generation |

## Config files

| File | Use |
|------|-----|
| `luanti_dataset.conf` | Dataset generation (base config for the parent repo's `scripts/`) |
| `luanti_network.conf`, `luanti_network_localhost.conf` | Frame export to a NetworkServer (FrameProducer) |
| `luanti_frameproducer_test.conf` | FrameProducer testing |
| `luanti_local.conf` | Shared-memory mode |

## Branches

- `main-RTAI`: integration branch (everything through the gazing agent)
- `feature/STUNIntegration`: two later headless fixes not yet merged (`d78f270a5`, `5724b1648`)

## Older docs in this directory

- [NETWORK_MODE.md](NETWORK_MODE.md), [SUCCESS_SUMMARY.md](SUCCESS_SUMMARY.md): the Oct 2025 UDP broadcast design (historical)
- [IMPLEMENTATION_NOTES.md](IMPLEMENTATION_NOTES.md), [CSHARP_BINDINGS_GUIDE.md](CSHARP_BINDINGS_GUIDE.md): Oct 2025 notes

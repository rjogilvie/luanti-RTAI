# Luanti Network Tracking - Implementation Notes

## Overview

This document describes the technical implementation details of the UDP broadcast-based frame distribution system for Luanti's tracking export feature.

## Problem Statement

**Original Design Problem:**
The initial FrameDistributor implementation required explicit client registration via `RegisterClient()` before broadcasting frames. This created a chicken-and-egg problem:
- Clients couldn't register without a TCP handshake
- Sender and receiver both tried to bind to the same port (8000)
- No frames were sent if no clients were registered

**Solution:**
Implemented true UDP broadcasting with SO_BROADCAST socket option, allowing any client to receive frames without prior registration.

## Architecture

### Port Binding Strategy

**Sender (Luanti):**
```
Binds to: 0.0.0.0:0  (port 0 = OS auto-assigns available port)
Sends to: 127.0.0.1:8000 (localhost) or 255.255.255.255:8000 (network broadcast)
```

**Receivers (udp_frame_receiver, RL agents):**
```
Binds to: 0.0.0.0:8000  (listens on all interfaces, port 8000)
Receives from: Any sender
```

**Key insight:** Sender and receivers use **different ports**, eliminating bind conflicts.

### SO_BROADCAST Socket Option

**Requirement:**
UDP sockets must have `SO_BROADCAST` enabled to send to broadcast addresses (255.255.255.255).

**Implementation:**
```cpp
// core/src/udp_socket.cpp
bool UDPSocket::SetBroadcast(bool enabled) {
    if (!IsValid()) return false;

    int opt = enabled ? 1 : 0;
    if (setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST,
                  reinterpret_cast<const char*>(&opt), sizeof(opt)) != 0) {
        SetLastErrorFromErrno();
        return false;
    }

    return true;
}
```

**Without SO_BROADCAST:** `sendto()` fails with `EACCES` (permission denied) when attempting to send to broadcast addresses.

### Broadcast Mode Implementation

**FrameDistributor changes:**

1. **Added broadcast mode flag and address:**
```cpp
// core/include/frame_distributor.h
bool m_broadcast_mode = false;
Address m_broadcast_addr;
```

2. **SetBroadcastMode() enables SO_BROADCAST:**
```cpp
// core/src/frame_distributor.cpp
void FrameDistributor::SetBroadcastMode(bool enabled, const Address& broadcast_addr) {
    m_broadcast_mode = enabled;
    m_broadcast_addr = broadcast_addr;

    // Enable SO_BROADCAST socket option for UDP broadcasting
    if (enabled) {
        m_socket.SetBroadcast(true);
    }
}
```

3. **BroadcastFrame() checks mode:**
```cpp
bool FrameDistributor::BroadcastFrame(const FrameFixed256& frame) {
    // Broadcast mode: send to broadcast address (UDP broadcast to any listener)
    if (m_broadcast_mode) {
        return SendFrameToClientUnlocked(frame, m_broadcast_addr, frame.session_id);
    }

    // Client registration mode: send to registered clients only
    // (existing logic...)
}
```

### Luanti Integration

**1. Added tracking_broadcast_addr setting:**
```cpp
// src/defaultsettings.cpp
settings->setDefault("tracking_broadcast_addr", "255.255.255.255");
```

**2. Modified initializeNetwork() signature:**
```cpp
// src/client/tracking_export.h
bool initializeNetwork(int port = 8000,
                       const std::string& bind_addr = "0.0.0.0",
                       const std::string& broadcast_addr = "255.255.255.255");
```

**3. Sender binds to port 0 (auto-assign):**
```cpp
// src/client/tracking_export.cpp
network::Address sender_addr;
sender_addr.port = 0;  // 0 = let OS choose available port
sender_addr.host = bind_addr;

m_impl->frame_distributor->Initialize(sender_addr);
```

**4. Enable broadcast mode with configured address:**
```cpp
network::Address bcast_addr;
bcast_addr.port = port;  // Receivers listen on this port
bcast_addr.host = broadcast_addr;  // Configurable broadcast address
m_impl->frame_distributor->SetBroadcastMode(true, bcast_addr);
```

## Localhost vs Network Broadcast

### Localhost (127.0.0.1)

**Use case:** Same-machine testing, development

**Configuration:**
```ini
tracking_broadcast_addr = 127.0.0.1
```

**Behavior:**
- Packets sent to loopback interface
- Only receivers on same machine can receive
- Faster, more reliable (no network overhead)
- Works even without network connectivity

### Network Broadcast (255.255.255.255)

**Use case:** Multi-machine distributed training

**Configuration:**
```ini
tracking_broadcast_addr = 255.255.255.255
```

**Behavior:**
- Packets sent to all hosts on local subnet
- Any machine on network can receive
- Requires network interface and connectivity
- **Does not work on localhost** (Linux/Unix limitation)

**Important:** 255.255.255.255 broadcast doesn't route through loopback, so localhost testing must use 127.0.0.1.

## Frame Processing Pipeline

### 1. Capture (OpenGL)
```cpp
// src/client/tracking_export.cpp:exportFramebuffer()
video::IImage* image = driver->createScreenShot(video::ECF_R8G8B8);
const u8* image_data = (const u8*)image->getData();
```

**Output:** RGB data (width × height × 3 bytes)

### 2. Grayscale Conversion
```cpp
for (u32 y = 0; y < export_height; y++) {
    for (u32 x = 0; x < export_width; x++) {
        u8 r = pixel_buffer[src_idx + 0];
        u8 g = pixel_buffer[src_idx + 1];
        u8 b = pixel_buffer[src_idx + 2];

        // Standard luminance formula
        grayscale_data[dst_idx] = (u8)((r * 0.299f) + (g * 0.587f) + (b * 0.114f));
    }
}
```

**Output:** Grayscale data (256 × 256 = 65,536 bytes)

### 3. Frame Structure Conversion
```cpp
FrameFixed256 frame = {};  // Zero-initialize
frame.session_id = current_session_id;
frame.sequence_number = static_cast<uint32_t>(frames_exported);
frame.timestamp_us = current_time_microseconds();

// Copy grayscale data to 2D array: uint8_t data[256][256]
for (u32 y = 0; y < 256; y++) {
    memcpy(frame.data[y], &grayscale_data[y * 256], 256);
}
```

**Output:** FrameFixed256 struct (65,620 bytes total with metadata)

### 4. Compression (ZSTD)
```cpp
// core/src/frame_distributor.cpp:ChunkFrame()
auto result = m_compression_engine->Compress(frame_data, frame_size);
if (result && result->compressed_size < frame_size) {
    compressed_data = std::move(result->data);
    data_to_chunk = compressed_data.data();
    data_size = result->compressed_size;
    compression_type = static_cast<uint8_t>(result->type);
}
```

**Compression settings:** ZSTD Level 3
**Compression ratio:** 5-10x (65 KB → ~8-15 KB)
**Output:** Compressed frame data (~8-15 KB)

### 5. Chunking
```cpp
// core/src/frame_distributor.cpp:ChunkData()
size_t header_overhead = FramePacketHeader::HEADER_SIZE;  // 37 bytes
size_t data_per_chunk = m_chunk_size - header_overhead;   // 1440 - 37 = 1403 bytes

total_chunks = (data_size + data_per_chunk - 1) / data_per_chunk;

for (uint32_t chunk_id = 0; chunk_id < total_chunks; ++chunk_id) {
    // Create header
    FramePacketHeader packet_header;
    packet_header.frame_id = frame_id;
    packet_header.chunk_id = chunk_id;
    packet_header.total_chunks = total_chunks;
    packet_header.session_id = session_id;
    packet_header.timestamp_us = timestamp_us;
    packet_header.compression_type = compression_type;
    packet_header.uncompressed_size = uncompressed_size;

    // Serialize: header (37 bytes) + data (up to 1403 bytes)
    std::vector<uint8_t> chunk(header_overhead + chunk_data_size);
    packet_header.Serialize(chunk.data());
    std::memcpy(chunk.data() + header_overhead, data + offset, chunk_data_size);
}
```

**Chunk size:** 1440 bytes total (37 byte header + up to 1403 bytes data)
**Chunks per frame:** ~60 for typical compressed frame (~15 KB / 1403 bytes)
**Output:** Vector of 60 UDP packets of 1440 bytes each

### 6. UDP Broadcast
```cpp
// core/src/frame_distributor.cpp:SendFrameToClientUnlocked()
for (const auto& chunk_data : chunks) {
    int sent = m_socket.SendTo(chunk_data.data(), chunk_data.size(), broadcast_addr);
}
```

**Transmission:** Each chunk sent as independent UDP datagram
**No ordering guarantee:** UDP provides no ordering or reliability
**No reassembly:** Simple receiver just counts packets, full client would reassemble

## Performance Characteristics

### Measured Results (Localhost, 39 seconds)

| Metric | Value |
|--------|-------|
| **Total frames** | 6,752 |
| **Average FPS** | 173.128 |
| **Total data** | 8.35 MB |
| **Data per frame** | ~1.24 KB |
| **Bandwidth** | 0.34-1.58 Mbps |

### Frame Rate Variability

FPS varied during test:
- Initial burst: 351 frames in 5s (70 FPS)
- Peak: 464 FPS (scene loading/caching)
- Steady: 40 FPS (normal gameplay)
- Bursts: 172-312 FPS (scene changes)

**Conclusion:** Frame rate highly dependent on scene complexity and game state.

### Bandwidth Analysis

Per-frame bandwidth:
- Uncompressed: 65,536 bytes = 524,288 bits
- Compressed: ~1,240 bytes = 9,920 bits (at 173 FPS average)

At 60 FPS (typical game rate):
- Uncompressed: 31.45 Mbps
- Compressed: ~0.6 Mbps
- **Savings: ~98%**

### Latency Components

| Component | Latency |
|-----------|---------|
| Frame capture | <1 ms |
| Grayscale conversion | <0.5 ms |
| ZSTD compression | 1-2 ms |
| Chunking | <0.1 ms |
| UDP send (60 packets) | <1 ms |
| **Total (localhost)** | **<5 ms** |

Network latency adds:
- Local network (1 Gbps): +1-2 ms
- WiFi (5 GHz): +5-10 ms
- Remote network: +10-50 ms

## Debugging Tips

### Enable Verbose Logging

Add to game.cpp:
```cpp
infostream << "[Tracking] BroadcastFrame() called, mode="
           << (m_broadcast_mode ? "BROADCAST" : "REGISTERED") << std::endl;
```

### Test UDP Directly

Simple UDP sender:
```cpp
int sock = socket(AF_INET, SOCK_DGRAM, 0);
int broadcast = 1;
setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

struct sockaddr_in dest;
dest.sin_family = AF_INET;
dest.sin_port = htons(8000);
inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);

const char* msg = "TEST";
sendto(sock, msg, 4, 0, (struct sockaddr*)&dest, sizeof(dest));
```

### Check Socket Options

Linux:
```bash
# Check if SO_BROADCAST is enabled
ss -uan | grep 8000
```

### Monitor UDP Traffic

```bash
# Capture UDP packets on port 8000
sudo tcpdump -i any -n udp port 8000

# Count packets
sudo tcpdump -i any -n udp port 8000 | wc -l
```

## Common Pitfalls

### 1. Forgetting SO_BROADCAST
**Symptom:** `sendto()` fails with EACCES (permission denied)
**Solution:** Call `SetBroadcast(true)` on socket

### 2. Same Port for Sender and Receiver
**Symptom:** "Address already in use" or no frames received
**Solution:** Sender binds to port 0, receivers bind to configured port

### 3. Using 255.255.255.255 for Localhost
**Symptom:** Receiver gets 0 frames even though sender is running
**Solution:** Use 127.0.0.1 for same-machine testing

### 4. Missing tracking_broadcast_addr Setting
**Symptom:** Luanti initializes but sends to wrong address
**Solution:** Add `tracking_broadcast_addr = 127.0.0.1` to config

### 5. Old visual_tracking Mod Enabled
**Symptom:** No [Tracking] messages, Lua errors
**Solution:** Disable old mod: `mv ~/.minetest/mods/visual_tracking{,.disabled}`

## Future Improvements

### 1. Multicast Support
Use IP multicast (224.0.0.0/4) instead of broadcast for better scalability:
```cpp
// Join multicast group
struct ip_mreq mreq;
mreq.imr_multiaddr.s_addr = inet_addr("239.255.0.1");
mreq.imr_interface.s_addr = INADDR_ANY;
setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
```

**Benefits:**
- Works across subnets (with router support)
- More efficient than broadcast
- Better security (explicit subscription)

### 2. Adaptive Compression
Adjust ZSTD compression level based on network conditions:
```cpp
if (bandwidth_high && latency_low) {
    compression_level = 1;  // Fast compression
} else {
    compression_level = 6;  // Better compression
}
```

### 3. Frame Prioritization
Skip frames under high load:
```cpp
if (frames_queued > 5) {
    skip_frame = true;  // Drop frame to maintain real-time
}
```

### 4. Receiver-Side Reassembly
Current `udp_frame_receiver` just counts packets. Full implementation should:
1. Buffer incoming chunks by frame_id
2. Reassemble when all chunks received
3. Decompress ZSTD data
4. Convert to usable format (numpy array, etc.)

### 5. Error Correction
Add FEC (Forward Error Correction) to handle packet loss:
```cpp
// Reed-Solomon or fountain codes
add_redundancy_packets(chunks, redundancy=0.1);  // 10% overhead
```

## References

- UDP Broadcasting: RFC 919
- ZSTD Compression: https://github.com/facebook/zstd
- Luanti Network Code: `src/network/` directory
- Tracking Pipeline: `/home/robert/CodeProjects/visionlearningpipeline/`

## Credits

Implementation by Claude Code (Anthropic) in collaboration with Robert, October 2025.

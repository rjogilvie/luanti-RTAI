# C# Bindings for NetworkClient

Guide for creating C# bindings to the C++ NetworkClient for RL training integration.

## Overview

The C++ `NetworkClient` class (from visionlearningpipeline) provides:
- Frame reception from Luanti via UDP broadcast
- Action transmission back to Luanti
- Reward signal transmission
- Connection management and statistics

We need C# bindings to use this from C# RL training code.

## Approach Options

### Option 1: C++/CLI Wrapper (Recommended)

**Pros:**
- Native C++ integration
- Minimal marshalling overhead
- Automatic memory management
- Easy debugging

**Cons:**
- Windows only (or Mono on Linux)
- Mixed-mode assemblies

**Implementation:**

```cpp
// TrackingClientWrapper.h (C++/CLI)
#pragma once

#include "network_client.h"
#include <msclr/marshal_cppstd.h>

using namespace System;
using namespace System::Runtime::InteropServices;

namespace Tracking {
namespace Network {

    // Managed configuration class
    public ref class ClientConfig {
    public:
        String^ ServerHost;
        UInt16 TcpPort;
        UInt16 UdpPort;
        String^ ClientId;
        UInt32 HeartbeatIntervalMs;

        ClientConfig() {
            ServerHost = "127.0.0.1";
            TcpPort = 7001;
            UdpPort = 8000;
            ClientId = "client";
            HeartbeatIntervalMs = 1000;
        }

        // Convert to native config
        tracking::network::ClientConfig ToNative() {
            tracking::network::ClientConfig native;
            native.server_host = msclr::interop::marshal_as<std::string>(ServerHost);
            native.tcp_port = TcpPort;
            native.udp_port = UdpPort;
            native.client_id = msclr::interop::marshal_as<std::string>(ClientId);
            native.heartbeat_interval_ms = HeartbeatIntervalMs;
            return native;
        }
    };

    // Managed frame class
    public ref class Frame256 {
    public:
        UInt32 SessionId;
        UInt32 SequenceNumber;
        UInt64 TimestampUs;
        array<Byte, 2>^ Data;  // 256x256 grayscale

        Frame256() {
            Data = gcnew array<Byte, 2>(256, 256);
        }

        // Convert from native frame
        static Frame256^ FromNative(const tracking::FrameFixed256& native) {
            auto managed = gcnew Frame256();
            managed->SessionId = native.session_id;
            managed->SequenceNumber = native.sequence_number;
            managed->TimestampUs = native.timestamp_us;

            // Copy 2D array
            for (int y = 0; y < 256; y++) {
                for (int x = 0; x < 256; x++) {
                    managed->Data[y, x] = native.data[y][x];
                }
            }

            return managed;
        }
    };

    // Managed client wrapper
    public ref class NetworkClient {
    public:
        NetworkClient() {
            m_native = new tracking::network::NetworkClient();
        }

        NetworkClient(ClientConfig^ config) {
            auto nativeConfig = config->ToNative();
            m_native = new tracking::network::NetworkClient(nativeConfig);
        }

        ~NetworkClient() {
            if (m_native != nullptr) {
                delete m_native;
                m_native = nullptr;
            }
        }

        // Connection management
        bool Connect(String^ serverHost, UInt16 tcpPort, UInt16 udpPort) {
            std::string host = msclr::interop::marshal_as<std::string>(serverHost);
            return m_native->Connect(host, tcpPort, udpPort);
        }

        bool Connect() {
            return m_native->Connect();
        }

        void Disconnect() {
            m_native->Disconnect();
        }

        bool IsConnected() {
            return m_native->IsConnected();
        }

        // Frame reception
        Frame256^ WaitForFrame(int timeoutMs) {
            auto nativeFrame = m_native->WaitForFrame(timeoutMs);
            if (nativeFrame.has_value()) {
                return Frame256::FromNative(nativeFrame.value());
            }
            return nullptr;
        }

        Frame256^ PollFrame(int timeoutMs) {
            auto nativeFrame = m_native->PollFrame(timeoutMs);
            if (nativeFrame.has_value()) {
                return Frame256::FromNative(nativeFrame.value());
            }
            return nullptr;
        }

        // Action transmission (placeholder - needs GazeCommand wrapper)
        // bool SendAction(GazeCommand^ cmd) { ... }

        // Statistics
        UInt64 GetFramesReceived() {
            return m_native->GetStats().frames_received;
        }

    private:
        tracking::network::NetworkClient* m_native;
    };

} // namespace Network
} // namespace Tracking
```

**Usage in C#:**

```csharp
using Tracking.Network;

var config = new ClientConfig {
    ServerHost = "127.0.0.1",
    UdpPort = 8000,
    ClientId = "rl-agent-1"
};

using var client = new NetworkClient(config);
if (client.Connect()) {
    while (training) {
        var frame = client.WaitForFrame(5000);
        if (frame != null) {
            // frame.Data is a 256x256 byte array
            ProcessFrame(frame.Data);
        }
    }
}
```

### Option 2: P/Invoke with C ABI

**Pros:**
- Cross-platform (Windows, Linux, macOS)
- Pure .NET
- No C++/CLI required

**Cons:**
- More manual marshalling
- C ABI wrapper needed
- More error-prone

**Implementation:**

```cpp
// network_client_c.h (C ABI wrapper)
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    uint32_t session_id;
    uint32_t sequence_number;
    uint64_t timestamp_us;
    uint8_t data[256 * 256];  // Flattened 2D array
} Frame256_C;

typedef void* NetworkClientHandle;

// Create/destroy client
EXPORT NetworkClientHandle NetworkClient_Create();
EXPORT void NetworkClient_Destroy(NetworkClientHandle handle);

// Connection
EXPORT int NetworkClient_Connect(NetworkClientHandle handle,
                                  const char* host,
                                  uint16_t tcp_port,
                                  uint16_t udp_port);
EXPORT void NetworkClient_Disconnect(NetworkClientHandle handle);
EXPORT int NetworkClient_IsConnected(NetworkClientHandle handle);

// Frame reception
EXPORT int NetworkClient_WaitForFrame(NetworkClientHandle handle,
                                       Frame256_C* out_frame,
                                       int timeout_ms);
EXPORT int NetworkClient_PollFrame(NetworkClientHandle handle,
                                    Frame256_C* out_frame,
                                    int timeout_ms);

// Statistics
EXPORT uint64_t NetworkClient_GetFramesReceived(NetworkClientHandle handle);

#ifdef __cplusplus
}
#endif
```

```cpp
// network_client_c.cpp
#include "network_client_c.h"
#include "network_client.h"
#include <cstring>

using namespace tracking::network;

EXPORT NetworkClientHandle NetworkClient_Create() {
    return reinterpret_cast<NetworkClientHandle>(new NetworkClient());
}

EXPORT void NetworkClient_Destroy(NetworkClientHandle handle) {
    delete reinterpret_cast<NetworkClient*>(handle);
}

EXPORT int NetworkClient_Connect(NetworkClientHandle handle,
                                   const char* host,
                                   uint16_t tcp_port,
                                   uint16_t udp_port) {
    auto client = reinterpret_cast<NetworkClient*>(handle);
    return client->Connect(std::string(host), tcp_port, udp_port) ? 1 : 0;
}

EXPORT int NetworkClient_WaitForFrame(NetworkClientHandle handle,
                                        Frame256_C* out_frame,
                                        int timeout_ms) {
    auto client = reinterpret_cast<NetworkClient*>(handle);
    auto frame = client->WaitForFrame(timeout_ms);

    if (frame.has_value()) {
        out_frame->session_id = frame->session_id;
        out_frame->sequence_number = frame->sequence_number;
        out_frame->timestamp_us = frame->timestamp_us;

        // Flatten 2D array to 1D
        for (int y = 0; y < 256; y++) {
            memcpy(&out_frame->data[y * 256], frame->data[y], 256);
        }

        return 1;
    }

    return 0;
}
```

**C# P/Invoke wrapper:**

```csharp
using System;
using System.Runtime.InteropServices;

namespace Tracking.Network
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Frame256
    {
        public uint SessionId;
        public uint SequenceNumber;
        public ulong TimestampUs;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256 * 256)]
        public byte[] Data;

        public byte[,] GetData2D()
        {
            var data2d = new byte[256, 256];
            for (int y = 0; y < 256; y++)
                for (int x = 0; x < 256; x++)
                    data2d[y, x] = Data[y * 256 + x];
            return data2d;
        }
    }

    public class NetworkClient : IDisposable
    {
        private IntPtr _handle;

        [DllImport("libtracking_network.so", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr NetworkClient_Create();

        [DllImport("libtracking_network.so", CallingConvention = CallingConvention.Cdecl)]
        private static extern void NetworkClient_Destroy(IntPtr handle);

        [DllImport("libtracking_network.so", CallingConvention = CallingConvention.Cdecl)]
        private static extern int NetworkClient_Connect(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPStr)] string host,
            ushort tcpPort,
            ushort udpPort);

        [DllImport("libtracking_network.so", CallingConvention = CallingConvention.Cdecl)]
        private static extern int NetworkClient_WaitForFrame(
            IntPtr handle,
            out Frame256 frame,
            int timeoutMs);

        public NetworkClient()
        {
            _handle = NetworkClient_Create();
        }

        public bool Connect(string host, ushort tcpPort, ushort udpPort)
        {
            return NetworkClient_Connect(_handle, host, tcpPort, udpPort) != 0;
        }

        public Frame256? WaitForFrame(int timeoutMs = 5000)
        {
            if (NetworkClient_WaitForFrame(_handle, out var frame, timeoutMs) != 0)
                return frame;
            return null;
        }

        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                NetworkClient_Destroy(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }
}
```

**Usage (same as C++/CLI):**

```csharp
using Tracking.Network;

using var client = new NetworkClient();
if (client.Connect("127.0.0.1", 7001, 8000)) {
    while (training) {
        var frame = client.WaitForFrame(5000);
        if (frame.HasValue) {
            var data2d = frame.Value.GetData2D();
            ProcessFrame(data2d);
        }
    }
}
```

## Recommendation

**For Windows-only RL training:** Use **C++/CLI** (Option 1)
- Simpler implementation
- Better performance
- Easier debugging

**For cross-platform support:** Use **P/Invoke** (Option 2)
- Works on Linux (important for GPU servers)
- Pure .NET
- More portable

## Build Configuration

### C++/CLI Project (Visual Studio)

```xml
<!-- TrackingClient.vcxproj -->
<Project>
  <PropertyGroup>
    <CLRSupport>true</CLRSupport>
    <TargetFramework>net6.0</TargetFramework>
  </PropertyGroup>

  <ItemGroup>
    <Reference Include="System" />
    <Reference Include="System.Core" />
  </ItemGroup>

  <ItemGroup>
    <ClCompile Include="TrackingClientWrapper.cpp">
      <CompileAsManaged>true</CompileAsManaged>
    </ClCompile>
  </ItemGroup>

  <ItemGroup>
    <ProjectReference Include="..\visionlearningpipeline\core\tracking_core.vcxproj" />
  </ItemGroup>
</Project>
```

### P/Invoke Shared Library (CMake)

```cmake
# CMakeLists.txt
add_library(tracking_network SHARED
    core/src/network_client.cpp
    bindings/c/network_client_c.cpp
)

target_include_directories(tracking_network
    PUBLIC core/include
           bindings/c
)

# Hide symbols by default, export only C API
set_target_properties(tracking_network PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
)
```

## Testing Strategy

1. **Unit test C++ NetworkClient** - Verify core functionality
2. **Test C ABI wrapper** - Ensure C bindings work
3. **Test C# bindings** - Verify marshalling and memory management
4. **Integration test** - Full Luanti → C# pipeline

## Memory Management Considerations

### C++/CLI
- Managed objects (`ref class`) use garbage collection
- Native objects need explicit deletion in destructor
- Use `delete` in destructor, `delete[]` for arrays

### P/Invoke
- Structs are stack-allocated by default
- Use `Marshal.AllocHGlobal` for heap allocation
- Always call `Marshal.FreeHGlobal` to prevent leaks
- Use `using` statements for automatic cleanup

## Performance Considerations

### Frame Data Transfer

For 256×256 frames at 60 FPS:
- **C++/CLI:** ~50-100 μs per frame (direct copy)
- **P/Invoke:** ~100-200 μs per frame (marshalling overhead)

Both are fast enough for real-time RL training.

### Optimization Tips

1. **Reuse buffers:** Don't allocate new arrays each frame
2. **Unsafe code:** Use `unsafe` and `fixed` for zero-copy marshalling
3. **Span<T>:** Use `Span<byte>` for efficient array operations
4. **Parallel processing:** Use `Parallel.For` for batch processing

## Example: ML.NET Integration

```csharp
using Tracking.Network;
using Microsoft.ML;
using Microsoft.ML.Transforms.Image;

var client = new NetworkClient();
client.Connect("127.0.0.1", 7001, 8000);

var mlContext = new MLContext();

while (training) {
    var frame = client.WaitForFrame(5000);
    if (frame != null) {
        // Convert frame to ML.NET ImageDataViewType
        var imageData = ConvertToImageData(frame.Data);

        // Run inference
        var prediction = model.Predict(imageData);

        // Send action back
        // client.SendAction(prediction.Action);
    }
}
```

## Next Steps

1. Choose C++/CLI (Windows) or P/Invoke (cross-platform)
2. Implement wrapper for `NetworkClient` class
3. Add wrappers for `GazeCommand` and `RewardInfo`
4. Create NuGet package for distribution
5. Write integration tests with actual Luanti instance
6. Document API with XML comments for IntelliSense

## References

- C++/CLI Documentation: https://learn.microsoft.com/en-us/cpp/dotnet/
- P/Invoke Tutorial: https://learn.microsoft.com/en-us/dotnet/standard/native-interop/pinvoke
- NetworkClient C++ Header: `/home/robert/CodeProjects/visionlearningpipeline/core/include/network_client.h`
- Luanti Integration: `/home/robert/CodeProjects/luanti-tracking/NETWORK_MODE.md`

# KV Store Benchmark Client

This package contains everything needed to run the benchmark client on a second laptop.

## Prerequisites
- gRPC and Protobuf (C++ development libraries)
- C++17 compiler (g++)

## How to use

1.  **Copy this folder** to your second laptop.
2.  **Compile the client** (if not already compiled or if on a different architecture):
    ```bash
    chmod +x compile_client.sh
    ./compile_client.sh
    ```
3.  **Run the benchmark**:
    ```bash
    chmod +x run_client.sh
    ./run_client.sh <THREADS> <SECONDS> <SERVER_IP> [PAYLOAD_BYTES]
    ```

### Example
To hit the server at `192.168.0.109` with 400 threads for 60 seconds:
```bash
./run_client.sh 400 60 192.168.0.109 64
```

### Server IP
The server IP should be the LAN address of the main computer (e.g., `192.168.0.109`). Ensure they are on the same network.

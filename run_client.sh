#!/usr/bin/env bash
# run_client.sh — Simple wrapper to run the load_client

if [ "$#" -lt 3 ]; then
    echo "Usage: ./run_client.sh <THREADS> <SECONDS> <SERVER_IP> [PAYLOAD_BYTES]"
    echo "Example: ./run_client.sh 400 60 192.168.1.10"
    exit 1
fi

THREADS=$1
SECONDS=$2
SERVER_IP=$3
PAYLOAD=${4:-64}

chmod +x ./load_client
./load_client "$THREADS" "$SECONDS" "$SERVER_IP" "$PAYLOAD"

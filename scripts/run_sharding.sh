#!/bin/bash
echo "Starting the Exchange Gateway with 4-Thread SO_REUSEPORT Sharding..."
./build/hft_engine/exchange &
EXCHANGE_PID=$!

sleep 2

echo "Starting the Trading Firm Simulator..."
./build/hft-trading-firm/trading_firm

kill $EXCHANGE_PID
echo "Benchmark Complete."

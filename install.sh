#!/bin/bash

# Installer script for GateServer/GameServer dependencies
# This script installs Boost, spdlog, and protobuf libraries on Ubuntu/Debian-based systems

echo "Updating package list..."
sudo apt update

echo "Installing Boost libraries..."
sudo apt install -y libboost-all-dev

echo "Installing spdlog..."
sudo apt install -y libspdlog-dev

echo "Installing protobuf compiler and runtime..."
sudo apt install -y protobuf-compiler libprotobuf-dev

echo "Installation completed!"
echo "You may need to restart VS Code or reload the window for IntelliSense to pick up the new libraries."

#!/bin/bash

# Installer script for GateServer dependencies
# This script installs Boost and spdlog libraries on Ubuntu/Debian-based systems

echo "Updating package list..."
sudo apt update

echo "Installing Boost libraries..."
sudo apt install -y libboost-all-dev

echo "Installing spdlog..."
sudo apt install -y libspdlog-dev

echo "Installation completed!"
echo "You may need to restart VS Code or reload the window for IntelliSense to pick up the new libraries."
#!/bin/bash
echo "Starting Flock Drive for Laptop/Surface..."
echo "Ensure you are running as root if you want to capture WiFi packets!"

# Check for virtual environment
if [ -d "venv" ]; then
    source venv/bin/activate
else
    echo "Warning: No 'venv' found. Running with system python."
fi

# Set PYTHONPATH
export PYTHONPATH=$PYTHONPATH:$(pwd)

# Run with python (add sudo if needed for packet capture, but better to let user handle that)
# Defaulting to no GPIO since it's a laptop
# Using --no-ble by default if on a device that might not support it well, but Surface RT might.
# We will enable both by default and let them fail gracefully.
python3 flock_drive/main.py --web-port 5000 "$@"

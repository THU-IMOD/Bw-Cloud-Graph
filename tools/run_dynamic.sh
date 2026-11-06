#!/bin/bash

# Script to run dynamic algorithm experiments across multiple datasets
# Datasets: rn, yt, lj, dg

# Define datasets
DATASETS=("rn" "yt" "lj" "dg")

# Executable path
EXECUTABLE="./build/bin/dynamic_algorithms"

# Check if executable exists
if [ ! -f "$EXECUTABLE" ]; then
    echo "Error: Executable not found at $EXECUTABLE"
    exit 1
fi

# Create logs directory if it doesn't exist
mkdir -p logs

# Print experiment configuration
echo "=========================================="
echo "Starting Dynamic Algorithm Experiments"
echo "=========================================="
echo "Datasets: ${DATASETS[@]}"
echo "Executable: $EXECUTABLE"
echo "=========================================="
echo ""

# Loop through each dataset
for dataset in "${DATASETS[@]}"; do
    CONFIG_FILE="./config/${dataset}.yaml"

    # Check if config file exists
    if [ ! -f "$CONFIG_FILE" ]; then
        echo "Warning: Config file not found: $CONFIG_FILE"
        echo "Skipping dataset: $dataset"
        echo ""
        continue
    fi

    echo "----------------------------------------"
    echo "Processing dataset: $dataset"
    echo "Config file: $CONFIG_FILE"
    echo "Command: $EXECUTABLE $CONFIG_FILE"
    echo "----------------------------------------"

    # Run the experiment
    $EXECUTABLE $CONFIG_FILE

    # Check if the command succeeded
    if [ $? -eq 0 ]; then
        echo "✓ Experiment completed successfully"
    else
        echo "✗ Experiment failed with exit code $?"
    fi

    echo ""

    # Optional: Add a small delay between experiments
    sleep 2
done

echo "=========================================="
echo "All Experiments Completed"
echo "=========================================="
echo "Results are saved in ./logs/ directory"
echo ""

# Optional: List generated log files
echo "Generated log files:"
ls -lht logs/htap-algo-*.log 2>/dev/null | head -10 || echo "No log files found"

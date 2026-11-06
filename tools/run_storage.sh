#!/bin/bash

# Script to run storage analysis across multiple datasets
# Datasets: rn, yt, lj, ot, dg, it, uk

# Define datasets
DATASETS=("rn" "yt" "lj" "ot" "dg" "it" "uk")

# Executable path
EXECUTABLE="./build/bin/storage"

# Check if executable exists
if [ ! -f "$EXECUTABLE" ]; then
    echo "Error: Executable not found at $EXECUTABLE"
    exit 1
fi

# Create logs directory if it doesn't exist
mkdir -p logs

# Print experiment configuration
echo "=========================================="
echo "Starting Storage Analysis Experiments"
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
        echo "✓ Storage analysis completed successfully"
    else
        echo "✗ Storage analysis failed with exit code $?"
    fi

    echo ""
done

echo "=========================================="
echo "All Storage Analyses Completed"
echo "=========================================="
echo "Results are saved in ./logs/ directory"
echo ""

# List generated log files
echo "Generated log files:"
ls -lht logs/*-storage-*.log 2>/dev/null | head -20 || echo "No log files found"

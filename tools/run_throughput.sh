#!/bin/bash

# Script to run throughput experiments across multiple datasets and insertion modes
# Datasets: rn08, yt08
# Insertion modes: 0 (sequential), 1 (random), 2 (zipfian)

# Define datasets and modes
DATASETS=("uk08")
MODES=(0 1 2)
MODE_NAMES=("sequential" "random" "zipfian")
ZIPFIAN_ALPHA="${ZIPFIAN_ALPHA:-0.8}"
ZIPFIAN_MAX_EDGES_PER_SOURCE="${ZIPFIAN_MAX_EDGES_PER_SOURCE:-10000}"

# Executable path
EXECUTABLE="./build/bin/throughput"

# Check if executable exists
if [ ! -f "$EXECUTABLE" ]; then
    echo "Error: Executable not found at $EXECUTABLE"
    exit 1
fi

# Create logs directory if it doesn't exist
mkdir -p logs

# Print experiment configuration
echo "=========================================="
echo "Starting Throughput Experiments"
echo "=========================================="
echo "Datasets: ${DATASETS[@]}"
echo "Modes: ${MODES[@]} (${MODE_NAMES[@]})"
echo "Executable: $EXECUTABLE"
echo "Zipfian alpha: $ZIPFIAN_ALPHA"
echo "Zipfian max edges per source: $ZIPFIAN_MAX_EDGES_PER_SOURCE"
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
    
    echo "Processing dataset: $dataset"
    echo "Config file: $CONFIG_FILE"
    echo ""
    
    # Loop through each insertion mode
    for i in "${!MODES[@]}"; do
        mode="${MODES[$i]}"
        mode_name="${MODE_NAMES[$i]}"
        
        echo "----------------------------------------"
        echo "Running: Dataset=$dataset, Mode=$mode_name ($mode)"
        if [ "$mode" -eq 2 ]; then
            echo "Command: $EXECUTABLE $CONFIG_FILE $mode $ZIPFIAN_ALPHA $ZIPFIAN_MAX_EDGES_PER_SOURCE"
        else
            echo "Command: $EXECUTABLE $CONFIG_FILE $mode"
        fi
        echo "----------------------------------------"
        
        # Run the experiment
        if [ "$mode" -eq 2 ]; then
            $EXECUTABLE $CONFIG_FILE $mode $ZIPFIAN_ALPHA $ZIPFIAN_MAX_EDGES_PER_SOURCE
        else
            $EXECUTABLE $CONFIG_FILE $mode
        fi
        
        # Check if the command succeeded
        if [ $? -eq 0 ]; then
            echo "✓ Experiment completed successfully"
        else
            echo "✗ Experiment failed with exit code $?"
        fi
        
        echo ""
        
        # Optional: Add a small delay between experiments
        sleep 1
    done
    
    echo "Finished processing dataset: $dataset"
    echo ""
done

echo "=========================================="
echo "All Experiments Completed"
echo "=========================================="
echo "Results are saved in ./logs/ directory"
echo ""

# Optional: List generated log files
echo "Generated log files:"
ls -lh logs/throughput-*.log 2>/dev/null || echo "No log files found"

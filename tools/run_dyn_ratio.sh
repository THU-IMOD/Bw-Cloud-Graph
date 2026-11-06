#!/bin/bash

# Configuration
BINARY="./build/bin/dynamic_algorithms"
DATASETS=("ot07" "ot09")

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored messages
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_header() {
    echo -e "${BLUE}[HEADER]${NC} $1"
}

# Check if binary exists
if [ ! -f "$BINARY" ]; then
    print_error "Binary not found: $BINARY"
    exit 1
fi

# Main experiment loop
print_header "=========================================="
print_header "Starting HTAP Algorithm Experiments"
print_header "=========================================="
print_info "Binary: $BINARY"
print_info "Datasets: ${DATASETS[@]}"
echo ""

total_experiments=${#DATASETS[@]}
current_experiment=0

for dataset in "${DATASETS[@]}"; do
    current_experiment=$((current_experiment + 1))
    
    config_file="./config/${dataset}.yaml"
    
    # Check if config file exists
    if [ ! -f "$config_file" ]; then
        print_warning "Config file not found: $config_file, skipping..."
        continue
    fi
    
    print_header "=========================================="
    print_info "Experiment [$current_experiment/$total_experiments]"
    print_info "Dataset: $dataset"
    print_info "Config: $config_file"
    print_header "=========================================="
    
    # Run the experiment
    $BINARY $config_file
    
    exit_code=$?
    
    if [ $exit_code -eq 0 ]; then
        print_info "Experiment completed successfully"
    else
        print_error "Experiment failed with exit code: $exit_code"
    fi
    
    echo ""
    
    # Optional: add a small delay between experiments
    if [ $current_experiment -lt $total_experiments ]; then
        print_info "Waiting 2 seconds before next experiment..."
        sleep 2
    fi
done

print_header "=========================================="
print_info "All experiments completed!"
print_info "Check ./logs/ directory for results"
print_header "=========================================="
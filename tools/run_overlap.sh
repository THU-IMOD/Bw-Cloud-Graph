#!/bin/bash

# Configuration
BINARY="./build/bin/overlap"
DATASETS=("lj06" "ot06")
OVERLAP_RATES=(0.1 0.2 0.3 0.4 0.5)

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
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

# Check if binary exists
if [ ! -f "$BINARY" ]; then
    print_error "Binary not found: $BINARY"
    exit 1
fi

# Main experiment loop
print_info "Starting overlap experiments..."
print_info "Datasets: ${DATASETS[@]}"
print_info "Overlap rates: ${OVERLAP_RATES[@]}"
echo ""

total_experiments=$((${#DATASETS[@]} * ${#OVERLAP_RATES[@]}))
current_experiment=0

for dataset in "${DATASETS[@]}"; do
    config_file="./config/${dataset}.yaml"
    
    # Check if config file exists
    if [ ! -f "$config_file" ]; then
        print_warning "Config file not found: $config_file, skipping..."
        continue
    fi
    
    print_info "=========================================="
    print_info "Running experiments for dataset: $dataset"
    print_info "=========================================="
    
    for overlap_rate in "${OVERLAP_RATES[@]}"; do
        current_experiment=$((current_experiment + 1))
        
        print_info "[$current_experiment/$total_experiments] Dataset: $dataset, Overlap rate: $overlap_rate"
        
        # Run the experiment
        $BINARY $config_file $overlap_rate
        
        exit_code=$?
        
        if [ $exit_code -eq 0 ]; then
            print_info "Experiment completed successfully"
        else
            print_error "Experiment failed with exit code: $exit_code"
        fi
        
        echo ""
        sleep 1
    done
done

print_info "=========================================="
print_info "All experiments completed!"
print_info "=========================================="
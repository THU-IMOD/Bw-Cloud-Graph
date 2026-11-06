#!/bin/bash

# Configuration
DELETE_BINARY="./build/bin/vertex_delete"
INSERT_BINARY="./build/bin/vertex_insert_batch"
DATASETS=("rn" "yt" "lj" "ot" "dg" "it" "uk")

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
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

print_task() {
    echo -e "${CYAN}[TASK]${NC} $1"
}

# Check if binaries exist
if [ ! -f "$DELETE_BINARY" ]; then
    print_error "Binary not found: $DELETE_BINARY"
    exit 1
fi

if [ ! -f "$INSERT_BINARY" ]; then
    print_error "Binary not found: $INSERT_BINARY"
    exit 1
fi

# Main experiment loop
print_header "=========================================="
print_header "Starting Vertex Delete/Insert Experiments"
print_header "=========================================="
print_info "Delete binary: $DELETE_BINARY"
print_info "Insert binary: $INSERT_BINARY"
print_info "Datasets: ${DATASETS[@]}"
echo ""

total_datasets=${#DATASETS[@]}
current_dataset=0

for dataset in "${DATASETS[@]}"; do
    current_dataset=$((current_dataset + 1))
    
    config_file="./config/${dataset}.yaml"
    
    # Check if config file exists
    if [ ! -f "$config_file" ]; then
        print_warning "Config file not found: $config_file, skipping..."
        continue
    fi
    
    print_header "=========================================="
    print_info "Dataset [$current_dataset/$total_datasets]: $dataset"
    print_info "Config: $config_file"
    print_header "=========================================="
    
    # Run vertex deletion
    print_task "Running vertex deletion..."
    $DELETE_BINARY $config_file
    
    delete_exit_code=$?
    
    if [ $delete_exit_code -eq 0 ]; then
        print_info "Vertex deletion completed successfully"
    else
        print_error "Vertex deletion failed with exit code: $delete_exit_code"
        print_warning "Skipping vertex insertion for this dataset"
        echo ""
        continue
    fi
    
    echo ""
    print_info "Waiting 2 seconds before insertion..."
    sleep 2
    
    # Run vertex insertion
    print_task "Running vertex insertion..."
    $INSERT_BINARY $config_file
    
    insert_exit_code=$?
    
    if [ $insert_exit_code -eq 0 ]; then
        print_info "Vertex insertion completed successfully"
    else
        print_error "Vertex insertion failed with exit code: $insert_exit_code"
    fi
    
    echo ""
    
    # Optional: add a delay between datasets
    if [ $current_dataset -lt $total_datasets ]; then
        print_info "Waiting 3 seconds before next dataset..."
        sleep 3
    fi
done

print_header "=========================================="
print_info "All experiments completed!"
print_info "Check ./logs/ directory for results"
print_header "=========================================="
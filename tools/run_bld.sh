#!/bin/bash

# Dataset list
# datasets=("rn" "yt")
datasets=("rn" "yt" "lj" "ot" "dg" "it" "uk")

# Iterate through each dataset
for dataset in "${datasets[@]}"; do
    echo "======================================"
    echo "Running dataset: $dataset"
    echo "======================================"
    
    ./build/bin/storage ./config/${dataset}.yaml
    
    if [ $? -eq 0 ]; then
        echo "Successfully completed: $dataset"
    else
        echo "Error occurred with dataset: $dataset"
    fi
    
    echo ""
done

echo "All datasets processed."